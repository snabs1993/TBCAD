#include "tbcad/ui/app_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "app_commands.h"
#include "tbcad/blocks/blocks.h"
#include "tbcad/blocks/index_refresh.h"
#include "tbcad/core/profile.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/ucs.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/geom/entity.h"
#include "tbcad/hatch/fill.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/platform/crash.h"
#include "tbcad/platform/file_lock.h"
#include "tbcad/platform/json.h"
#include "tbcad/platform/paths.h"
#include "tbcad/text/text.h"
#include "tbcad/tmpl/tmpl.h"
#include "tbcad/ui/customize.h"
#include "tbcad/ui/selection.h"
#include "tbcad/ui/settings_model.h"
#include "tbcad/xref/xref.h"

namespace tbcad::ui {

namespace fs = std::filesystem;

namespace {

constexpr double kZoomStep = 1.2;
constexpr std::size_t kMaxHistory = 4000;

std::vector<std::string> splitWords(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
            ++i;
        std::size_t start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t')
            ++i;
        if (i > start)
            out.emplace_back(text.substr(start, i - start));
    }
    return out;
}

bool isTrue(std::string_view v) {
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

}  // namespace

// Reports changes of the open document to the application.
class AppCore::AppObserver final : public DocumentObserver {
public:
    AppObserver(Document& doc, AppCore& app) : doc_(doc), app_(app) { doc_.addObserver(this); }
    ~AppObserver() override { doc_.removeObserver(this); }

    void onChanged(const ChangeSet& cs) override {
        if (!app_.session_)
            return;  // the program is closing: a running command is being undone
        app_.pruneSelection();
        if (!app_.inUndo_ && !cs.entities.empty() && cs.entities.size() <= 12) {
            std::vector<EntityId> added;
            for (const EntityChange& c : cs.entities) {
                if (c.before != kNoSnapshot || c.after == kNoSnapshot) {
                    added.clear();
                    break;
                }
                added.push_back(cs.idOf(c));
            }
            if (!added.empty())
                app_.flashNew(std::move(added));
        }
        if (!cs.outOfArea.empty() && !app_.inUndo_)
            app_.outOfArea_ = OutOfAreaNotice{cs.outOfArea};
    }

private:
    Document& doc_;
    AppCore& app_;
};

struct AppCore::Session {
    explicit Session(std::unique_ptr<Document> d) : docPtr(std::move(d)), doc(*docPtr) {}

    // Destroyed last (members below refer to it).
    std::unique_ptr<Document> docPtr;
    Document& doc;
    cmd::UndoStack undo{cmd::UndoStack::machineLimitBytes()};
    index::SpatialIndex index{};
    std::unique_ptr<index::DocumentIndexer> indexer{};
    // Declared after the indexer so it is destroyed first.
    std::unique_ptr<blocks::BlockIndexRefresher> blockRefresher{};
    std::unique_ptr<SceneSync> sync{};
    std::unique_ptr<AppObserver> observer{};
    std::unique_ptr<cmd::CommandRunner> runner{};
    fs::path file{};
    std::uint64_t savedRevision = 0;
    std::string readOnlyFormat{};  // "DWG" while the drawing came from a read-only format
    std::string openReportFormat{};
    std::string openReport{};  // DWGREPORT text of the reader's notices (foreign files)
    // The drawing file is marked as open here; the stamp is its modification time when this
    // window last read or wrote it, to notice that another program changed it meanwhile.
    std::unique_ptr<platform::FileLock> lock{};
    bool lockedElsewhere = false;
    std::optional<fs::file_time_type> diskStamp{};
    bool overwriteConfirmed = false;  // saved again after being told the file had changed
    // File stamp of each xref that a reload was already tried for (a failed reload is not retried
    // until the file changes again).
    std::map<BlockId, std::int64_t> xrefTried{};
    std::vector<std::string> xrefChanged{};  // as of the last check
    std::vector<std::string> xrefNotice{};   // for the balloon
    std::vector<AppCore::MissingXref> missingXrefs{};
    std::size_t xrefNoticeDismissedFor = 0;  // the notice was closed at this many missing xrefs
    // The current UCS (the "drafting.ucs" setting) of this drawing while it is in the background.
    std::optional<std::string> ucsSetting{};
    // SNAPANG and SNAPBASE ("angle" and "x,y" settings) of this drawing while it is in the
    // background.
    std::optional<std::pair<std::string, std::string>> snapSetting{};
};

struct AppCore::DocSlot {
    std::unique_ptr<Session> session;
    std::uint64_t id = 0;
    render::Camera2D camera{};  // the view while the drawing is in the background
    std::vector<EntityId> selection{};
    bool viewSaved = false;
    std::uint64_t autosavedRevision = ~std::uint64_t{0};  // revision last written to the autosave
    // An autosave being finished on another thread (the drawing was encoded when it started), and
    // the revision it holds.
    std::future<bool> autosaveJob{};
    std::uint64_t autosaveJobRevision = 0;
};

AppCore::AppCore(cmd::Registry& registry, platform::AppPaths paths)
    : registry_(registry), paths_(std::move(paths)), settings_(platform::Settings::withDefaults()) {
    if (auto r = settings_.load(paths_.settingsFile()); !r) {
        // Keep the damaged file for the user to look at: the next save would replace it.
        std::error_code ec;
        fs::path kept = paths_.settingsFile();
        kept += ".bad";
        fs::rename(paths_.settingsFile(), kept, ec);
        startupWarnings_.push_back(
            r.error().message + " (using default settings" +
            (ec ? std::string(")") : "; the file was kept as " + kept.filename().string() + ")"));
    }
    for (auto& w : settings_.repair())
        startupWarnings_.push_back(std::move(w));

    keybindings_.declare("file.new", "Ctrl+N");
    keybindings_.declare("file.open", "Ctrl+O");
    keybindings_.declare("file.save", "Ctrl+S");
    keybindings_.declare("file.saveAs", "Ctrl+Shift+S");
    keybindings_.declare("file.close", "Ctrl+W");
    keybindings_.declare("file.exit", "Ctrl+Q");  // as in AutoCAD
    keybindings_.declare("view.nextDocument", "Ctrl+Tab");
    keybindings_.declare("view.prevDocument", "Ctrl+Shift+Tab");
    keybindings_.declare("view.nextLayout", "Ctrl+PageDown");  // Model, Layout1, ... as in AutoCAD
    keybindings_.declare("view.prevLayout", "Ctrl+PageUp");
    keybindings_.declare("edit.undo", "Ctrl+Z");
    keybindings_.declare("edit.redo", "Ctrl+Y");
    keybindings_.declare("edit.selectAll", "Ctrl+A");
    keybindings_.declare("edit.deselect", "Escape");
    keybindings_.declare("edit.delete", "Delete");
    keybindings_.declare("view.zoomExtents", "");
    keybindings_.declare("drafting.snapAlt", "Ctrl+B");        // AutoCAD's second key for F9
    keybindings_.declare("view.toggleCommandLine", "Ctrl+9");  // as in AutoCAD
    keybindings_.declare("view.toggleProperties", "Ctrl+1");   // as in AutoCAD
    keybindings_.declare("view.commandPalette", "Ctrl+Shift+P");
    keybindings_.declare("view.cursorHud", "F12");
    keybindings_.declare("view.expandLog", "Ctrl+L");
    keybindings_.declare("view.textWindow", "F2");
    keybindings_.declare("view.cleanScreen", "F11");
    keybindings_.declare("view.cleanScreenAlt", "Ctrl+0");  // AutoCAD's chord for the same
    keybindings_.declare("help.quickStart", "F1");
    keybindings_.declare("tools.settings", "");
    if (auto r = keybindings_.load(paths_.keybindingsFile()); r) {
        for (auto& w : r.value())
            startupWarnings_.push_back(std::move(w));
    } else {
        startupWarnings_.push_back(r.error().message + " (using default key bindings)");
    }

    registerAppCommands(registry_);
    auto report = registry_.loadUserAliases(paths_.configDir / "aliases.json");
    for (const auto& e : report.errors)
        startupWarnings_.push_back(e);
    for (const auto& w : report.warnings)
        startupWarnings_.push_back(w);

    {
        std::string problem;
        auto doc = defaultDocument(&problem);
        if (!problem.empty())
            startupWarnings_.push_back(problem);
        installSession(std::move(doc), {});
    }
    for (const auto& w : startupWarnings_)
        print(w);
}

AppCore::~AppCore() {
    // The runners may hold coroutine frames that refer to the host and the documents.
    session_ = nullptr;
    slots_.clear();
}

namespace {

// Locks the session's file and records its modification time. Only our own formats are locked:
// a read-only DWG is never touched, and no lock file is left next to it.
template <class Sess>
void watchFile(Sess& s) {
    s.lock.reset();
    s.lockedElsewhere = false;
    s.diskStamp.reset();
    s.overwriteConfirmed = false;
    if (s.file.empty())
        return;
    std::string ext = s.file.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".tbcad" || ext == ".dxf") {
        s.lock = std::make_unique<platform::FileLock>(s.file);
        s.lockedElsewhere = s.lock->heldElsewhere();
        if (!s.lock->held())
            s.lock.reset();
    }
    std::error_code ec;
    const auto t = fs::last_write_time(s.file, ec);
    if (!ec)
        s.diskStamp = t;
}

// The file has a different modification time than when this window last read or wrote it.
template <class Sess>
bool changedOnDisk(const Sess& s) {
    if (!s.diskStamp || s.file.empty())
        return false;
    std::error_code ec;
    const auto t = fs::last_write_time(s.file, ec);
    return !ec && t != *s.diskStamp;
}

}  // namespace

namespace {

constexpr const char* kCurrentUcsKey = "tbcad.ucs.current";

constexpr const char* kSnapAngVar = "dxf.hv.$SNAPANG";    // DXF $SNAPANG, radians (group 50)
constexpr const char* kSnapBaseVar = "dxf.hv.$SNAPBASE";  // DXF $SNAPBASE (groups 10, 20)
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

// The snap angle (degrees) and snap base ("x,y") a drawing was saved with; defaults when absent.
std::pair<std::string, std::string> snapFromHeader(const Document& doc) {
    const auto& custom = doc.header().custom;
    std::pair<std::string, std::string> r{"0", "0,0"};
    auto field = [](const std::string& text, int code) -> std::optional<double> {
        const std::string tag = std::to_string(code) + "\t";
        std::size_t at = text.rfind(tag, 0) == 0 ? 0 : text.find("\n" + tag);
        if (at == std::string::npos)
            return std::nullopt;
        at += at == 0 && text.rfind(tag, 0) == 0 ? tag.size() : tag.size() + 1;
        const double v = std::strtod(text.c_str() + at, nullptr);
        return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
    };
    char buf[96];
    if (auto it = custom.find(kSnapAngVar); it != custom.end())
        if (auto a = field(it->second, 50)) {
            std::snprintf(buf, sizeof buf, "%.17g", *a / kDegToRad);
            r.first = buf;
        }
    if (auto it = custom.find(kSnapBaseVar); it != custom.end()) {
        const auto x = field(it->second, 10), y = field(it->second, 20);
        if (x && y) {
            std::snprintf(buf, sizeof buf, "%.17g,%.17g", *x, *y);
            r.second = buf;
        }
    }
    return r;
}

// The "drafting.ucs" setting a drawing was saved with (name TAB x TAB y TAB angle in the header).
std::string ucsFromHeader(const Document& doc) {
    auto it = doc.header().custom.find(kCurrentUcsKey);
    if (it == doc.header().custom.end())
        return "World";
    std::vector<std::string> f;
    std::size_t from = 0;
    for (std::size_t t; (t = it->second.find('\t', from)) != std::string::npos; from = t + 1)
        f.push_back(it->second.substr(from, t - from));
    f.push_back(it->second.substr(from));
    if (f.size() != 4)
        return "World";
    drafting::Ucs u;
    u.name = f[0];
    u.origin = {std::strtod(f[1].c_str(), nullptr), std::strtod(f[2].c_str(), nullptr)};
    u.angle = std::strtod(f[3].c_str(), nullptr);
    if (!u.name.empty())
        for (const drafting::Ucs& n : drafting::ucsList(doc))
            if (n.name == u.name)
                return u.name;
    return u.isWorld() ? "World" : drafting::unnamedUcsSetting(u);
}

// The slowest objects to measure, and the time by kind (TBCAD_INDEX_PROFILE).
void printIndexProfile(const Document& d, std::vector<std::pair<double, const Entity*>>& times) {
    double total = 0;
    std::map<int, std::pair<double, std::size_t>> byKind;
    for (const auto& [ms, e] : times) {
        total += ms;
        auto& k = byKind[static_cast<int>(kind(*e))];
        k.first += ms;
        ++k.second;
    }
    std::fprintf(stderr, "index profile: %zu objects, %.0f ms measuring (all threads)\n",
                 times.size(), total);
    for (const auto& [k, v] : byKind)
        if (v.first >= total * 0.01)
            std::fprintf(stderr, "  kind %-3d %8.0f ms  %zu objects\n", k, v.first, v.second);
    const std::size_t top = std::min<std::size_t>(20, times.size());
    std::partial_sort(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(top), times.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < top; ++i) {
        const Entity& e = *times[i].second;
        std::string what = "kind " + std::to_string(static_cast<int>(kind(e)));
        if (const auto* ins = entityAs<InsertData>(e)) {
            const BlockDef* b = d.block(ins->block);
            what = "insert of " + (b ? b->name : std::string("?")) +
                   (ins->attribs.empty() ? "" : " (attributes)") +
                   (std::abs(ins->rotation) > 1e-9 ? " (turned)" : "");
        } else if (const auto* h = entityAs<HatchData>(e)) {
            what = "hatch " + h->pattern + " with " + std::to_string(h->loops.size()) + " loops";
        }
        std::fprintf(stderr, "  %8.1f ms  #%llu %s\n", times[i].first,
                     static_cast<unsigned long long>(e.common.id.value), what.c_str());
    }
}

}  // namespace

std::unique_ptr<AppCore::Session> AppCore::makeSession(std::unique_ptr<Document> doc,
                                                       fs::path file) {
    auto s = std::make_unique<Session>(std::move(doc));
    Document& d = s->doc;
    // The new session can take the address of one just closed, at the same revision number: the
    // extents remembered for that one must not be taken for this one's.
    extentsCache_ = {};
    // TBCAD_INDEX_PROFILE=1: time each object's box and print the slowest (to find what makes a
    // drawing slow to open).
    struct IndexProfile {
        std::mutex lock;
        std::vector<std::pair<double, const Entity*>> times;
    };
    std::shared_ptr<IndexProfile> profile;
    if (const char* v = std::getenv("TBCAD_INDEX_PROFILE"); v && *v && *v != '0')
        profile = std::make_shared<IndexProfile>();
    s->indexer = std::make_unique<index::DocumentIndexer>(
        d, s->index,
        [&d, profile](const Entity& e) {
            if (!profile)
                return geom::bounds(e, d);
            const auto t0 = std::chrono::steady_clock::now();
            const BBox2 b = geom::bounds(e, d);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            std::lock_guard g(profile->lock);
            profile->times.emplace_back(ms, &e);
            return b;
        },
        [] { return std::make_shared<blocks::BoundsMemoScope>(); },
        [] {
            // Boxes are measured on several threads: each has its own block memo and fonts.
            struct Scope {
                blocks::BoundsMemoScope memo;
                text::FontScope fonts;
            };
            return std::make_shared<Scope>();
        });
    openStep("  index the objects");
    if (profile && !profile->times.empty())
        printIndexProfile(d, profile->times);
    s->blockRefresher = std::make_unique<blocks::BlockIndexRefresher>(d, *s->indexer);
    openStep("  index the blocks");
    s->sync = std::make_unique<SceneSync>(d, scene_);
    openStep("  scene link");
    s->observer = std::make_unique<AppObserver>(d, *this);
    s->runner = std::make_unique<cmd::CommandRunner>(d, s->undo, *this, registry_);
    s->runner->setFilterChain(&filterChain_);
    s->runner->setLineParser(lineParser_);
    s->undo.setMessageSink([this](std::string m) { print(m); });
    s->file = std::move(file);
    d.setFilePath(platform::pathToUtf8(s->file));
    s->savedRevision = d.revision();
    openStep("  command setup");
    watchFile(*s);
    openStep("  watch the file");
    if (s->lockedElsewhere)
        print(
            s->file.filename().string() +
            " is also open in another TBCAD window. Changes saved there may be overwritten here.");
    return s;
}

std::vector<std::string> AppCore::layoutNames() const {
    std::vector<std::string> out;
    if (!session_)
        return out;
    for (const layout::Layout& l : layout::layouts(session_->doc))
        out.push_back(l.name);
    return out;
}

void AppCore::leavePaper() {
    editingLayout_.clear();
    editingViewport_ = 0;
    if (activeLayout_.empty())
        return;
    lastLayout_ = activeLayout_;
    const int w = camera_.viewportW, h = camera_.viewportH;
    camera_ = modelCamera_;
    camera_.viewportW = w;
    camera_.viewportH = h;
    activeLayout_.clear();
    paper_.clear();
    paperIndex_.bulkLoad(std::span<const index::IdBox>{});
    selection_.clear();
    ++selectionRevision_;
    lastTessellatedFor_ = {};
    notifySessionHooks();
    settings_.setString("plot.layout", "");  // PLOT follows what is on screen
}

bool AppCore::setActiveLayout(const std::string& name) {
    if (!session_)
        return false;
    commitViewportView();
    if (name.empty() || !layout::findLayout(session_->doc, name)) {
        leavePaper();
        return name.empty();
    }
    if (runner().active())
        pressEscape();
    clearTransientState();
    const bool wasModel = activeLayout_.empty();
    if (wasModel)
        modelCamera_ = camera_;
    const std::string problem = paper_.build(session_->doc, name);
    if (!problem.empty()) {
        print(problem);
        if (!wasModel)
            leavePaper();
        return false;
    }
    activeLayout_ = name;
    settings_.setString("plot.layout", name);  // PLOT follows what is on screen
    paperRevision_ = session_->doc.revision();
    rebuildPaperIndex();
    notifySessionHooks();
    // The sheet and whatever is drawn on it: some drawings keep their title blocks and viewports
    // well away from the paper.
    BBox2 shown = paper_.sheet();
    shown.merge(paper_.scene().bounds());
    camera_.fit(shown);
    return true;
}

void AppCore::clearTransientState() {
    selection_.clear();
    ++selectionRevision_;
    outOfArea_.reset();
    drag_.reset();
    hasPreview_ = false;
    prompt_.reset();
    feedback_ = {};
    scene_.setSelected({});
    scene_.setHighlighted({});
    hoverId_ = {};
}

void AppCore::enterSlot(std::size_t index, bool fitView) {
    leavePaper();
    // Tiled viewports belong to the drawing on screen; another drawing starts with one.
    if (tiles_.size() > 1)
        setTileLayout("1");
    clearTransientState();
    if (session_ && session_ != slots_[index].session.get()) {
        session_->ucsSetting = settings_.getString(drafting::kUcsSettingKey, "World");
        session_->snapSetting =
            std::make_pair(settings_.getString(drafting::keys::snapAngle, "0"),
                           settings_.getString(drafting::keys::snapBase, "0,0"));
    }
    active_ = index;
    DocSlot& slot = slots_[index];
    session_ = slot.session.get();
    settings_.setString(drafting::kUcsSettingKey, session_->ucsSetting
                                                      ? *session_->ucsSetting
                                                      : ucsFromHeader(session_->doc));
    {
        const auto snap =
            session_->snapSetting ? *session_->snapSetting : snapFromHeader(session_->doc);
        settings_.setString(drafting::keys::snapAngle, snap.first);
        settings_.setString(drafting::keys::snapBase, snap.second);
    }
    if (slot.viewSaved && !fitView) {
        const int w = camera_.viewportW, h = camera_.viewportH;
        camera_ = slot.camera;
        camera_.viewportW = w;
        camera_.viewportH = h;
        selection_ = slot.selection;
        selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                        [&](EntityId id) { return !session_->doc.find(id); }),
                         selection_.end());
        ++selectionRevision_;
    }
    auto step = [&](const char* name) { openStep(name); };
    // Fit the view first: the scene is then built for the zoom the drawing is first seen at, not
    // built once and then built again when the zoom change makes curves and text stale.
    if (fitView && (!session_->index.empty() || !slot.viewSaved)) {
        zoomExtents();
        step("  zoom to fit (finds the extents)");
        fitPending_ = 6;
        fitUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    }
    session_->sync->setPixelSize(camera_.pixelSizeMm());
    session_->sync->setShowTransparency(settings_.getBool("view.transparency", true));
    const double tileSize = render::Scene::suggestTileSize(drawingExtents());
    step("  drawing extents");
    session_->sync->rebuildAll(tileSize);
    step("  build scene");
    scene_.setSelected(selection_);
    // PLOT follows what is on screen: a layout named for the drawing shown before means nothing
    // here.
    settings_.setString("plot.layout", activeLayout_);
    ++generation_;
    lastTessellatedFor_ = camera_;
    notifySessionHooks();
    step("  panels and hooks");
    scanMissingXrefs();
    step("  look for missing xrefs");
}

void AppCore::scanMissingXrefs() {
    session_->missingXrefs.clear();
    for (BlockId id : xref::xrefs(session_->doc)) {
        const BlockDef* def = session_->doc.block(id);
        if (!def)
            continue;
        // Not found on disk, whether the snapshot was taken (Missing) or never was (an import
        // that could not read the file leaves it unloaded).
        std::error_code ec;
        if (!fs::is_regular_file(xref::resolvePath(def->xrefPath, session_->file), ec))
            session_->missingXrefs.push_back({def->name, def->xrefPath});
    }
    if (session_->missingXrefs.size() != session_->xrefNoticeDismissedFor)
        session_->xrefNoticeDismissedFor = 0;
}

const std::vector<AppCore::MissingXref>& AppCore::missingXrefs() const {
    return session_->missingXrefs;
}

bool AppCore::xrefNoticeVisible() const {
    return session_ && !session_->missingXrefs.empty() &&
           session_->missingXrefs.size() != session_->xrefNoticeDismissedFor;
}

void AppCore::dismissXrefNotice() {
    if (session_)
        session_->xrefNoticeDismissedFor = session_->missingXrefs.size();
}

void AppCore::installSession(std::unique_ptr<Document> doc, fs::path file) {
    if (slots_.empty())
        slots_.emplace_back();
    DocSlot& slot = slots_[active_];
    session_ = nullptr;
    if (slot.session)
        removeAutosaveOf(slot.id);
    slot.session.reset();
    slot.session = makeSession(std::move(doc), std::move(file));
    slot.id = nextDocId_++;
    slot.viewSaved = false;
    slot.autosavedRevision = ~std::uint64_t{0};
    enterSlot(active_, true);
}

void AppCore::addSession(std::unique_ptr<Document> doc, fs::path file) {
    if (!slots_.empty() && isPristine()) {
        installSession(std::move(doc), std::move(file));
        return;
    }
    if (!slots_.empty()) {
        leavePaper();
        slots_[active_].camera = camera_;
        slots_[active_].selection = selection_;
        slots_[active_].viewSaved = true;
    }
    DocSlot slot;
    slot.session = makeSession(std::move(doc), std::move(file));
    slot.id = nextDocId_++;
    slots_.push_back(std::move(slot));
    // The vector may have moved the slots, but the sessions are heap objects.
    enterSlot(slots_.size() - 1, true);
}

bool AppCore::activateDocument(std::size_t index) {
    if (index >= slots_.size())
        return false;
    if (index == active_)
        return true;
    if (runner().active()) {
        print("Finish the current command first.");
        return false;
    }
    leavePaper();
    slots_[active_].camera = camera_;
    slots_[active_].selection = selection_;
    slots_[active_].viewSaved = true;
    enterSlot(index, false);
    return true;
}

void AppCore::closeDocument(std::size_t index) {
    if (index < slots_.size()) {
        const std::string id = std::to_string(slots_[index].id);
        const std::string layoutPrefix = "layout:" + id + ":";
        previews_.erase("doc:" + id);
        for (auto it = previews_.begin(); it != previews_.end();)
            it = it->first.compare(0, layoutPrefix.size(), layoutPrefix) == 0 ? previews_.erase(it)
                                                                              : std::next(it);
    }
    if (index >= slots_.size())
        return;
    if (index == active_ && runner().active())
        pressEscape();
    removeAutosaveOf(slots_[index].id);
    if (slots_.size() == 1) {
        installSession(defaultDocument(), {});
        return;
    }
    if (index == active_) {
        const std::size_t next = index + 1 < slots_.size() ? index + 1 : index - 1;
        std::unique_ptr<Session> dying = std::move(slots_[index].session);
        slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
        session_ = nullptr;  // enterSlot would store the UCS setting into the closed session
        dying.reset();
        enterSlot(next > index ? next - 1 : next, false);
    } else {
        slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
        if (index < active_)
            --active_;
    }
}

std::size_t AppCore::documentCount() const {
    return slots_.size();
}

std::string AppCore::documentTitle(std::size_t index) const {
    if (index >= slots_.size())
        return {};
    const Session& s = *slots_[index].session;
    std::string name = s.file.empty() ? std::string("Untitled") : s.file.filename().string();
    return s.readOnlyFormat.empty() ? name : name + " [Read-only]";
}

std::filesystem::path AppCore::documentFile(std::size_t index) const {
    return index < slots_.size() ? slots_[index].session->file : std::filesystem::path{};
}

std::vector<std::string> AppCore::documentLayouts(std::size_t index) const {
    std::vector<std::string> out;
    if (index >= slots_.size())
        return out;
    for (const layout::Layout& l : layout::layouts(slots_[index].session->doc))
        out.push_back(l.name);
    return out;
}

AppCore::SaveAllReport AppCore::saveAll() {
    SaveAllReport report;
    const std::size_t back = active_;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!documentModified(i))
            continue;
        const Session& s = *slots_[i].session;
        if (s.file.empty() || !s.readOnlyFormat.empty()) {
            report.needSaveAs.push_back(i);
            continue;
        }
        if (!activateDocument(i)) {
            report.failed.push_back(documentTitle(i) + ": a command is running");
            continue;
        }
        if (auto r = save(); r)
            ++report.saved;
        else
            report.failed.push_back(documentTitle(i) + ": " + r.error().message);
    }
    (void)activateDocument(back);
    return report;
}

bool AppCore::documentModified(std::size_t index) const {
    if (index >= slots_.size())
        return false;
    const Session& s = *slots_[index].session;
    return s.doc.revision() != s.savedRevision;
}

bool AppCore::anyDocumentModified() const {
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (documentModified(i))
            return true;
    return false;
}

std::uint64_t AppCore::documentId(std::size_t index) const {
    return index < slots_.size() ? slots_[index].id : 0;
}

const TabPreview& AppCore::documentPreview(std::size_t index) {
    static const TabPreview none;
    if (index >= slots_.size())
        return none;
    const Document& doc = slots_[index].session->doc;
    PreviewEntry& e = previews_["doc:" + std::to_string(slots_[index].id)];
    if (e.revision != doc.revision()) {
        e.preview = previewOfModel(doc);
        e.revision = doc.revision();
    }
    return e.preview;
}

const TabPreview& AppCore::layoutPreview(const std::string& layoutName) {
    static const TabPreview none;
    if (!session_ || active_ >= slots_.size())
        return none;
    PreviewEntry& e = previews_["layout:" + std::to_string(slots_[active_].id) + ":" + layoutName];
    if (e.revision != session_->doc.revision()) {
        e.preview = previewOfLayout(session_->doc, layoutName);
        e.revision = session_->doc.revision();
    }
    return e.preview;
}

bool AppCore::isPristine() const {
    return session_ && session_->file.empty() && !modified() &&
           session_->doc.modelSpaceCount() == 0;
}

std::optional<std::size_t> AppCore::slotOfFile(const fs::path& path) const {
    std::error_code ec;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const fs::path& f = slots_[i].session->file;
        if (f.empty())
            continue;
        if (fs::equivalent(f, path, ec) || f.lexically_normal() == path.lexically_normal())
            return i;
    }
    return std::nullopt;
}

Document& AppCore::document() {
    return session_->doc;
}
const Document& AppCore::document() const {
    return session_->doc;
}
const index::SpatialIndex& AppCore::spatialIndex() const {
    return inPaperSpace() ? paperIndex_ : session_->index;
}

BlockId AppCore::drawingOwner() const {
    if (!session_ || !inPaperSpace())
        return {};
    const auto lay = layout::findLayout(session_->doc, activeLayout_);
    return lay ? lay->content : BlockId{};
}

void AppCore::rebuildPaperIndex() {
    std::vector<index::IdBox> items;
    if (session_ && inPaperSpace()) {
        const Document& d = session_->doc;
        if (const BlockDef* b = d.block(drawingOwner())) {
            for (EntityId id : b->entities) {
                const Entity* e = d.find(id);
                if (!e)
                    continue;
                const BBox2 box = geom::bounds(*e, d);
                if (!box.empty())
                    items.emplace_back(id, box);
            }
        }
    }
    paperIndex_.bulkLoad(items);
}
cmd::UndoStack& AppCore::undoStack() {
    return session_->undo;
}
cmd::CommandRunner& AppCore::runner() {
    return *session_->runner;
}
bool AppCore::hasFile() const {
    return !session_->file.empty();
}
const fs::path& AppCore::file() const {
    return session_->file;
}
bool AppCore::modified() const {
    return session_->doc.revision() != session_->savedRevision;
}

// ---- recovery / shutdown -------------------------------------------------------------------

namespace {

Recovery readRecovery(const fs::path& tbcadFile) {
    Recovery r;
    r.autosaveFile = tbcadFile;
    {
        std::error_code ec;
        const auto sz = fs::file_size(tbcadFile, ec);
        if (!ec)
            r.sizeBytes = sz;
        ec.clear();
        const auto ft = fs::last_write_time(tbcadFile, ec);
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &t);
#else
            localtime_r(&t, &local);
#endif
            char buf[32];
            if (std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &local) > 0)
                r.savedAt = buf;
        }
    }
    fs::path meta = tbcadFile;
    meta.replace_extension(".json");
    std::ifstream in(meta, std::ios::binary);
    if (in) {
        std::ostringstream buf;
        buf << in.rdbuf();
        if (auto j = platform::parseJson(buf.str()); j && j.value().isObject())
            if (const auto* o = j.value().find("original"))
                r.originalPath = o->asString();
    }
    return r;
}

bool isRecoveryFile(const fs::path& p) {
    const std::string name = p.filename().string();
    return name.rfind("recovery", 0) == 0 &&
           (p.extension() == ".tbcad" || p.extension() == ".json");
}

}  // namespace

// Every unsaved drawing has its own pair of files, recovery-<n>.tbcad and recovery-<n>.json
// (a single recovery.tbcad from older versions is found too).
std::vector<Recovery> AppCore::pendingRecoveries() const {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(paths_.autosaveDir(), ec), end; !ec && it != end;
         it.increment(ec))
        if (it->path().extension() == ".tbcad" && isRecoveryFile(it->path()))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    std::vector<Recovery> out;
    for (const fs::path& f : files)
        out.push_back(readRecovery(f));
    return out;
}

std::optional<Recovery> AppCore::pendingRecovery() const {
    auto all = pendingRecoveries();
    if (all.empty())
        return std::nullopt;
    return all.front();
}

Result<void> AppCore::recover(const Recovery& r) {
    io::IoReport report;
    auto doc = io::readTbcad(r.autosaveFile, report);
    if (!doc)
        return doc.error();
    installSession(std::move(doc.value()), platform::pathFromUtf8(r.originalPath));
    session_->savedRevision = ~std::uint64_t{0};  // unsaved: the recovered content is not on disk
    print("Recovered the drawing from the last autosave. Save it to keep the changes.");
    return {};
}

Result<void> AppCore::recoverAll() {
    const auto all = pendingRecoveries();
    std::optional<Error> firstError;
    std::size_t done = 0;
    for (const Recovery& r : all) {
        if (done == 0) {
            if (auto res = recover(r); !res) {
                firstError = res.error();
                continue;
            }
        } else {
            io::IoReport report;
            auto doc = io::readTbcad(r.autosaveFile, report);
            if (!doc) {
                firstError = doc.error();
                continue;
            }
            addSession(std::move(doc.value()), platform::pathFromUtf8(r.originalPath));
            session_->savedRevision = ~std::uint64_t{0};
        }
        ++done;
    }
    // The leftovers are replaced by autosaves of the recovered drawings.
    removeAutosaveFiles();
    autosaveNow();
    if (done == 0 && firstError)
        return *firstError;
    if (done > 1)
        print("Recovered " + std::to_string(done) + " drawings from the last autosave.");
    return {};
}

void AppCore::discardRecovery() {
    removeAutosaveFiles();
}

std::vector<Recovery> AppCore::leftoverRecoveries() const {
    std::vector<Recovery> out;
    for (Recovery& r : pendingRecoveries()) {
        bool own = false;
        for (const DocSlot& slot : slots_)
            own = own || r.autosaveFile.filename() ==
                             fs::path("recovery-" + std::to_string(slot.id) + ".tbcad");
        if (!own)
            out.push_back(std::move(r));
    }
    return out;
}

std::vector<Recovery> AppCore::backupFiles() const {
    std::vector<fs::path> drawings;
    for (const DocSlot& slot : slots_)
        if (slot.session && !slot.session->file.empty())
            drawings.push_back(slot.session->file);
    for (const std::string& f : recentFiles())
        drawings.push_back(platform::pathFromUtf8(f));
    std::vector<Recovery> out;
    std::set<fs::path> seen;
    for (const fs::path& d : drawings) {
        fs::path bak = d;
        bak += ".bak";
        std::error_code ec;
        if (!seen.insert(bak.lexically_normal()).second || !fs::is_regular_file(bak, ec))
            continue;
        Recovery r = readRecovery(bak);
        r.originalPath = platform::pathToUtf8(d);
        out.push_back(std::move(r));
    }
    return out;
}

Result<void> AppCore::openRecovered(const Recovery& r, bool removeAfter) {
    std::string ext = platform::pathFromUtf8(r.originalPath).extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!r.originalPath.empty() && ext != ".tbcad" && ext != ".tbt" &&
        r.autosaveFile.extension() == ".bak")
        return makeError(ErrorCode::Unsupported,
                         "Only backups of .tbcad drawings open here: rename " +
                             platform::pathToUtf8(r.autosaveFile) + " and open it.");
    io::IoReport report;
    auto doc = io::readTbcad(r.autosaveFile, report);
    if (!doc)
        return doc.error();
    addSession(std::move(doc.value()), {});
    session_->savedRevision = ~std::uint64_t{0};
    const std::string what =
        r.originalPath.empty()
            ? std::string("an unsaved drawing")
            : platform::pathToUtf8(platform::pathFromUtf8(r.originalPath).filename());
    print("Opened " +
          std::string(r.autosaveFile.extension() == ".bak" ? "the backup" : "the autosave") +
          " of " + what + " as a new drawing. Save it under a name to keep it.");
    if (removeAfter) {
        std::error_code ec;
        fs::path meta = r.autosaveFile;
        meta.replace_extension(".json");
        fs::remove(r.autosaveFile, ec);
        fs::remove(meta, ec);
    }
    return {};
}

// Collects a finished background autosave; with `wait` also waits for one still running.
void AppCore::reapAutosave(DocSlot& slot, bool wait) {
    if (!slot.autosaveJob.valid())
        return;
    if (!wait && slot.autosaveJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    if (slot.autosaveJob.get())
        slot.autosavedRevision = slot.autosaveJobRevision;
}

void AppCore::removeAutosaveFiles() {
    for (DocSlot& slot : slots_)
        reapAutosave(slot, true);
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(paths_.autosaveDir(), ec), end; !ec && it != end;
         it.increment(ec))
        if (isRecoveryFile(it->path()))
            files.push_back(it->path());
    for (const fs::path& f : files)
        fs::remove(f, ec);
}

void AppCore::removeAutosaveOf(std::uint64_t slotId) {
    for (DocSlot& slot : slots_)
        if (slot.id == slotId && slot.autosaveJob.valid())
            (void)slot.autosaveJob.get();  // finished, and its result no longer counts
    std::error_code ec;
    const std::string base = "recovery-" + std::to_string(slotId);
    fs::remove(paths_.autosaveDir() / (base + ".tbcad"), ec);
    fs::remove(paths_.autosaveDir() / (base + ".json"), ec);
}

void AppCore::shutdown() {
    if (settings_.dirty())
        (void)settings_.save(paths_.settingsFile());
    removeAutosaveFiles();
}

// Starts the autosave of one drawing: a snapshot of the drawing is taken now and the rest
// (encode, deflate, write, sync, rename) runs on other threads, so the window does not freeze on
// a big drawing.
bool AppCore::startAutosave(DocSlot& slot) {
    Session& ses = *slot.session;
    if (slot.autosaveJob.valid() || ses.doc.revision() == ses.savedRevision ||
        slot.autosavedRevision == ses.doc.revision())
        return false;
    if (!platform::ensureDirectory(paths_.autosaveDir()))
        return false;
    const std::string base = "recovery-" + std::to_string(slot.id);
    io::WriteOptions opt;
    opt.keepBackup = false;
    std::future<Result<void>> written =
        io::writeTbcadAsync(ses.doc.snapshot(), paths_.autosaveDir() / (base + ".tbcad"), opt);
    slot.autosaveJobRevision = ses.doc.revision();
    const fs::path meta = paths_.autosaveDir() / (base + ".json");
    const std::string original = platform::pathToUtf8(ses.file);
    slot.autosaveJob = std::async(
        std::launch::async, [written = std::move(written), meta, original]() mutable -> bool {
            if (!written.get())
                return false;
            platform::Json::Object o;
            o["original"] = original;
            // Written beside and renamed over, so a kill in the middle never leaves half a file.
            fs::path tmp = meta;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << platform::toJson(platform::Json(std::move(o)));
            }
            std::error_code ec;
            fs::rename(tmp, meta, ec);
            if (ec)
                fs::remove(tmp, ec);
            return true;
        });
    return true;
}

bool AppCore::autosaveNow() {
    bool wrote = false;
    for (DocSlot& slot : slots_) {
        reapAutosave(slot, true);
        if (!startAutosave(slot))
            continue;
        reapAutosave(slot, true);
        wrote = wrote || slot.autosavedRevision == slot.autosaveJobRevision;
    }
    return wrote;
}

void AppCore::waitForAutosave() {
    for (DocSlot& slot : slots_)
        reapAutosave(slot, true);
}

void AppCore::autosaveInBackground() {
    for (DocSlot& slot : slots_) {
        reapAutosave(slot, false);
        (void)startAutosave(slot);
    }
}

void AppCore::tickAutosave(std::chrono::steady_clock::time_point now) {
    // Changed settings reach settings.json within a moment, not only at a clean exit, so a crash
    // or a kill does not lose them.
    if (settings_.dirty()) {
        if (!settingsDirtySeen_) {
            settingsDirtySeen_ = true;
            settingsDirtyAt_ = now;
        } else if (now - settingsDirtyAt_ >= std::chrono::seconds(1)) {
            (void)settings_.save(paths_.settingsFile());
            settingsDirtySeen_ = false;
        }
    } else {
        settingsDirtySeen_ = false;
    }
    for (DocSlot& slot : slots_)
        reapAutosave(slot, false);
    if (!autosaveClockStarted_) {
        lastAutosave_ = now;
        autosaveClockStarted_ = true;
        return;
    }
    if (!settings_.getBool("autosave.enabled", true))
        return;
    // Crash journal: the first change after a save or open is written out within about 30 s, so
    // a crash or kill soon after starting to draw still leaves something to recover.
    {
        bool fresh = false;
        for (const DocSlot& slot : slots_) {
            const Session& ses = *slot.session;
            if (ses.doc.revision() != ses.savedRevision &&
                (slot.autosavedRevision == ~std::uint64_t{0} ||
                 slot.autosavedRevision < ses.savedRevision))
                fresh = true;
        }
        if (!fresh) {
            quickJournalPending_ = false;
        } else if (!quickJournalPending_) {
            quickJournalPending_ = true;
            quickJournalAt_ = now;
        } else if (now - quickJournalAt_ >= std::chrono::seconds(30)) {
            quickJournalPending_ = false;
            lastAutosave_ = now;
            autosaveInBackground();
            return;
        }
    }
    double minutes = std::max(0.1, settings_.getNumber("autosave.minutes", 10));
    if (now - lastAutosave_ < std::chrono::duration<double, std::ratio<60>>(minutes))
        return;
    lastAutosave_ = now;
    autosaveInBackground();
}

// ---- files -------------------------------------------------------------------------------------

std::unique_ptr<Document> AppCore::defaultDocument(std::string* problem) {
    const std::string name = settings_.getString("template.default", "");
    if (tmpl::isBuiltinTemplate(name))
        return tmpl::builtinTemplate(name);
    if (!name.empty()) {
        std::string why;
        const fs::path file = templateFile(name);
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            why = "No template named \"" + name + "\".";
        } else if (auto doc = readTemplateFile(file, why)) {
            return doc;
        }
        if (problem)
            *problem = "Default template \"" + name + "\": " + why + " Using the built-in drawing.";
    }
    return tmpl::builtinTemplate();
}

void AppCore::newDocument() {
    std::string problem;
    auto doc = defaultDocument(&problem);
    if (!problem.empty())
        print(problem);
    addSession(std::move(doc), {});
}

std::string AppCore::exportKeybindings(const fs::path& file) {
    return KeybindingEditor(keybindings_, paths_.keybindingsFile()).exportTo(file);
}

std::string AppCore::importKeybindings(const fs::path& file) {
    std::vector<std::string> warnings;
    auto out = KeybindingEditor(keybindings_, paths_.keybindingsFile()).importFrom(file, &warnings);
    for (const std::string& w : warnings)
        print("Key bindings: " + w);
    if (!out.ok)
        return out.error;
    print("Key bindings imported from " + platform::pathToUtf8(file));
    return {};
}

static std::string lowerExtension(const fs::path& p);

std::vector<std::string> AppCore::templateNames() const {
    std::vector<std::string> out{tmpl::kBuiltinTemplate, tmpl::kBuiltinInchTemplate};
    for (const auto& t : tmpl::listTemplates(templateFolder()))
        out.push_back(t.name);
    // DXF and DWT files dropped into the template folder are templates too; they keep their
    // extension in the name so ACME.dxf and ACME.tbt can sit side by side.
    std::error_code ec;
    std::vector<std::string> foreign;
    for (fs::directory_iterator it(templateFolder(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string e = lowerExtension(it->path());
        if ((e == ".dxf" || e == ".dwt") && it->is_regular_file(ec))
            foreign.push_back(platform::pathToUtf8(it->path().filename()));
    }
    std::sort(foreign.begin(), foreign.end());
    out.insert(out.end(), foreign.begin(), foreign.end());
    return out;
}

fs::path AppCore::templateFile(std::string_view name) const {
    const fs::path given = platform::pathFromUtf8(std::string(name));
    const std::string e = lowerExtension(given);
    if (e == ".dxf" || e == ".dwt" || e == ".tbt") {
        // A full path, or a file in the template folder.
        if (given.is_absolute() || given.has_parent_path())
            return given;
        return templateFolder() / given;
    }
    return templateFolder() / platform::pathFromUtf8(tmpl::sanitizeTemplateName(std::string(name)) +
                                                     tmpl::kTemplateExtension);
}

std::unique_ptr<Document> AppCore::readTemplateFile(const fs::path& file, std::string& why) {
    const std::string e = lowerExtension(file);
    if (e == ".dxf") {
        std::ifstream in(platform::longPath(file), std::ios::binary);
        if (!in) {
            why = "Cannot read the template: " + file.string();
            return nullptr;
        }
        in.seekg(0, std::ios::end);
        std::vector<std::byte> bytes(
            static_cast<std::size_t>(std::max<std::streamoff>(in.tellg(), 0)));
        in.seekg(0, std::ios::beg);
        if (!bytes.empty())
            in.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        dxf::Report report;
        auto r = dxf::importDxf(bytes, {}, report);
        if (!r) {
            why = "Cannot read the template: " + r.error().message;
            return nullptr;
        }
        return std::move(r.value().document);
    }
    if (e == ".dwt") {
        // A DWT is a DWG with another extension.
        auto it = foreignReaders_.find(".dwg");
        if (it == foreignReaders_.end()) {
            why = "DWT templates need the DWG reader, which is not available.";
            return nullptr;
        }
        auto r = it->second.read(file);
        if (!r) {
            why = "Cannot read the template: " + r.error().message;
            return nullptr;
        }
        return std::move(r.value().document);
    }
    auto doc = tmpl::loadTemplate(file);
    if (!doc) {
        why = "Cannot read the template: " + doc.error().message;
        return nullptr;
    }
    return std::move(doc.value());
}

std::string AppCore::newFromTemplate(std::string_view name) {
    if (name.empty() || tmpl::isBuiltinTemplate(name)) {
        addSession(name.empty() ? tmpl::builtinTemplate() : tmpl::builtinTemplate(name), {});
        return {};
    }
    const fs::path file = templateFile(name);
    std::error_code ec;
    if (!fs::exists(file, ec))
        return "No template named \"" + std::string(name) + "\".";
    std::string why;
    auto doc = readTemplateFile(file, why);
    if (!doc)
        return why;
    // The template stays as it is: the new drawing is untitled.
    addSession(std::move(doc), {});
    return {};
}

std::string AppCore::saveAsTemplate(std::string_view name) {
    const std::string clean = tmpl::sanitizeTemplateName(std::string(name));
    if (tmpl::isBuiltinTemplate(name) || tmpl::isBuiltinTemplate(clean))
        return "\"" + std::string(name) + "\" is reserved; choose another name.";
    auto r = tmpl::saveTemplate(document(), templateFolder() / platform::pathFromUtf8(clean));
    if (!r)
        return r.error().message;
    print("Template saved: " + platform::pathToUtf8(r.value()));
    return {};
}

namespace {

bool isDxfPath(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".dxf";
}

}  // namespace

void AppCore::printOpenFailure(const std::string& text) {
    if (echoOpenFailures_)
        print(text);
}

void AppCore::openLap(const std::string& name) {
    const auto t = std::chrono::steady_clock::now();
    openTimings_.emplace_back(name,
                              std::chrono::duration<double, std::milli>(t - openLapAt_).count());
    openLapAt_ = t;
    if (openStepAt_)
        openStepAt_ = t;
}

void AppCore::openStep(const char* name) {
    if (!openStepAt_)
        return;
    const auto t = std::chrono::steady_clock::now();
    openTimings_.emplace_back(name,
                              std::chrono::duration<double, std::milli>(t - *openStepAt_).count());
    openStepAt_ = t;
}

Result<void> AppCore::openDxf(const fs::path& path) {
    auto lap = [&](const char* name) { openLap(name); };
    std::error_code isDirEc;
    std::ifstream in(platform::longPath(path), std::ios::binary);
    if (!in || fs::is_directory(platform::longPath(path), isDirEc)) {
        printOpenFailure("Cannot open " + path.string());
        settings_.removeRecentFile(platform::pathToUtf8(path));
        return makeError(ErrorCode::Io, "Cannot read " + path.string());
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(size > 0 ? static_cast<std::size_t>(size) : 0);
    if (!bytes.empty())
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    lap("read file");
    dxf::Report report;
    auto result = dxf::importDxf(bytes, {}, report);
    std::vector<std::byte>().swap(bytes);  // not needed while the scene and index are built
    lap("convert to drawing");
    if (!result) {
        printOpenFailure("Cannot open " + path.string() + ": " + result.error().message);
        return makeError(ErrorCode::Parse, result.error().message);
    }
    auto& imported = result.value();
    std::size_t total = 0;
    for (const auto& [name, count] : imported.summary.entities)
        total += count.imported;
    // External references whose files are found are loaded, as AutoCAD does on open.
    const xref::OpenLoad xrefLoad = xref::loadOnOpen(*imported.document, path);
    lap("load external references");
    addSession(std::move(imported.document), path);
    for (const std::string& name : xrefLoad.loaded)
        print("Loaded external reference " + name + ".");
    for (const std::string& line : xrefLoad.failed)
        print("Cannot load external reference " + line + ".");
    lap("index, scene, zoom (total)");
    std::size_t shown = 0;
    for (const auto& d : report.diagnostics()) {
        if (d.severity == dxf::Severity::Info)
            continue;
        if (++shown > 5)
            break;
        print("DXF: " + d.message);
    }
    std::size_t warnings =
        report.count(dxf::Severity::Warning) + report.count(dxf::Severity::Error);
    if (warnings > 5)
        print("DXF: " + std::to_string(warnings - 5) + " more notice(s) not shown.");
    if (imported.summary.unitsAssumed)
        print("DXF: the file has no units; millimetres assumed.");
    settings_.addRecentFile(platform::pathToUtf8(path));
    wantThumbnail(path);
    (void)settings_.save(paths_.settingsFile());
    // The status bar counts model space; the rest of the file's objects are inside blocks,
    // layouts or other spaces.
    std::string counts = std::to_string(session_->doc.modelSpaceCount()) + " objects";
    if (total > session_->doc.modelSpaceCount())
        counts += ", " + std::to_string(total - session_->doc.modelSpaceCount()) +
                  " more inside blocks and layouts";
    print("Opened " + path.string() + " (" + counts + ")");
    return {};
}

dxf::ExportOptions AppCore::dxfSaveOptions() const {
    dxf::ExportOptions options;
    // files.dxfVersion picks R12 / R2000 / R2007 for every DXF save; "auto" (default) keeps a
    // drawing that came in as AC1021 or later in that version, so its text keeps UTF-8 instead
    // of being narrowed to a code page.
    const std::string chosen = settings_.getString("files.dxfVersion", "auto");
    if (chosen != "auto") {
        if (auto v = dxf::parseVersion(chosen)) {
            options.version = *v;
            return options;
        }
    }
    const auto& custom = session_->doc.header().custom;
    if (auto it = custom.find("dxf.acadver"); it != custom.end() && it->second.size() == 6 &&
                                              it->second.compare(0, 2, "AC") == 0 &&
                                              std::atoi(it->second.c_str() + 2) >= 1021)
        options.version = dxf::DxfVersion::R2007;
    return options;
}

std::vector<std::string> AppCore::dxfLossWarnings(dxf::DxfVersion version) const {
    dxf::Report report;
    dxf::ExportOptions options;
    options.version = version;
    std::vector<std::string> out;
    auto r = dxf::exportDxf(session_->doc, options, report);
    if (!r)
        return {r.error().message};
    for (const auto& d : report.diagnostics()) {
        if (d.severity == dxf::Severity::Info)
            continue;
        if (out.size() >= 20) {
            out.push_back("...and more (see the DXF export report).");
            break;
        }
        out.push_back(d.message);
    }
    return out;
}

Result<void> AppCore::saveDxf(const fs::path& path) {
    dxf::Report report;
    const dxf::ExportOptions options = dxfSaveOptions();
    auto out = dxf::exportDxf(session_->doc, options, report);
    if (!out) {
        print("Cannot save " + path.string() + ": " + out.error().message);
        return makeError(ErrorCode::InvalidArgument, out.error().message);
    }
    // Same safe write as .tbcad: temporary file synced, then swapped in (the old file only moves
    // to .bak once the new one is ready), so a failed or killed save never leaves the path empty.
    const auto& bytes = out.value().bytes;
    io::WriteOptions wo;
    wo.keepBackup = settings_.getBool("files.backup", true);
    auto saved =
        io::saveTbcadBytes(std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()),
                           path, wo);
    if (!saved) {
        print("Cannot save " + path.string() + ": " + saved.error().message);
        return makeError(ErrorCode::Io, saved.error().message);
    }
    const auto& sum = out.value().summary;
    if (sum.skipped > 0)
        print("DXF: " + std::to_string(sum.skipped) + " object(s) could not be written.");
    if (sum.downgraded > 0)
        print("DXF: " + std::to_string(sum.downgraded) +
              " object(s) were written in a simpler form.");
    return {};
}

static std::string lowerExtension(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

void AppCore::setForeignReader(std::string extension, ForeignReadFn read, bool readOnly) {
    foreignReaders_[std::move(extension)] = ForeignReader{std::move(read), readOnly};
}

bool AppCore::canOpenExtension(std::string_view e) const {
    return e == ".tbcad" || e == ".dxf" || foreignReaders_.find(e) != foreignReaders_.end();
}

bool AppCore::readOnlyOrigin() const {
    return !session_->readOnlyFormat.empty();
}

std::string AppCore::readOnlyBanner() const {
    if (!readOnlyOrigin())
        return {};
    return session_->readOnlyFormat +
           " opened read-only. Use Save As to keep a copy as .tbcad or .dxf.";
}

Result<void> AppCore::finishForeignOpen(const fs::path& path, const std::string& ext, bool readOnly,
                                        Result<ForeignOpenResult> res) {
    if (!res) {
        printOpenFailure("Cannot open " + path.string() + ": " + res.error().message);
        settings_.removeRecentFile(platform::pathToUtf8(path));
        return res.error();
    }
    ForeignOpenResult& r = res.value();
    addSession(std::move(r.document), path);
    if (readOnly) {
        std::string fmt = ext.size() > 1 ? ext.substr(1) : ext;
        for (char& c : fmt)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        session_->readOnlyFormat = fmt;
    }
    if (!r.report.empty()) {
        std::string text;
        for (const auto& line : r.report)
            text += line + "\n";
        session_->openReport = std::move(text);
        session_->openReportFormat = r.format;
    }
    for (const auto& m : r.messages)
        print(m);
    openStep("  print messages");
    settings_.addRecentFile(platform::pathToUtf8(path));
    wantThumbnail(path);
    (void)settings_.save(paths_.settingsFile());
    openStep("  save recent files");
    print("Opened " + path.string() + (r.format.empty() ? "" : " (" + r.format + ")") +
          (readOnly ? " read-only" : ""));
    if (readOnly)
        print(readOnlyBanner());
    return {};
}

struct AppCore::PendingOpen {
    fs::path path;
    std::string ext;
    bool readOnly = true;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::future<Result<ForeignOpenResult>> result;
};

bool AppCore::openFileAsync(const fs::path& path) {
    if (pendingOpen_ || slotOfFile(path))
        return false;
    auto it = foreignReaders_.find(lowerExtension(path));
    if (it == foreignReaders_.end())
        return false;
    auto job = std::make_shared<PendingOpen>();
    job->path = path;
    job->ext = it->first;
    job->readOnly = it->second.readOnly;
    job->result =
        std::async(std::launch::async, [read = it->second.read, path] { return read(path); });
    pendingOpen_ = std::move(job);
    return true;
}

std::optional<Result<void>> AppCore::pollOpen() {
    if (!pendingOpen_)
        return std::nullopt;
    if (pendingOpen_->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return std::nullopt;
    std::shared_ptr<PendingOpen> job = std::move(pendingOpen_);
    pendingOpen_.reset();
    return finishForeignOpen(job->path, job->ext, job->readOnly, job->result.get());
}

std::optional<AppCore::OpeningInfo> AppCore::opening() const {
    if (!pendingOpen_)
        return std::nullopt;
    return OpeningInfo{
        pendingOpen_->path,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - pendingOpen_->start)
            .count()};
}

Result<void> AppCore::openFile(const fs::path& path) {
    if (auto open = slotOfFile(path)) {
        activateDocument(*open);
        return {};
    }
    {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            const std::string text = "File not found: " + path.string() +
                                     ". It may have been moved, renamed or deleted.";
            printOpenFailure(text);
            settings_.removeRecentFile(platform::pathToUtf8(path));  // no point offering it again
            return makeError(ErrorCode::NotFound, text);
        }
        if (fs::is_directory(path, ec)) {
            const std::string text = path.string() + " is a folder, not a drawing file.";
            printOpenFailure(text);
            return makeError(ErrorCode::InvalidArgument, text);
        }
    }
    openTimings_.clear();
    openLapAt_ = std::chrono::steady_clock::now();
    openStepAt_ = openLapAt_;
    struct StopSteps {
        std::optional<std::chrono::steady_clock::time_point>& at;
        ~StopSteps() { at.reset(); }
    } stopSteps{openStepAt_};
    if (auto it = foreignReaders_.find(lowerExtension(path)); it != foreignReaders_.end()) {
        auto res = it->second.read(path);
        openLap("read " + it->first + " file (all steps)");
        if (res)
            for (const auto& t : res.value().timings)
                openTimings_.emplace_back("  " + t.first, t.second);
        auto done = finishForeignOpen(path, it->first, it->second.readOnly, std::move(res));
        openLap("index, scene, zoom (total)");
        return done;
    }
    if (isDxfPath(path))
        return openDxf(path);
    if (tmpl::isTemplatePath(path)) {
        // A template opens as a new untitled drawing; the template file stays as it is.
        auto doc = tmpl::loadTemplate(path);
        if (!doc) {
            printOpenFailure("Cannot open " + path.string() + ": " + doc.error().message);
            return doc.error();
        }
        addSession(std::move(doc.value()), {});
        print("New drawing from template " + path.string());
        return {};
    }
    io::IoReport report;
    auto doc = io::readTbcad(path, report);
    openLap("read file");
    bool salvaged = false;
    if (!doc) {
        // A damaged file: keep whatever can still be read, as an untitled drawing so the
        // original file is never overwritten by mistake.
        const std::string why = doc.error().message;
        io::IoReport salvageReport;
        io::ReadOptions so;
        so.salvage = true;
        auto rescued = io::readTbcad(path, salvageReport, so);
        if (!rescued || salvageReport.entityCount == 0) {
            printOpenFailure("Cannot open " + path.string() + ": " + why);
            settings_.removeRecentFile(platform::pathToUtf8(path));
            return doc.error();
        }
        print(path.filename().string() + " is damaged (" + why + ").");
        print("Recovered " + std::to_string(salvageReport.entityCount) +
              " objects. Save the drawing under a new name; the original file is left as it is.");
        doc = std::move(rescued);
        report = std::move(salvageReport);
        salvaged = true;
    }
    addSession(std::move(doc.value()), salvaged ? fs::path{} : path);
    openLap("index, scene, zoom");
    if (salvaged)
        session_->savedRevision = ~std::uint64_t{0};  // unsaved: the rescued content is not on disk
    for (const auto& w : report.warnings) {
        // Reader diagnostics for developers ("header stages: required=...") are not for the user.
        if (w.find("header stages") != std::string::npos ||
            w.find("required=") != std::string::npos) {
            std::fprintf(stderr, "tbcad: %s\n", w.c_str());
            continue;
        }
        print("Warning: " + w);
    }
    if (report.unknownEntityKinds > 0)
        print(std::to_string(report.unknownEntityKinds) +
              " object(s) from a newer version are kept read-only.");
    if (salvaged)
        return {};
    settings_.addRecentFile(platform::pathToUtf8(path));
    wantThumbnail(path);
    (void)settings_.save(paths_.settingsFile());
    print("Opened " + path.string() + " (" + std::to_string(report.entityCount) + " objects)");
    return {};
}

Result<void> AppCore::save() {
    if (readOnlyOrigin())
        return makeError(ErrorCode::Conflict,
                         session_->readOnlyFormat + " files are read-only: use Save As.");
    if (session_->file.empty())
        return makeError(ErrorCode::InvalidArgument, "This drawing has no file name yet.");
    if (changedOnDisk(*session_) && !session_->overwriteConfirmed) {
        session_->overwriteConfirmed = true;
        const std::string msg = session_->file.filename().string() +
                                " was changed by another program or window since it was opened. "
                                "Save again to overwrite it, or use Save As to keep both.";
        print(msg);
        return makeError(ErrorCode::Conflict, msg);
    }
    return saveAs(session_->file);
}

bool AppCore::fileChangedOnDisk() const {
    return !readOnlyOrigin() && changedOnDisk(*session_);
}

Result<void> AppCore::saveOverwrite() {
    if (session_ && !session_->file.empty())
        session_->overwriteConfirmed = true;
    return save();
}

// Puts the current UCS into the drawing's header so that it comes back when the file is reopened.
void AppCore::storeCurrentUcs() {
    const drafting::Ucs u = drafting::currentUcs(&session_->doc, this);
    std::string row;
    if (!u.isWorld()) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "\t%.17g\t%.17g\t%.17g", u.origin.x, u.origin.y, u.angle);
        row = u.name + buf;
    }
    const auto& custom = session_->doc.header().custom;
    auto it = custom.find(kCurrentUcsKey);
    if (row.empty() ? it == custom.end() : (it != custom.end() && it->second == row))
        return;
    auto tx = session_->doc.begin("UCS");
    (void)tx.modifyHeader([&](Header& h) {
        if (row.empty())
            h.custom.erase(kCurrentUcsKey);
        else
            h.custom[kCurrentUcsKey] = row;
    });
    tx.commit();
}

// Puts SNAPANG and SNAPBASE into the header (the DXF variables of those names) so that the
// rotated snap comes back when the file is reopened.
void AppCore::storeSnapAids() {
    const std::string angText = settings_.getString(drafting::keys::snapAngle, "0");
    const std::string baseText = settings_.getString(drafting::keys::snapBase, "0,0");
    const auto cur = snapFromHeader(session_->doc);
    auto num = [](const std::string& t) { return std::strtod(t.c_str(), nullptr); };
    if (num(angText) == num(cur.first) && baseText == cur.second)
        return;
    const double bx = std::strtod(baseText.c_str(), nullptr);
    const auto comma = baseText.find(',');
    const double by =
        comma == std::string::npos ? 0 : std::strtod(baseText.c_str() + comma + 1, nullptr);
    char buf[160];
    auto tx = session_->doc.begin("SNAP");
    (void)tx.modifyHeader([&](Header& h) {
        std::snprintf(buf, sizeof buf, "50\t%.17g\n", num(angText) * kDegToRad);
        h.custom[kSnapAngVar] = buf;
        std::snprintf(buf, sizeof buf, "10\t%.17g\n20\t%.17g\n", bx, by);
        h.custom[kSnapBaseVar] = buf;
    });
    tx.commit();
}

Result<void> AppCore::saveAs(const fs::path& requested) {
    fs::path path = requested;
    if (!path.has_extension())
        path += settings_.getString("files.defaultFormat", "tbcad") == "dxf" ? ".dxf" : ".tbcad";
    if (runner().active()) {
        // Saving is an explicit request: end the waiting command (as Esc would) rather than
        // refuse and leave the work unsaved.
        runner().submit(cmd::Input::cancel());
        if (runner().active()) {
            print("Finish the current command first.");
            return makeError(ErrorCode::Conflict, "A command is running.");
        }
        print("The running command was cancelled to save.");
    }
    const std::string ext = lowerExtension(path);
    if (ext == ".tbt") {
        // "Drawing Template" in the Save As dialog: the template is a copy; this drawing keeps
        // its own name and stays as it is.
        auto r = tmpl::saveTemplate(document(), path);
        if (!r) {
            print("Cannot save template " + path.string() + ": " + r.error().message);
            return r.error();
        }
        print("Template saved: " + platform::pathToUtf8(r.value()));
        return {};
    }
    if (ext != ".tbcad" && ext != ".dxf") {
        return makeError(ErrorCode::Unsupported,
                         "TBCAD can save as .tbcad or .dxf; it never writes " + ext + " files.");
    }
    storeCurrentUcs();
    storeSnapAids();
    // Relative xref paths must keep finding their files from the new folder.
    const fs::path oldFile = session_->file;
    bool rebased = false;
    if (!oldFile.empty() && oldFile.parent_path() != path.parent_path()) {
        Transaction tx = session_->doc.begin("SAVEAS");
        rebased = xref::rebaseAll(tx, session_->doc, oldFile, path) > 0;
        if (rebased)
            tx.commit();
    }
    io::WriteOptions writeOpt;
    writeOpt.keepBackup = settings_.getBool("files.backup", true);  // ISAVEBAK
    auto r = isDxfPath(path) ? saveDxf(path) : io::writeTbcad(session_->doc, path, writeOpt);
    if (!r && rebased) {  // the file did not move: put the paths back
        Transaction tx = session_->doc.begin("SAVEAS");
        (void)xref::rebaseAll(tx, session_->doc, path, oldFile);
        tx.commit();
    }
    if (!r) {
        print("Cannot save " + path.string() + ": " + r.error().message);
        return r;
    }
    const bool sameFile = session_->file == path;
    session_->file = path;
    session_->doc.setFilePath(platform::pathToUtf8(path));
    session_->readOnlyFormat.clear();
    session_->savedRevision = session_->doc.revision();
    if (sameFile && session_->lock) {
        // Keep the lock; only the stamp moves to the file just written.
        std::error_code ec;
        const auto t = fs::last_write_time(path, ec);
        session_->diskStamp = ec ? std::nullopt : std::optional(t);
        session_->overwriteConfirmed = false;
    } else {
        watchFile(*session_);
    }
    removeAutosaveOf(slots_[active_].id);
    if (!anyDocumentModified())
        removeAutosaveFiles();
    settings_.addRecentFile(platform::pathToUtf8(path));
    wantThumbnail(path);
    (void)settings_.save(paths_.settingsFile());
    print("Saved " + path.string());
    return {};
}

std::string AppCore::displayName() const {
    std::string name =
        session_->file.empty() ? std::string("Untitled") : session_->file.filename().string();
    return readOnlyOrigin() ? name + " [Read-only]" : name;
}

std::string AppCore::windowTitle() const {
    return displayName() + (modified() ? "*" : "") + " - TBCAD";
}

// ---- undo / redo -----------------------------------------------------------------------------

bool AppCore::undo() {
    inUndo_ = true;
    bool ok = runner().undo();
    inUndo_ = false;
    return ok;
}

bool AppCore::redo() {
    inUndo_ = true;
    bool ok = runner().redo();
    inUndo_ = false;
    return ok;
}

bool AppCore::historyStepHeavy(bool redo) const {
    std::size_t limit = std::size_t{16} << 20;
    if (auto v = setting("undo.busyBytes"))
        if (const long long n = std::strtoll(v->c_str(), nullptr, 10); n > 0)
            limit = static_cast<std::size_t>(n);
    const auto& u = session_->undo;
    return (redo ? u.nextRedoBytes() : u.nextUndoBytes()) >= limit;
}

std::string AppCore::undoLabel() const {
    if (session_->runner->active() || !session_->undo.canUndo())
        return {};
    return "Undo " + session_->undo.undoLabel();
}

std::string AppCore::redoLabel() const {
    if (session_->runner->active() || !session_->undo.canRedo())
        return {};
    return "Redo " + session_->undo.redoLabel();
}

// ---- command line ------------------------------------------------------------------------------

void AppCore::print(std::string_view message) {
    platform::recordCommandLine(std::string(message));
    if (logFile_.is_open()) {
        logFile_ << message << '\n';
        logFile_.flush();
    }
    history_.emplace_back(message);
    if (history_.size() > kMaxHistory)
        history_.erase(history_.begin(),
                       history_.begin() + static_cast<std::ptrdiff_t>(kMaxHistory / 4));
}

std::filesystem::path AppCore::logFolder() const {
    return logDir_.empty() ? paths_.dataDir / "logs" : logDir_;
}

bool AppCore::startLogFile() {
    if (logFile_.is_open())
        return true;
    std::error_code ec;
    std::filesystem::create_directories(logFolder(), ec);
    char stamp[32] = "session";
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    logPath_ = logFolder() / (std::string("tbcad-") + stamp + ".log");
    logFile_.open(logPath_, std::ios::app | std::ios::binary);
    if (!logFile_.is_open())
        return false;
    // The lines already on screen go first, so the file reads like the command window.
    for (const auto& l : history_)
        logFile_ << l << '\n';
    logFile_.flush();
    return true;
}

void AppCore::stopLogFile() {
    if (logFile_.is_open())
        logFile_.close();
}

std::string AppCore::promptText() const {
    if (!prompt_)
        return "Command: ";
    return prompt_->display();
}

namespace {

// Commands that only change what is shown (layer visibility), not the drawing itself.
bool isViewStateCommand(const std::vector<std::string>& words) {
    if (words.empty())
        return false;
    const std::string name = cmd::toUpperAscii(words[0]);
    if (name == "LAYISO" || name == "LAYUNISO" || name == "LAYOFF" || name == "LAYON" ||
        name == "LAYFRZ" || name == "LAYTHW")
        return true;
    if (name != "-LAYER" || words.size() < 2)
        return false;
    const std::string option = cmd::toUpperAscii(words[1]);
    return option == "ON" || option == "OFF" || option == "FREEZE" || option == "THAW" ||
           option == "SET" || option == "ISOLATE" || option == "SHOWALL";
}

}  // namespace

void AppCore::submitPanelText(std::string_view text) {
    echoMode_ = runner().active() ? EchoMode::None : EchoMode::Names;
    submitText(text);
    echoMode_ = EchoMode::Typed;
}

namespace {
// Numbers with a long fraction ("3787.4686716791975,5741.31161") are shortened to a few decimals
// for the command line; the command itself still gets the full value.
std::string roundedNumbers(const std::string& token) {
    std::string out;
    for (std::size_t i = 0; i < token.size();) {
        const char c = token[i];
        const bool start = std::isdigit(static_cast<unsigned char>(c)) ||
                           ((c == '-' || c == '.') && i + 1 < token.size() &&
                            std::isdigit(static_cast<unsigned char>(token[i + 1])));
        if (!start) {
            out.push_back(c);
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        bool dot = c == '.';
        while (j < token.size() &&
               (std::isdigit(static_cast<unsigned char>(token[j])) || (!dot && token[j] == '.'))) {
            dot = dot || token[j] == '.';
            ++j;
        }
        const std::string num = token.substr(i, j - i);
        const std::size_t point = num.find('.');
        if (point != std::string::npos && num.size() - point - 1 > 4) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.4f", std::strtod(num.c_str(), nullptr));
            std::string r = buf;
            while (r.back() == '0')
                r.pop_back();
            if (r.back() == '.')
                r.pop_back();
            out += r;
        } else {
            out += num;
        }
        i = j;
    }
    return out;
}
}  // namespace

// "#12" in a line a panel sent stands for a layer (or, after "Linetype", a linetype).
std::string AppCore::friendlyEcho(const std::string& line) const {
    std::vector<std::string> words = splitWords(line);
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::string w = words[i];
        if (w.size() > 1 && w[0] == '#' &&
            w.find_first_not_of("0123456789", 1) == std::string::npos) {
            const auto id = static_cast<std::uint32_t>(std::strtoul(w.c_str() + 1, nullptr, 10));
            const bool lt = i > 0 && cmd::toUpperAscii(words[i - 1]) == "LINETYPE";
            if (lt) {
                if (const Linetype* l = session_->doc.linetype(LinetypeId{id}))
                    w = l->name;
            } else if (const Layer* l = session_->doc.layer(LayerId{id})) {
                w = l->name;
            }
        } else
            w = roundedNumbers(w);
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

void AppCore::submitScript(std::string_view script) {
    for (std::size_t at = 0; at <= script.size();) {
        const std::size_t end = script.find(';', at);
        submitText(script.substr(at, end == std::string_view::npos ? end : end - at));
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
}

void AppCore::submitText(std::string_view text) {
    std::string trimmed = cmd::trim(text);
    cmd::CommandRunner& run = runner();
    // Turning layers on and off in a drawing opened from a read-only format is not an edit: it
    // must not mark the drawing as modified (there is nothing to save back).
    struct ViewStateGuard {
        AppCore& core;
        ~ViewStateGuard() {
            core.applyPendingLayout();
            if (core.viewStatePending_ && !core.runner().active()) {
                core.viewStatePending_ = false;
                core.session_->savedRevision = core.session_->doc.revision();
            }
        }
    } guard{*this};
    if (!run.active() && !trimmed.empty())
        viewStatePending_ =
            readOnlyOrigin() && !modified() &&
            isViewStateCommand(splitWords(trimmed[0] == '\'' ? trimmed.substr(1) : trimmed));
    if (echoMode_ == EchoMode::Typed)
        print(promptText() + trimmed);
    else if (echoMode_ == EchoMode::Names)
        print(promptText() + friendlyEcho(trimmed));
    if (run.active()) {
        run.submit(trimmed.empty() ? cmd::Input::enter() : cmd::Input::typed(trimmed));
        return;
    }
    if (trimmed.empty()) {
        run.submit(cmd::Input::enter());
        return;
    }
    // A leading apostrophe marks a transparent command; with nothing running it is just a command.
    if (trimmed.size() > 1 && trimmed[0] == '\'')
        trimmed.erase(0, 1);
    std::vector<std::string> words = splitWords(trimmed);
    if (words.empty())
        return;
    if (words[0] == "?" || cmd::toUpperAscii(words[0]) == "HELP") {
        std::string query;
        for (std::size_t i = 1; i < words.size(); ++i)
            query += (query.empty() ? "" : " ") + words[i];
        showHelp(query);
        return;
    }
    if (cmd::SubmitResult r = run.submit(cmd::Input::typed(words[0]));
        r == cmd::SubmitResult::UnknownCommand)
        return;
    // Remaining words answer the prompts of the command that just started.
    for (std::size_t i = 1; i < words.size() && run.active(); ++i)
        run.submit(cmd::Input::typed(words[i]));
}

// HELP / ?: a command name (or alias) shows its page; other words search the pages; nothing lists
// every page that exists.
void AppCore::showHelp(const std::string& query) {
    if (help_.empty()) {
        print("The help pages were not found (assets/help/commands.md).");
        return;
    }
    std::string name = query;
    if (const cmd::Command* c = registry_.find(query))
        name = std::string(c->name());
    if (const cmd::HelpEntry* e = query.empty() ? nullptr : help_.find(name)) {
        for (const std::string& l : cmd::helpLines(*e))
            print(l);
        helpRequest_ = e->name;
        return;
    }
    std::vector<const cmd::HelpEntry*> hits = help_.search(query);
    if (hits.empty()) {
        print("No help page matches \"" + query + "\". HELP lists the commands that have one.");
        return;
    }
    std::string names;
    for (const cmd::HelpEntry* e : hits)
        names += (names.empty() ? "" : ", ") + e->name;
    print((query.empty() ? "Commands with a help page: " : "Help pages for \"" + query + "\": ") +
          names);
    print("Type HELP followed by a command name to read its page, or press F1 while it runs.");
}

void AppCore::pressEscape() {
    setSetting("drafting.osnap.once", "");
    if (windowCorner_) {
        windowCorner_.reset();
        setSelectionDrag(std::nullopt);
        return;
    }
    if (runner().active())
        runner().submit(cmd::Input::cancel());
    else
        clearSelection();
}

std::vector<std::string> AppCore::complete(std::string_view prefix) const {
    std::vector<std::string> out;
    std::string up = cmd::toUpperAscii(prefix);
    if (up.empty())
        return out;
    for (cmd::Command* c : registry_.list()) {
        if (c->description().empty())
            continue;  // hidden commands
        std::string name(c->name());
        if (name.rfind(up, 0) == 0)
            out.push_back(name);
    }
    for (const auto& [alias, target] : registry_.aliases())
        if (alias.rfind(up, 0) == 0 && std::find(out.begin(), out.end(), alias) == out.end())
            out.push_back(alias);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- viewport ----------------------------------------------------------------------------------

void AppCore::setViewportOrigin(double x, double y) {
    if (haveOrigin_ && camera_.viewportW > 1 && fitPending_ == 0 &&
        (x != viewportOrigin_.x || y != viewportOrigin_.y)) {
        camera_.center.x += (x - viewportOrigin_.x) / camera_.pixelsPerMm;
        camera_.center.y -= (y - viewportOrigin_.y) / camera_.pixelsPerMm;
    }
    viewportOrigin_ = {x, y};
    haveOrigin_ = true;
}

void AppCore::setViewportSize(int w, int h) {
    const int nw = std::max(w, 1), nh = std::max(h, 1);
    const bool changed = nw != camera_.viewportW || nh != camera_.viewportH;
    const int oldW = camera_.viewportW, oldH = camera_.viewportH;
    camera_.viewportW = nw;
    camera_.viewportH = nh;
    // A drawing just opened was fitted to the size the window had then; while the user has not
    // touched the view, fit it again when the panels settle into their real size. That only
    // lasts a moment: a panel the user opens later must not move the drawing.
    if (fitPending_ > 0 && std::chrono::steady_clock::now() > fitUntil_)
        fitPending_ = 0;
    if (changed && fitPending_ > 0 && session_) {
        --fitPending_;
        zoomExtents();
    } else if (changed && oldW > 1 && oldH > 1) {
        // Keep what is at the top left of the canvas where it is when a panel opens or closes
        // (the panels sit at the right and bottom), so the drawing does not jump.
        camera_.center.x += (nw - oldW) / 2.0 / camera_.pixelsPerMm;
        camera_.center.y -= (nh - oldH) / 2.0 / camera_.pixelsPerMm;
        // (not a pan of the viewport being edited)
        editEntryCenter_.x += (nw - oldW) / 2.0 / camera_.pixelsPerMm;
        editEntryCenter_.y -= (nh - oldH) / 2.0 / camera_.pixelsPerMm;
    }
}

void AppCore::setCamera(const render::Camera2D& cam) {
    camera_ = cam;
    fitPending_ = 0;
}

void AppCore::rememberGestureStart() {
    // A burst of wheel notches or one drag is one step of ZOOM Previous.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastGesture_ > std::chrono::milliseconds(1000))
        cmd::pushViewHistory(*this, view());
    lastGesture_ = now;
}

void AppCore::zoomAtScreen(Vec2 screen, double notches) {
    rememberGestureStart();
    fitPending_ = 0;
    camera_.zoomAt(screen, std::pow(kZoomStep, notches));
}

void AppCore::panPixels(double dx, double dy) {
    rememberGestureStart();
    fitPending_ = 0;
    camera_.panPixels(dx, dy);
}

// Rays and xlines have no end, so they do not count; if they are all there is, use them.
BBox2 AppCore::drawingExtents() const {
    if (extentsCache_.session == session_ && extentsCache_.revision == session_->doc.revision())
        return extentsCache_.box;
    const BBox2 box = computeDrawingExtents();
    extentsCache_.session = session_;
    extentsCache_.revision = session_->doc.revision();
    extentsCache_.box = box;
    return box;
}

BBox2 AppCore::computeDrawingExtents() const {
    BBox2 box;
    BBox2 withoutPoints;
    std::vector<BBox2> boxes;
    const Document& doc = session_->doc;
    boxes.reserve(doc.modelSpaceCount());
    // Like AutoCAD's ZOOM Extents, only what is shown counts: objects on layers that are off or
    // frozen (an imported file's hidden backup layers) do not stretch the view. If nothing is
    // shown, fall back to everything so the command still goes somewhere.
    // Measured on several threads: a 393,000-object survey took 200 ms on one.
    const std::vector<EntityId> ids = doc.modelSpace();
    for (const auto& [id, layer] : doc.layers())
        (void)doc.effective(id);  // (fills the lookups that cache on first use, before the threads)
    struct Part {
        BBox2 box, withoutPoints;
        std::vector<BBox2> boxes;
    };
    const std::size_t lanes = std::clamp<std::size_t>(
        std::min<std::size_t>(std::thread::hardware_concurrency(), ids.size() / 20000), 1, 8);
    for (bool onlyShown : {true, false}) {
        std::vector<Part> parts(lanes);
        auto scan = [&](std::size_t k) {
            Part& p = parts[k];
            const std::size_t from = ids.size() * k / lanes, to = ids.size() * (k + 1) / lanes;
            p.boxes.reserve(to - from);
            for (std::size_t i = from; i < to; ++i) {
                const Entity* e = doc.find(ids[i]);
                if (!e || !geom::countsForExtents(*e) || (onlyShown && !ui::isVisible(doc, *e)))
                    continue;
                const BBox2 b = session_->index.boxOf(ids[i]);
                p.box.merge(b);
                if (kind(*e) != EntityKind::Point)
                    p.withoutPoints.merge(b);
                if (!b.empty())
                    p.boxes.push_back(b);
            }
        };
        std::vector<std::future<void>> jobs;
        for (std::size_t k = 1; k < lanes; ++k)
            jobs.push_back(std::async(std::launch::async, scan, k));
        scan(0);
        for (auto& j : jobs)
            j.get();
        for (Part& p : parts) {
            box.merge(p.box);
            withoutPoints.merge(p.withoutPoints);
            boxes.insert(boxes.end(), p.boxes.begin(), p.boxes.end());
        }
        if (!box.empty())
            break;
        boxes.clear();
    }
    // Survey points far off the drawing (a stray POINT at the origin of a file drawn in
    // coordinates millions of units away) must not shrink the drawing itself: when points more
    // than double the size of everything else, leave them out.
    if (!withoutPoints.empty() &&
        (box.width() > 2 * withoutPoints.width() || box.height() > 2 * withoutPoints.height())) {
        box = withoutPoints;
        boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                                   [&](const BBox2& b) { return !withoutPoints.containsBox(b); }),
                    boxes.end());
    }
    // A few stray objects far from everything else (common in imported files) would shrink the
    // real drawing to a speck: when the outermost 5% of the objects stretch the view more than
    // fivefold, frame the rest.
    if (boxes.size() >= 10) {
        std::vector<double> xs, ys;
        xs.reserve(boxes.size());
        ys.reserve(boxes.size());
        for (const BBox2& b : boxes) {
            xs.push_back(b.center().x);
            ys.push_back(b.center().y);
        }
        const std::size_t cut = std::max<std::size_t>(1, boxes.size() / 20);
        // Only the 5% and 95% values are needed, not the whole order.
        auto lowHigh = [&](std::vector<double>& v) {
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(cut), v.end());
            const double low = v[cut];
            std::nth_element(v.begin() + static_cast<std::ptrdiff_t>(cut),
                             v.begin() + static_cast<std::ptrdiff_t>(v.size() - 1 - cut), v.end());
            return std::pair<double, double>{low, v[v.size() - 1 - cut]};
        };
        const auto [xLow, xHigh] = lowHigh(xs);
        const auto [yLow, yHigh] = lowHigh(ys);
        const double spanX = xHigh - xLow;
        const double spanY = yHigh - yLow;
        BBox2 core;
        for (const BBox2& b : boxes) {
            const Vec2 c = b.center();
            if (c.x >= xLow - spanX && c.x <= xHigh + spanX && c.y >= yLow - spanY &&
                c.y <= yHigh + spanY)
                core.merge(b);
        }
        if (!core.empty() && (box.width() > 5 * core.width() || box.height() > 5 * core.height()))
            box = core;
    }
    return box.empty() ? session_->index.bounds() : box;
}

BBox2 AppCore::viewExtents() const {
    return inPaperSpace() ? paper_.sheet() : drawingExtents();
}

void AppCore::zoomExtents() {
    const BBox2 box = viewExtents();
    if (box.empty() && !inPaperSpace()) {
        // Nothing drawn yet: start at room size (about 20 m across) instead of 1 px = 1 mm.
        constexpr double kRoomMm = 20000.0;
        const double aspect = static_cast<double>(std::max(camera_.viewportH, 1)) /
                              static_cast<double>(std::max(camera_.viewportW, 1));
        BBox2 room;
        // The origin sits a little inside the corner, so it is visible and not under the edge.
        room.expand({-0.06 * kRoomMm, -0.06 * kRoomMm});
        room.expand({0.94 * kRoomMm, (0.94 * aspect - 0.06 * (1.0 - aspect)) * kRoomMm});
        camera_.fit(room);
        return;
    }
    zoomToBox(box);
}

void AppCore::zoomToBox(const BBox2& box) {
    if (box.empty())
        return;
    camera_.fit(box);
}

void AppCore::zoomToEntities(const std::vector<EntityId>& ids) {
    BBox2 box;
    for (EntityId id : ids)
        box.merge(session_->index.boxOf(id));
    zoomToBox(box);
}

void AppCore::tickXrefs(std::chrono::steady_clock::time_point now) {
    using namespace std::chrono_literals;
    if (xrefReloadWait_ > 0) {
        // The window has had a frame to show the "Reloading" card; the reload itself blocks.
        if (--xrefReloadWait_ == 0 && session_ && !session_->runner->active()) {
            submitText("XRELOAD");
            submitText("");
        }
        return;
    }
    if (now - lastXrefCheck_ < 2s)
        return;
    lastXrefCheck_ = now;
    if (!session_)
        return;
    const std::string notifyText = setting("xref.notify").value_or("2");
    const int notify = notifyText.size() == 1 && notifyText[0] >= '0' && notifyText[0] <= '3'
                           ? notifyText[0] - '0'
                           : 2;
    session_->xrefChanged.clear();
    if (notify == 0) {
        session_->xrefNotice.clear();
        return;
    }
    if (session_->runner->active() && notify == 3)
        return;
    std::vector<std::string> fresh;
    for (BlockId id : xref::xrefs(session_->doc)) {
        if (xref::state(session_->doc, id, session_->file) != xref::State::Changed)
            continue;
        const BlockDef* def = session_->doc.block(id);
        session_->xrefChanged.push_back(def->name);
        std::int64_t stamp = xref::fileStamp(xref::resolvePath(def->xrefPath, session_->file));
        auto tried = session_->xrefTried.find(id);
        if (tried != session_->xrefTried.end() && tried->second == stamp)
            continue;
        session_->xrefTried[id] = stamp;
        fresh.push_back(def->name);
    }
    // A reference reloaded or detached meanwhile leaves the balloon.
    std::erase_if(session_->xrefNotice, [&](const std::string& n) {
        return std::find(session_->xrefChanged.begin(), session_->xrefChanged.end(), n) ==
               session_->xrefChanged.end();
    });
    if (fresh.empty())
        return;
    if (notify == 2) {
        for (std::string& n : fresh)
            if (std::find(session_->xrefNotice.begin(), session_->xrefNotice.end(), n) ==
                session_->xrefNotice.end())
                session_->xrefNotice.push_back(std::move(n));
        return;
    }
    if (notify != 3 || !registry_.find("XRELOAD"))
        return;
    print("External reference changed on disk; reloading");
    xrefReloadWait_ = 1;  // one frame with the notice on screen, then the blocking reload
}

std::vector<std::string> AppCore::changedXrefs() const {
    return session_ ? session_->xrefChanged : std::vector<std::string>{};
}

std::vector<std::string> AppCore::xrefChangeNotice() const {
    return session_ ? session_->xrefNotice : std::vector<std::string>{};
}

void AppCore::dismissXrefChangeNotice() {
    if (session_)
        session_->xrefNotice.clear();
}

void AppCore::reloadXrefs(const std::vector<std::string>& names) {
    if (!session_)
        return;
    const std::vector<std::string> targets = names.empty() ? session_->xrefChanged : names;
    session_->xrefNotice.clear();
    if (targets.empty() || !registry_.find("XRELOAD"))
        return;
    if (session_->runner->active())
        pressEscape();
    for (const std::string& n : targets) {
        submitText("XRELOAD");
        submitText(n);
    }
    std::erase_if(session_->xrefChanged, [&](const std::string& n) {
        return std::find(targets.begin(), targets.end(), n) != targets.end();
    });
}

std::vector<render::ImageDraw> AppCore::underlayImages() {
    std::vector<render::ImageDraw> out;
    if (!session_ || inPaperSpace())
        return out;
    const std::filesystem::path host = session_->file;
    auto list = underlay::collectImages(session_->doc, imageCache_, [&](std::string_view stored) {
        return xref::resolvePath(stored, host);
    });
    out.reserve(list.size());
    for (underlay::ImageInstance& im : list) {
        render::ImageDraw d;
        d.key = im.key;
        d.data = std::move(im.data);
        d.corners = im.corners;
        d.uMin = im.uMin;
        d.uMax = im.uMax;
        d.vMin = im.vMin;
        d.vMax = im.vMax;
        d.opacity = im.opacity;
        out.push_back(std::move(d));
    }
    return out;
}

void AppCore::enterViewport(const std::string& layoutName, const layout::Viewport& vp) {
    const BBox2 paper = vp.paper;
    const Vec2 center = vp.center;
    const double scaleN = vp.scaleN;
    const double halfW = paper.width() * scaleN / 2;
    const double halfH = paper.height() * scaleN / 2;
    leavePaper();
    // The annotation scale follows the viewport being edited, so new notes come out right.
    if (scaleN > 0 && currentAnnotationScale(session_->doc.header()) != scaleN &&
        !runner().active()) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.12g", scaleN);
        submitText("CANNOSCALE");
        submitText(buf);
    }
    zoomToBox({{center.x - halfW, center.y - halfH}, {center.x + halfW, center.y + halfH}});
    editingLayout_ = layoutName;
    editingViewport_ = vp.id;
    editEntryPixelsPerMm_ = camera_.pixelsPerMm;
    editEntryCenter_ = camera_.center;
    annoSyncRevision_ = 0;
}

void AppCore::commitViewportView() {
    if (editingLayout_.empty() || !session_ || !(editEntryPixelsPerMm_ > 0))
        return;
    const std::string name = editingLayout_;
    const int id = editingViewport_;
    const auto lay = layout::findLayout(session_->doc, name);
    const layout::Viewport* vp = lay ? lay->findViewport(id) : nullptr;
    if (!vp || vp->locked || !(camera_.pixelsPerMm > 0))
        return;
    const double k = editEntryPixelsPerMm_ / camera_.pixelsPerMm;
    const Vec2 moved{camera_.center.x - editEntryCenter_.x, camera_.center.y - editEntryCenter_.y};
    const double pixel = 1.0 / camera_.pixelsPerMm;  // under a pixel of panning is no change
    if (std::abs(k - 1) < 1e-9 && std::abs(moved.x) < pixel && std::abs(moved.y) < pixel)
        return;
    const double scaleN = vp->scaleN * k;
    const Vec2 center{vp->center.x + moved.x, vp->center.y + moved.y};
    auto tx = session_->doc.begin("Viewport view");
    if (auto r = layout::setViewportScale(tx, name, id, scaleN); !r) {
        print(r.error().message);
        return;
    }
    if (auto r = layout::setViewportCenter(tx, name, id, center); !r) {
        print(r.error().message);
        return;
    }
    ChangeSet cs = tx.commit();
    if (!cs.empty())
        undoStack().push("Viewport view", {std::move(cs)});
}

// While a locked viewport is being edited its scale is fixed, so the annotation scale follows it:
// a CANNOSCALE typed meanwhile is put back.
void AppCore::followLockedViewport() {
    if (editingLayout_.empty() || !session_ || runner().active())
        return;
    const std::uint64_t rev = session_->doc.revision();
    if (rev == annoSyncRevision_)
        return;
    annoSyncRevision_ = rev;
    const auto lay = layout::findLayout(session_->doc, editingLayout_);
    const layout::Viewport* vp = lay ? lay->findViewport(editingViewport_) : nullptr;
    if (!vp || !vp->locked || !(vp->scaleN > 0) ||
        currentAnnotationScale(session_->doc.header()) == vp->scaleN)
        return;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.12g", vp->scaleN);
    print(std::string("The viewport is locked: the annotation scale stays 1:") + buf + ".");
    submitText("CANNOSCALE");
    submitText(buf);
    annoSyncRevision_ = session_->doc.revision();
}

// While a layout viewport is edited from model space, the layers frozen in it are hidden there
// too, as on the sheet (VPLAYER, the VP Frz column of the Layers panel).
void AppCore::hideViewportFrozenLayers() {
    if (!session_)
        return;
    const std::string key = editingLayout_.empty()
                                ? std::string()
                                : editingLayout_ + ":" + std::to_string(editingViewport_) + ":" +
                                      std::to_string(session_->doc.revision());
    if (key == vpHiddenKey_ && session_ == vpHiddenSession_)
        return;
    vpHiddenKey_ = key;
    vpHiddenSession_ = session_;
    std::unordered_set<LayerId> hidden;
    if (!editingLayout_.empty())
        if (const auto lay = layout::findLayout(session_->doc, editingLayout_))
            if (const layout::Viewport* vp = lay->findViewport(editingViewport_))
                for (const auto& [id, l] : session_->doc.layers())
                    if (layout::frozenInViewport(session_->doc, *vp, id))
                        hidden.insert(id);
    const bool changed = hidden != session_->doc.viewHiddenLayers();
    session_->doc.setViewHiddenLayers(hidden);
    session_->sync->setExtraHiddenLayers(std::move(hidden));
    if (changed)
        pruneSelection();
}

// ZOOM nXP: sets the scale of the viewport on the sheet (the selected one, else the only one) or
// of the one being edited from model space to n times paper space, i.e. 1:(1/n).
void AppCore::applyZoomXp() {
    if (pendingZoomXp_ <= 0 || runner().active() || !session_)
        return;
    const double n = pendingZoomXp_;
    pendingZoomXp_ = 0;
    std::string name;
    int id = 0;
    const bool editing = !inPaperSpace();
    if (editing) {
        name = editingLayout_;
        id = editingViewport_;
    } else if (const auto lay = layout::findLayout(session_->doc, activeLayout_)) {
        name = activeLayout_;
        const std::string sel = setting("tmp.layout.viewport").value_or("");
        if (const auto colon = sel.find(':');
            colon != std::string::npos && sel.substr(0, colon) == name)
            id = lay->findViewport(std::atoi(sel.c_str() + colon + 1))
                     ? std::atoi(sel.c_str() + colon + 1)
                     : 0;
        if (!id && lay->viewports.size() == 1)
            id = lay->viewports.front().id;
    }
    if (name.empty() || !id) {
        print(
            "ZOOM nXP needs a viewport: click a viewport border on the sheet, or enter one with "
            "MSPACE.");
        return;
    }
    auto tx = session_->doc.begin("ZOOM XP");
    if (auto r = layout::setViewportScale(tx, name, id, 1.0 / n); !r) {
        print(r.error().message);
        return;
    }
    (void)tx.commit();
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6g", 1.0 / n);
    print("Viewport " + std::to_string(id) + " is now 1:" + buf + " (" + std::to_string(n) +
          "XP).");
    if (editing)
        if (const auto lay = layout::findLayout(session_->doc, name))
            if (const layout::Viewport* vp = lay->findViewport(id)) {
                const layout::Viewport copy = *vp;
                enterViewport(name, copy);
            }
}

// MSPACE: edit the model through a viewport (the selected one, else the only or first one).
// PSPACE: back to the sheet that was left.
void AppCore::applyPendingSpace() {
    applyZoomXp();
    if (pendingSpace_.empty() || runner().active() || !session_)
        return;
    const std::string what = std::move(pendingSpace_);
    pendingSpace_.clear();
    if (what == "tile1" || what == "tile0" || what == "next" || what == "prev") {
        const auto all = layout::layouts(session_->doc);
        if (what == "tile1") {
            setActiveLayout("");
        } else if (what == "tile0") {
            std::string name = lastLayout_;
            if (name.empty() || !layout::findLayout(session_->doc, name))
                name = all.empty() ? std::string() : all.front().name;
            if (name.empty())
                print("This drawing has no layouts. Use LAYOUT > New.");
            else if (!inPaperSpace())
                setActiveLayout(name);
        } else {
            // Tab strip order: Model first, then the layouts; it wraps round.
            std::vector<std::string> tabs{std::string()};
            for (const layout::Layout& l : all)
                tabs.push_back(l.name);
            size_t at = 0;
            for (size_t i = 0; i < tabs.size(); ++i)
                if (tabs[i] == activeLayout_)
                    at = i;
            at = what == "next" ? (at + 1) % tabs.size() : (at + tabs.size() - 1) % tabs.size();
            setActiveLayout(tabs[at]);
        }
        return;
    }
    if (what == "paper") {
        if (inPaperSpace()) {
            print("Already on the sheet.");
            return;
        }
        std::string name = lastLayout_;
        if (name.empty() || !layout::findLayout(session_->doc, name)) {
            const auto all = layout::layouts(session_->doc);
            name = all.empty() ? std::string() : all.front().name;
        }
        if (name.empty())
            print("This drawing has no layouts. Use LAYOUT > New.");
        else
            setActiveLayout(name);
        return;
    }
    if (!inPaperSpace()) {
        print("Already in model space.");
        return;
    }
    const auto lay = layout::findLayout(session_->doc, activeLayout_);
    if (!lay || lay->viewports.empty()) {
        print("\"" + activeLayout_ + "\" has no viewports. Use MVIEW to add one.");
        return;
    }
    const layout::Viewport* pick = nullptr;
    const std::string sel = setting("tmp.layout.viewport").value_or("");
    if (const auto colon = sel.find(':');
        colon != std::string::npos && sel.substr(0, colon) == activeLayout_)
        pick = lay->findViewport(std::atoi(sel.c_str() + colon + 1));
    for (const layout::Viewport& v : lay->viewports)
        if (!pick && v.on)
            pick = &v;
    if (pick)
        enterViewport(std::string(activeLayout_), *pick);
}

// ---- tiled model viewports ---------------------------------------------------------------------

namespace {
struct TileRect {
    double x0, y0, x1, y1;
};
// The tiles of a VPORTS layout code and which one becomes active; empty for an unknown code.
std::vector<TileRect> tileRectsFor(std::string_view code, std::size_t& active) {
    active = 0;
    if (code == "1")
        return {{0, 0, 1, 1}};
    if (code == "2V")
        return {{0, 0, .5, 1}, {.5, 0, 1, 1}};
    if (code == "2H")
        return {{0, 0, 1, .5}, {0, .5, 1, 1}};
    if (code == "3R") {
        active = 2;
        return {{0, 0, .5, .5}, {0, .5, .5, 1}, {.5, 0, 1, 1}};
    }
    if (code == "3L")
        return {{0, 0, .5, 1}, {.5, 0, 1, .5}, {.5, .5, 1, 1}};
    if (code == "3A")
        return {{0, 0, 1, .5}, {0, .5, .5, 1}, {.5, .5, 1, 1}};
    if (code == "3B") {
        active = 2;
        return {{0, 0, .5, .5}, {.5, 0, 1, .5}, {0, .5, 1, 1}};
    }
    if (code == "3V")
        return {{0, 0, 1.0 / 3, 1}, {1.0 / 3, 0, 2.0 / 3, 1}, {2.0 / 3, 0, 1, 1}};
    if (code == "3H")
        return {{0, 0, 1, 1.0 / 3}, {0, 1.0 / 3, 1, 2.0 / 3}, {0, 2.0 / 3, 1, 1}};
    if (code == "4")
        return {{0, 0, .5, .5}, {.5, 0, 1, .5}, {0, .5, .5, 1}, {.5, .5, 1, 1}};
    return {};
}
constexpr const char* kVportsPrefix = "tbcad.vports.";
}  // namespace

bool AppCore::setTileLayout(std::string_view code) {
    std::size_t active = 0;
    const std::vector<TileRect> rects = tileRectsFor(code, active);
    if (rects.empty())
        return false;
    // The whole drawing area, worked out from the active tile.
    const ViewTile& was = tiles_[activeTile_];
    const double fullW = camera_.viewportW / std::max(was.x1 - was.x0, 1e-6);
    const double fullH = camera_.viewportH / std::max(was.y1 - was.y0, 1e-6);
    const Vec2 fullOrigin{viewportOrigin_.x - was.x0 * fullW, viewportOrigin_.y - was.y0 * fullH};
    tiles_.clear();
    for (const TileRect& r : rects)
        tiles_.push_back(ViewTile{r.x0, r.y0, r.x1, r.y1, camera_.center, camera_.pixelsPerMm});
    activeTile_ = active;
    tileLayout_ = std::string(code);
    const ViewTile& now = tiles_[activeTile_];
    // The active tile keeps the view's centre and zoom; set its size here so the window's next
    // size report does not move the view.
    camera_.viewportW = std::max(1, static_cast<int>(std::lround(fullW * (now.x1 - now.x0))));
    camera_.viewportH = std::max(1, static_cast<int>(std::lround(fullH * (now.y1 - now.y0))));
    viewportOrigin_ = {fullOrigin.x + now.x0 * fullW, fullOrigin.y + now.y0 * fullH};
    return true;
}

void AppCore::activateTile(std::size_t i, double x, double y, int w, int h) {
    if (i >= tiles_.size() || i == activeTile_)
        return;
    tiles_[activeTile_].center = camera_.center;
    tiles_[activeTile_].pixelsPerMm = camera_.pixelsPerMm;
    activeTile_ = i;
    camera_.center = tiles_[i].center;
    camera_.pixelsPerMm = tiles_[i].pixelsPerMm;
    camera_.viewportW = std::max(w, 1);
    camera_.viewportH = std::max(h, 1);
    viewportOrigin_ = {x, y};
    haveOrigin_ = true;
    fitPending_ = 0;
}

render::Camera2D AppCore::tileCamera(std::size_t i, int w, int h) const {
    render::Camera2D cam = camera_;
    if (i < tiles_.size() && i != activeTile_) {
        cam.center = tiles_[i].center;
        cam.pixelsPerMm = tiles_[i].pixelsPerMm;
    }
    cam.viewportW = std::max(w, 1);
    cam.viewportH = std::max(h, 1);
    return cam;
}

// -VPORTS: a layout code, or save:/restore:/delete:<name>, or list.
void AppCore::applyPendingVports() {
    if (pendingVports_.empty() || runner().active() || !session_)
        return;
    const std::vector<std::string> todo = std::exchange(pendingVports_, {});
    for (const std::string& what : todo) {
        if (inPaperSpace() || !editingLayout_.empty()) {
            print("Tiled viewports are for model space; on a layout use MVIEW.");
            continue;
        }
        auto lowerName = [](std::string n) {
            for (char& c : n)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return n;
        };
        auto keyOf = [&](const std::string& name) -> std::string {
            for (const auto& [k, v] : session_->doc.header().custom)
                if (k.rfind(kVportsPrefix, 0) == 0 &&
                    lowerName(k.substr(std::strlen(kVportsPrefix))) == lowerName(name))
                    return k;
            return {};
        };
        if (what == "list") {
            std::size_t n = 0;
            for (const auto& [k, v] : session_->doc.header().custom)
                if (k.rfind(kVportsPrefix, 0) == 0) {
                    print("  " + k.substr(std::strlen(kVportsPrefix)) + "  (" +
                          v.substr(0, v.find('\n')) + ")");
                    ++n;
                }
            print("Current configuration: " + tileLayout_ + (n ? "" : "; no saved ones."));
        } else if (what.rfind("save:", 0) == 0) {
            const std::string name = what.substr(5);
            tiles_[activeTile_].center = camera_.center;
            tiles_[activeTile_].pixelsPerMm = camera_.pixelsPerMm;
            std::string text = tileLayout_ + "\n" + std::to_string(activeTile_) + "\n";
            for (const ViewTile& t : tiles_) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "%.12g %.12g %.12g\n", t.center.x, t.center.y,
                              t.pixelsPerMm);
                text += buf;
            }
            const std::string old = keyOf(name);
            auto tx = session_->doc.begin("VPORTS Save");
            (void)tx.modifyHeader([&](Header& h) {
                if (!old.empty())
                    h.custom.erase(old);
                h.custom[kVportsPrefix + name] = text;
            });
            ChangeSet cs = tx.commit();
            if (!cs.empty())
                undoStack().push("VPORTS Save", {std::move(cs)});
            print("Viewport configuration \"" + name + "\" saved.");
        } else if (what.rfind("delete:", 0) == 0) {
            const std::string key = keyOf(what.substr(7));
            if (key.empty()) {
                print("No viewport configuration named \"" + what.substr(7) + "\".");
                continue;
            }
            auto tx = session_->doc.begin("VPORTS Delete");
            (void)tx.modifyHeader([&](Header& h) { h.custom.erase(key); });
            ChangeSet cs = tx.commit();
            if (!cs.empty())
                undoStack().push("VPORTS Delete", {std::move(cs)});
        } else if (what.rfind("restore:", 0) == 0) {
            const std::string key = keyOf(what.substr(8));
            if (key.empty()) {
                print("No viewport configuration named \"" + what.substr(8) + "\".");
                continue;
            }
            std::istringstream in(session_->doc.header().custom.at(key));
            std::string code;
            std::size_t active = 0;
            if (!std::getline(in, code) || !(in >> active) || !setTileLayout(code)) {
                print("The viewport configuration is damaged.");
                continue;
            }
            for (ViewTile& t : tiles_) {
                double x = 0, y = 0, p = 0;
                if (in >> x >> y >> p && p > 0) {
                    t.center = {x, y};
                    t.pixelsPerMm = p;
                }
            }
            if (active < tiles_.size()) {
                activeTile_ = active;
            }
            camera_.center = tiles_[activeTile_].center;
            camera_.pixelsPerMm = tiles_[activeTile_].pixelsPerMm;
        } else if (!setTileLayout(what)) {
            print("Unknown viewport layout \"" + what + "\".");
        }
    }
}

void AppCore::applyPendingLayout() {
    applyPendingVports();
    applyPendingSpace();
    if (pendingLayout_.empty() || runner().active())
        return;
    const std::string name = std::move(pendingLayout_);
    pendingLayout_.clear();
    setActiveLayout(name);
}

void AppCore::updateFrame(std::chrono::steady_clock::time_point now) {
    applyPendingLayout();
    {
        profile::EditTimer t("frame: autosave");
        tickAutosave(now);
    }
    tickXrefs(now);
    if (inPaperSpace()) {
        if (!layout::findLayout(session_->doc, activeLayout_)) {
            leavePaper();  // the layout was deleted
        } else if (session_->doc.revision() != paperRevision_) {
            paperRevision_ = session_->doc.revision();
            paper_.build(session_->doc, activeLayout_);
            rebuildPaperIndex();
            pruneSelection();
        }
        if (inPaperSpace())
            return;
    }
    followLockedViewport();
    hideViewportFrozenLayers();
    session_->sync->setLockedFade(settings_.getNumber("view.lockFade", 60));
    {
        profile::EditTimer t("frame: re-tile");
        retileIfFragmented();
    }
    {
        // Objects picked so far at "Select objects:" show as selected, so the drafter sees them.
        const std::vector<EntityId>& picked = runner().collected();
        if (picked.size() != shownPicked_) {
            shownPicked_ = picked.size();
            std::vector<EntityId> all = selection_;
            all.insert(all.end(), picked.begin(), picked.end());
            scene_.setSelected(all);
        }
    }
    if (!flashIds_.empty() && now >= flashUntil_) {
        flashIds_.clear();
        scene_.setHighlighted(hoverId_ ? std::vector<EntityId>{hoverId_} : std::vector<EntityId>{});
    }
    if (render::Scene::needsRetessellation(lastTessellatedFor_, camera_) ||
        (staleWaiting_ && !(lastTessellatedFor_ == camera_))) {
        session_->sync->setPixelSize(camera_.pixelSizeMm());
        session_->sync->refreshStale(camera_);
        staleWaiting_ = session_->sync->staleOffscreen();
        // Keep the zoom the waiting objects were measured against: only a pan re-checks.
        const bool zoomed = render::Scene::needsRetessellation(lastTessellatedFor_, camera_);
        if (zoomed || !staleWaiting_)
            lastTessellatedFor_ = camera_;
        else
            lastTessellatedFor_.center = camera_.center;
    }
    // Geometry of a drawing-sized edit is built a slice per frame, what the camera sees first.
    // (about two frames' worth: the rest follows over the next frames)
    std::optional<profile::EditTimer> pumpTimer;
    pumpTimer.emplace("frame: build waiting objects", session_->sync->pendingCount());
    const std::size_t pumped = session_->sync->pumpPending(camera_, 8000, 30.0);
    pumpTimer.reset();
    // (objects out of view may have been built for a far zoom: rebuilt when the camera comes)
    staleWaiting_ = staleWaiting_ || session_->sync->staleOffscreen();
    if (pumped > 0) {
        ++generation_;
        trimWhenSettled_ = true;
    }
    if (trimWhenSettled_ && !sceneBuilding()) {
        // The batches of a big drawing were grown piece by piece and hold up to twice the room
        // they use: give it back once, when the drawing is complete.
        trimWhenSettled_ = false;
        if (scene_.entityCount() >= 100000)
            scene_.trimMemory();
    }
}

fs::path AppCore::thumbnailFile(const std::string& drawingPath) const {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : drawingPath) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char name[24];
    std::snprintf(name, sizeof name, "%016llx.png", static_cast<unsigned long long>(h));
    return paths_.dataDir / "thumbnails" / name;
}

void AppCore::wantThumbnail(const fs::path& path) {
    thumbnailFor_ = platform::pathToUtf8(path);
    thumbnailDoc_ = document().uid();
}

std::string AppCore::pendingThumbnail() const {
    if (thumbnailFor_.empty() || document().uid() != thumbnailDoc_ || sceneBuilding())
        return {};
    return thumbnailFor_;
}

bool AppCore::sceneBuilding() const {
    return session_->sync->pendingCount() > 0;
}

// Tiles are sized when a drawing opens. A drawing that grew a lot since (generated, pasted,
// inserted) can end up with thousands of tiny tiles, each drawn with several draw calls, which is
// what makes a big view slow. Re-tile once when that happens.
void AppCore::retileIfFragmented() {
    const std::size_t count = scene_.entityCount();
    if (count < 20000 || scene_.tiles().size() < 512)
        return;
    if (count < retileCheckedAt_ * 2 && retileCheckedAt_ != 0)
        return;
    retileCheckedAt_ = count;
    const double wanted = render::Scene::suggestTileSize(drawingExtents());
    if (wanted < scene_.tileSize() * 1.9)
        return;
    session_->sync->setPixelSize(camera_.pixelSizeMm());
    session_->sync->rebuildAll(wanted);
    scene_.setSelected(selection_);
    lastTessellatedFor_ = camera_;
    ++generation_;
}

std::optional<std::vector<EntityId>> AppCore::entitiesInWindow(const BBox2& window) const {
    return spatialIndex().queryIds(window);
}

std::optional<std::vector<EntityId>> AppCore::selectShape(SelectShape shape,
                                                          const std::vector<Vec2>& pts) {
    switch (shape) {
        case SelectShape::Window:
        case SelectShape::Crossing: {
            if (pts.size() < 2)
                return std::vector<EntityId>{};
            BBox2 box;
            box.expand(pts[0]);
            box.expand(pts[1]);
            return selectInBox(session_->doc, spatialIndex(), box, shape == SelectShape::Crossing);
        }
        case SelectShape::WPolygon:
        case SelectShape::CPolygon:
            return selectInPolygon(session_->doc, spatialIndex(), pts,
                                   shape == SelectShape::CPolygon);
        case SelectShape::Fence:
            return selectAlongFence(session_->doc, spatialIndex(), pts);
    }
    return std::nullopt;
}

cmd::ViewState AppCore::view() const {
    cmd::ViewState v;
    v.center = camera_.center;
    v.scale = camera_.pixelSizeMm();
    v.sizePx = {static_cast<double>(camera_.viewportW), static_cast<double>(camera_.viewportH)};
    return v;
}

void AppCore::setView(const cmd::ViewState& v) {
    camera_.center = v.center;
    if (v.scale > 0 && std::isfinite(v.scale))
        camera_.pixelsPerMm = std::clamp(1.0 / v.scale, render::Camera2D::kMinPixelsPerMm,
                                         render::Camera2D::kMaxPixelsPerMm);
}

// ---- pointer -----------------------------------------------------------------------------------

double AppCore::apertureMm() const {
    return settings_.getNumber("pick.aperturePx", 6) * camera_.pixelSizeMm();
}

cmd::RawInput AppCore::makeRaw(const PointerState& p) const {
    cmd::RawInput raw;
    raw.worldCursor = camera_.screenToWorld(p.screen);
    raw.screenCursor = p.screen;
    raw.pixelSize = camera_.pixelSizeMm();
    raw.shift = p.shift;
    raw.ctrl = p.ctrl;
    return raw;
}

void AppCore::updateHover(const PointerState& p) {
    std::vector<EntityId> hover;
    bool wantsHover =
        !runner().active() || (prompt_ && (prompt_->kind == cmd::PromptInfo::Kind::Selection ||
                                           prompt_->kind == cmd::PromptInfo::Kind::Entity));
    if (wantsHover) {
        Vec2 w = camera_.screenToWorld(p.screen);
        if (auto id = pickEntity(session_->doc, spatialIndex(), w, apertureMm()))
            hover.push_back(*id);
    }
    hoverId_ = hover.empty() ? EntityId{} : hover.front();
    if (inPaperSpace())
        return;  // the sheet draws its own highlight (buildOverlay)
    hover.insert(hover.end(), flashIds_.begin(), flashIds_.end());
    hover.insert(hover.end(), dragPreview_.begin(), dragPreview_.end());
    scene_.setHighlighted(hover);
}

void AppCore::flashNew(std::vector<EntityId> ids) {
    if (inPaperSpace())
        return;
    flashIds_ = std::move(ids);
    flashUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    std::vector<EntityId> all = flashIds_;
    if (hoverId_)
        all.push_back(hoverId_);
    scene_.setHighlighted(all);
}

std::string AppCore::describeEntity(EntityId id) const {
    if (!session_)
        return {};
    const Entity* e = session_->doc.find(id);
    if (!e)
        return {};
    static const char* const kNames[] = {"Line",    "Circle",    "Arc",    "Polyline", "Point",
                                         "Ellipse", "Spline",    "Text",   "MText",    "Hatch",
                                         "Block",   "Dimension", "Leader", "Proxy",    "Table",
                                         "Ray",     "Multiline"};
    const auto k = static_cast<std::size_t>(kind(*e));
    // As AutoCAD's rollover tooltip: the type, then colour, layer and linetype, then the size.
    std::string out = k < std::size(kNames) ? kNames[k] : "Object";
    const EntityCommon& c = e->common;
    std::string color;
    switch (c.color.kind) {
        case ColorKind::ByLayer:
            color = "ByLayer";
            break;
        case ColorKind::ByBlock:
            color = "ByBlock";
            break;
        case ColorKind::ByParent:
            color = "ByParent";
            break;
        case ColorKind::Index: {
            static const char* const names[] = {"red",  "yellow",  "green", "cyan",
                                                "blue", "magenta", "white"};
            const int i = c.color.indexValue();
            color = i >= 1 && i <= 7 ? names[i - 1] : std::to_string(i);
            break;
        }
        case ColorKind::Rgb:
            color = std::to_string(c.color.r) + "," + std::to_string(c.color.g) + "," +
                    std::to_string(c.color.b);
            break;
    }
    out += "\nColor: " + color;
    if (const Layer* l = session_->doc.layer(c.layer))
        out += "\nLayer: " + l->name;
    std::string linetype = "ByLayer";
    if (c.linetype == kLinetypeByBlock)
        linetype = "ByBlock";
    else if (c.linetype == kLinetypeByParent)
        linetype = "ByParent";
    else if (c.linetype != kLinetypeByLayer)
        if (const Linetype* lt = session_->doc.linetype(c.linetype))
            linetype = lt->name;
    out += "\nLinetype: " + linetype;
    char buf[64];
    if (const auto* circle = std::get_if<CircleData>(&e->data)) {
        std::snprintf(buf, sizeof buf, "\nRadius: %.2f mm", circle->radius);
        out += buf;
    } else if (const double len = geom::length(*e); len > 0) {
        std::snprintf(buf, sizeof buf, "\nLength: %.2f mm", len);
        out += buf;
    }
    return out;
}

void AppCore::pointerMoved(const PointerState& p) {
    lastPointer_ = p;
    cursorInside_ = true;
    cmd::RawInput raw = makeRaw(p);
    cursorWorld_ = raw.worldCursor;
    if (runner().active())
        runner().pointerMoved(raw);
    updateHover(p);
    if (windowCorner_)
        setSelectionDrag(std::make_pair(*windowCorner_, p.screen));
}

void AppCore::pointerLeft() {
    cursorInside_ = false;
    scene_.setHighlighted({});
    hoverId_ = {};
}

void AppCore::applyPick(const std::vector<EntityId>& picked, bool remove) {
    // A picked member brings its whole group along (GROUP).
    const std::vector<EntityId> ids = expandGroups(session_->doc, picked);
    // Picks come from the selection queries, which already skip hidden and locked objects, so
    // only the extra group members need the check; a big window select then skips 150k lookups.
    const bool trusted = ids.size() == picked.size();
    std::vector<EntityId> sel;
    if (remove) {
        std::unordered_set<EntityId> drop(ids.begin(), ids.end());
        sel.reserve(selection_.size());
        for (EntityId id : selection_)
            if (!drop.count(id))
                sel.push_back(id);
    } else if (selection_.empty()) {
        sel = ids;
        // Strictly increasing ids cannot repeat; anything else needs the de-duplication.
        if (!std::is_sorted(sel.begin(), sel.end()) ||
            std::adjacent_find(sel.begin(), sel.end()) != sel.end()) {
            std::unordered_set<EntityId> seen;
            std::vector<EntityId> unique;
            unique.reserve(sel.size());
            for (EntityId id : sel)
                if (seen.insert(id).second)
                    unique.push_back(id);
            sel = std::move(unique);
        }
    } else {
        std::unordered_set<EntityId> have(selection_.begin(), selection_.end());
        sel = selection_;
        for (EntityId id : ids)
            if (have.insert(id).second)
                sel.push_back(id);
    }
    if (trusted) {
        selection_ = std::move(sel);
        ++selectionRevision_;
        scene_.setSelected(selection_);
    } else {
        setSelection(std::move(sel));
    }
}

void AppCore::applyUserPick(const std::vector<EntityId>& ids, bool shift) {
    if (setting("selection.pickadd").value_or("2") != "0") {
        applyPick(ids, shift);
        return;
    }
    if (!shift) {  // PICKADD 0: the new pick replaces the selection
        if (!ids.empty())
            setSelection(ids);
        return;
    }
    std::unordered_set<EntityId> have(selection_.begin(), selection_.end());
    std::vector<EntityId> drop, add;
    for (EntityId id : ids)
        (have.count(id) ? drop : add).push_back(id);
    if (!drop.empty())
        applyPick(drop, true);
    if (!add.empty())
        applyPick(add, false);
}

PanelContext AppCore::makePanelContext() {
    PanelContext pc;
    pc.doc = &session_->doc;
    pc.host = this;
    pc.selection = &selection_;
    pc.camera = &camera_;
    pc.feedback = &feedback_;
    pc.cursorWorld = cursorWorld_;
    pc.apertureMm = apertureMm();
    pc.commandActive = runner().active();
    pc.runCommand = [this](const std::string& line) { submitPanelText(line); };
    pc.cancelCommand = [this] {
        if (runner().active())
            pressEscape();
    };
    return pc;
}

bool AppCore::pressOnGrip(const PointerState& p) {
    if (runner().active() || !ui_ || ui_->idleClickHooks().empty())
        return false;
    const Vec2 world = makeRaw(p).worldCursor;
    PanelContext pc = makePanelContext();
    for (const auto& hook : ui_->idleClickHooks())
        if (hook(pc, world, apertureMm(), p.shift, p.ctrl))
            return true;
    return false;
}

bool AppCore::hoverMenu(HoverMenu& out) {
    if (runner().active() || !ui_ || ui_->hoverMenus().empty())
        return false;
    PanelContext pc = makePanelContext();
    for (const auto& hook : ui_->hoverMenus()) {
        out = HoverMenu{};
        if (hook(pc, out))
            return true;
    }
    return false;
}

void AppCore::click(const PointerState& p) {
    cmd::RawInput raw = makeRaw(p);
    Vec2 world = raw.worldCursor;
    cmd::CommandRunner& run = runner();
    if (run.active()) {
        if (!prompt_)
            return;
        switch (prompt_->kind) {
            case cmd::PromptInfo::Kind::Point:
            case cmd::PromptInfo::Kind::Distance:
            case cmd::PromptInfo::Kind::Angle:
                run.submit(cmd::Input::click(raw));
                setSetting("drafting.osnap.once", "");  // a snap override serves one point
                break;
            case cmd::PromptInfo::Kind::Entity:
            case cmd::PromptInfo::Kind::Selection:
                if (prompt_->kind == cmd::PromptInfo::Kind::Selection && run.selectShapePending()) {
                    run.submit(cmd::Input::click(raw));
                    break;
                }
                if (auto id = pickEntity(session_->doc, spatialIndex(), world, apertureMm()))
                    run.submit(cmd::Input::pick(*id, world, p.shift));
                break;
            default:
                break;
        }
        return;
    }
    if (ui_ && !ui_->idleClickHooks().empty()) {
        PanelContext pc = makePanelContext();
        for (const auto& hook : ui_->idleClickHooks()) {
            if (hook(pc, world, apertureMm(), p.shift, p.ctrl))
                return;
        }
    }
    auto id = pickEntity(session_->doc, spatialIndex(), world, apertureMm());
    if (!id) {
        if (!p.shift)
            clearSelection();
        return;
    }
    applyUserPick({*id}, p.shift);
}

void AppCore::doubleClick(const PointerState& p) {
    bool onSheetObject = false;
    if (inPaperSpace() && !runner().active()) {
        const Vec2 at = camera_.screenToWorld(p.screen);
        if (auto id = pickEntity(session_->doc, spatialIndex(), at, apertureMm())) {
            onSheetObject = true;
            // Inside a viewport only notes, dimensions and tables on the sheet keep the double
            // click for themselves (to edit them). Hatches, frames, title blocks and images
            // drawn over or behind the viewport would otherwise make it impossible to enter.
            bool inViewport = false;
            if (auto lay = layout::findLayout(session_->doc, activeLayout_))
                for (const layout::Viewport& vp : lay->viewports)
                    inViewport = inViewport || (vp.on && layout::viewportContains(vp, at));
            const Entity* e = session_->doc.find(*id);
            if (inViewport && e) {
                const EntityKind k = kind(*e);
                onSheetObject = k == EntityKind::Text || k == EntityKind::MText ||
                                k == EntityKind::Dimension || k == EntityKind::Leader ||
                                k == EntityKind::MLeader || k == EntityKind::Table;
            }
        }
    }
    if (inPaperSpace() && !onSheetObject && !runner().active()) {
        // A double click inside a viewport goes to model space, showing what the viewport shows
        // there, so the drawing can be edited from the sheet.
        const Vec2 at = camera_.screenToWorld(p.screen);
        if (auto lay = layout::findLayout(session_->doc, activeLayout_)) {
            for (const layout::Viewport& vp : lay->viewports) {
                if (!vp.on || !layout::viewportContains(vp, at))
                    continue;
                enterViewport(std::string(activeLayout_), vp);
                return;
            }
        }
        return;
    }
    if (runner().active()) {
        click(p);
        return;
    }
    Vec2 world = makeRaw(p).worldCursor;
    auto id = pickEntity(session_->doc, spatialIndex(), world, apertureMm());
    const Entity* e = id ? session_->doc.find(*id) : nullptr;
    if (!e) {
        click(p);
        return;
    }
    applyPick({*id}, false);
    if (setting(kDoubleClickEditKey).value_or("1") == "0")
        return;  // DBLCLKEDIT off: the double click only selects
    // The action set for the object's type (Customize page, as AutoCAD's CUI): an edit command
    // that takes the picked object, or the Properties panel.
    const std::string action = doubleClickAction(settings_, session_->doc, *e);
    if (action == kOpenProperties || !registry().find(commandLabel(action)))
        openPanel("properties");
    else
        submitText(action);
}

std::optional<std::size_t> AppCore::dragSelectCount() {
    if (!drag_ || !session_)
        return std::nullopt;
    const auto [a, b] = *drag_;
    const std::uint64_t rev = session_->doc.revision();
    if (a.x != dragCount_.a.x || a.y != dragCount_.a.y || b.x != dragCount_.b.x ||
        b.y != dragCount_.b.y || rev != dragCount_.revision) {
        BBox2 box;
        box.expand(camera_.screenToWorld(a));
        box.expand(camera_.screenToWorld(b));
        std::vector<EntityId> ids =
            selectInBox(session_->doc, spatialIndex(), box, dragIsCrossing(a.x, b.x));
        dragCount_ = {a, b, rev, ids.size(), std::move(ids)};
    }
    return dragCount_.count;
}

void AppCore::setSelectionDrag(std::optional<std::pair<Vec2, Vec2>> corners) {
    drag_ = corners;
    std::vector<EntityId> preview;
    if (drag_ && session_ && !inPaperSpace() &&
        settings_.getString("selection.preview", "1") != "0") {
        dragSelectCount();
        preview = dragCount_.ids;
        constexpr std::size_t kMaxPreview = 5000;  // keeps the highlight cheap on huge drawings
        if (preview.size() > kMaxPreview)
            preview.clear();
    }
    if (preview.empty() && dragPreview_.empty())
        return;
    dragPreview_ = std::move(preview);
    std::vector<EntityId> all = dragPreview_;
    all.insert(all.end(), flashIds_.begin(), flashIds_.end());
    scene_.setHighlighted(all);
}

void AppCore::dragSelect(Vec2 a, Vec2 b, const PointerState& p) {
    const bool crossing = dragIsCrossing(a.x, b.x);
    Vec2 wa = camera_.screenToWorld(a);
    Vec2 wb = camera_.screenToWorld(b);
    BBox2 box;
    box.expand(wa);
    box.expand(wb);
    std::vector<EntityId> ids = selectInBox(session_->doc, spatialIndex(), box, crossing);
    cmd::CommandRunner& run = runner();
    if (run.active()) {
        if (prompt_ && prompt_->kind == cmd::PromptInfo::Kind::Selection && !ids.empty()) {
            cmd::Input in = cmd::Input::select(std::move(ids));
            in.raw.shift = p.shift;
            run.submit(in);
        }
        return;
    }
    applyUserPick(ids, p.shift);
}

bool AppCore::dragPicksWindow() const {
    return setting("selection.pickdrag").value_or("1") != "0";
}

bool AppCore::windowClick(const PointerState& p) {
    if (dragPicksWindow() || !session_) {
        windowCorner_.reset();
        return false;
    }
    if (windowCorner_) {
        const Vec2 first = *windowCorner_;
        windowCorner_.reset();
        setSelectionDrag(std::nullopt);
        dragSelect(first, p.screen, p);
        return true;
    }
    // Only a click that hits no object (and no point prompt) starts a window.
    if (runner().active() && !(prompt_ && prompt_->kind == cmd::PromptInfo::Kind::Selection &&
                               !runner().selectShapePending()))
        return false;
    if (pickEntity(session_->doc, spatialIndex(), camera_.screenToWorld(p.screen), apertureMm()))
        return false;
    windowCorner_ = p.screen;
    return true;
}

void AppCore::lassoSelect(const std::vector<Vec2>& screenPath, const PointerState& p) {
    if (screenPath.size() < 3)
        return;
    std::vector<Vec2> poly;
    poly.reserve(screenPath.size());
    for (const Vec2& s : screenPath)
        poly.push_back(camera_.screenToWorld(s));
    const bool crossing = dragIsCrossing(screenPath.front().x, screenPath.back().x);
    std::vector<EntityId> ids = selectInPolygon(session_->doc, spatialIndex(), poly, crossing);
    cmd::CommandRunner& run = runner();
    if (run.active()) {
        if (prompt_ && prompt_->kind == cmd::PromptInfo::Kind::Selection && !ids.empty()) {
            cmd::Input in = cmd::Input::select(std::move(ids));
            in.raw.shift = p.shift;
            run.submit(in);
        }
        return;
    }
    applyUserPick(ids, p.shift);
}

void AppCore::cycleClick(const PointerState& p) {
    const Vec2 world = makeRaw(p).worldCursor;
    const double aperture = apertureMm();
    cmd::CommandRunner& run = runner();
    const bool pickPrompt = prompt_ && (prompt_->kind == cmd::PromptInfo::Kind::Entity ||
                                        prompt_->kind == cmd::PromptInfo::Kind::Selection);
    if (run.active() && !pickPrompt)
        return click(p);
    // A new spot starts a new stack; the same spot goes on to the next object in it.
    if (cycleStack_.empty() || distance(world, cycleAt_) > aperture) {
        cycleStack_ = pickAllAt(session_->doc, spatialIndex(), world, aperture);
        cycleCurrent_.reset();
        cycleAt_ = world;
    }
    auto id = cycleNext(cycleStack_, cycleCurrent_);
    if (!id) {
        if (!run.active() && !p.shift)
            clearSelection();
        return;
    }
    cycleCurrent_ = id;
    if (run.active())
        run.submit(cmd::Input::pick(*id, world, p.shift));
    else if (p.shift)
        applyPick({*id}, true);
    else
        setSelection({*id});  // cycling replaces the previous pick
}

std::vector<EntityId> AppCore::objectsAt(const PointerState& p) {
    const bool pickPrompt = prompt_ && (prompt_->kind == cmd::PromptInfo::Kind::Entity ||
                                        prompt_->kind == cmd::PromptInfo::Kind::Selection);
    if (runner().active() && !pickPrompt)
        return {};
    return pickAllAt(session_->doc, spatialIndex(), makeRaw(p).worldCursor, apertureMm());
}

void AppCore::pickFromList(EntityId id, const PointerState& p) {
    if (!session_->doc.find(id))
        return;
    cmd::CommandRunner& run = runner();
    if (run.active())
        run.submit(cmd::Input::pick(id, makeRaw(p).worldCursor, p.shift));
    else
        applyPick({id}, p.shift);
}

void AppCore::highlightCandidate(EntityId id) {
    hoverId_ = id;
    if (inPaperSpace())
        return;
    std::vector<EntityId> all = flashIds_;
    if (id)
        all.push_back(id);
    scene_.setHighlighted(all);
}

// ---- selection ---------------------------------------------------------------------------------

void AppCore::setSelection(std::vector<EntityId> ids) {
    std::vector<EntityId> clean;
    clean.reserve(ids.size());
    std::unordered_set<EntityId> seen;
    for (EntityId id : ids) {
        const Entity* e = session_->doc.find(id);
        if (e && isVisible(session_->doc, *e) && seen.insert(id).second)
            clean.push_back(id);
    }
    selection_ = std::move(clean);
    ++selectionRevision_;
    scene_.setSelected(selection_);
}

void AppCore::selectAll() {
    // Model space lists each object once and isSelectable() implies visible, so the checks of
    // setSelection() would only repeat this loop.
    // Neighbours usually share layer and level, so the last answer is reused.
    const Document& doc = session_->doc;
    const std::vector<EntityId> ids = doc.modelSpace();
    std::vector<EntityId> all;
    all.reserve(ids.size());
    LayerId lastLayer{};
    LevelId lastLevel{};
    bool lastOk = false, haveLast = false;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e || !e->common.visible)
            continue;
        if (!haveLast || e->common.layer != lastLayer || e->common.level != lastLevel) {
            const EffectiveLayer layer = doc.effective(e->common);
            lastOk = layer.visible() && !layer.locked &&
                     !doc.viewHiddenLayers().contains(e->common.layer);
            lastLayer = e->common.layer;
            lastLevel = e->common.level;
            haveLast = true;
        }
        if (lastOk)
            all.push_back(id);
    }
    selection_ = std::move(all);
    ++selectionRevision_;
    scene_.setSelected(selection_);
}

void AppCore::pruneSelection() {
    if (selection_.empty())
        return;
    std::vector<EntityId> keep;
    keep.reserve(selection_.size());
    for (EntityId id : selection_) {
        const Entity* e = session_->doc.find(id);
        if (e && isSelectable(session_->doc, *e))
            keep.push_back(id);
    }
    if (keep.size() != selection_.size()) {
        selection_ = std::move(keep);
        ++selectionRevision_;
        scene_.setSelected(selection_);
    }
}

// ---- overlay -----------------------------------------------------------------------------------

void AppCore::buildOverlay(render::Overlay& overlay, const UiRegistry* ui) {
    overlay.clear();
    // Command preview (rubber band, ghost geometry).
    if (hasPreview_) {
        render::OverlayStyle st;
        const Rgba ink = inPaperSpace() ? Rgba{20, 20, 20, 255} : Rgba{255, 255, 255, 255};
        st.color = ink;
        st.widthPx = 1.0;
        st.dashPx = 6;
        st.gapPx = 4;
        for (const auto& item : preview_.items) {
            if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                if (pl->pts.size() >= 2)
                    overlay.polyline(render::Space::World, pl->pts, pl->closed, st);
            } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
                // Filled shapes (text glyphs) are previewed as solid outlines.
                render::OverlayStyle solid = st;
                solid.dashPx = 0;
                solid.gapPx = 0;
                for (const auto& ring : fill->rings)
                    if (ring.size() >= 2)
                        overlay.polyline(render::Space::World, ring, true, solid);
            } else if (const auto* pts = std::get_if<DisplayPoints>(&item)) {
                for (const Vec2& pt : pts->pts)
                    overlay.marker(render::Space::World, pt, render::MarkerKind::Cross, 6, ink);
            }
        }
    }
    // Tracking guide and snap marker from the point filters.
    if (feedback_.active) {
        if (feedback_.guide) {
            render::OverlayStyle g;
            g.color = {120, 220, 120, 200};
            g.dashPx = 8;
            g.gapPx = 5;
            overlay.line(render::Space::World, feedback_.guide->first, feedback_.guide->second, g);
        }
        // With a UI registry the drafting module draws the snap marker itself.
        if (!ui && !feedback_.marker.empty()) {
            render::MarkerKind mk = render::MarkerKind::Square;
            if (feedback_.marker == "midpoint")
                mk = render::MarkerKind::Triangle;
            else if (feedback_.marker == "center")
                mk = render::MarkerKind::Circle;
            else if (feedback_.marker == "intersection")
                mk = render::MarkerKind::X;
            else if (feedback_.marker == "node" || feedback_.marker == "insertion")
                mk = render::MarkerKind::Cross;
            overlay.marker(render::Space::World, feedback_.markerPos, mk, 10, theme_.snap);
        }
    }
    if (drag_) {
        const bool crossing = dragIsCrossing(drag_->first.x, drag_->second.x);
        const Rgba tone = crossing ? theme_.crossing : theme_.select;
        overlay.selectionWindow(drag_->first, drag_->second, crossing, tone,
                                ui::withAlpha(tone, 0.15));
    }
    if (lasso_.size() >= 2) {
        const bool crossing = dragIsCrossing(lasso_.front().x, lasso_.back().x);
        const Rgba tone = crossing ? theme_.crossing : theme_.select;
        render::OverlayStyle st;
        st.color = tone;
        st.widthPx = 1.0;
        st.dashPx = crossing ? 6 : 0;
        st.gapPx = crossing ? 4 : 0;
        overlay.polyline(render::Space::Screen, lasso_, false, st);
    }
    if (cursorInside_ && !drag_ && lasso_.empty()) {
        const Vec2 c = lastPointer_.screen;
        // view.crosshair: "full" or "off" ("short", from older settings, is 10 %); the length is
        // view.crosshairSize % of the drawing area (CURSORSIZE), across it at 100.
        const std::string mode = settings_.getString("view.crosshair", "full");
        const double size =
            mode == "short"
                ? 10.0
                : std::clamp(settings_.getNumber("view.crosshairSize", 100), 1.0, 100.0);
        const double full = std::max(camera_.viewportW, camera_.viewportH);
        const double arm = size >= 100 ? full : std::max(8.0, full * size / 200.0);
        render::OverlayStyle cross;
        cross.color = inPaperSpace() ? Rgba{110, 110, 110, 200} : ui::withAlpha(theme_.text, 0.6);
        if (auto custom = ui::parseHexColor(settings_.getString("view.crosshairColor", "")))
            cross.color = *custom;
        cross.widthPx = 1.0;
        if (mode != "off") {
            overlay.line(render::Space::Screen, {c.x - arm, c.y}, {c.x + arm, c.y}, cross);
            overlay.line(render::Space::Screen, {c.x, c.y - arm}, {c.x, c.y + arm}, cross);
        }
        // Pick box, only when no command is waiting for a point.
        const bool pointPrompt = prompt_ && (prompt_->kind == cmd::PromptInfo::Kind::Point ||
                                             prompt_->kind == cmd::PromptInfo::Kind::Distance ||
                                             prompt_->kind == cmd::PromptInfo::Kind::Angle);
        if (!pointPrompt) {
            double h = settings_.getNumber("pick.aperturePx", 6);
            Vec2 box[4] = {
                {c.x - h, c.y - h}, {c.x + h, c.y - h}, {c.x + h, c.y + h}, {c.x - h, c.y + h}};
            render::OverlayStyle pb;
            pb.color = inPaperSpace() ? Rgba{90, 90, 90, 230} : ui::withAlpha(theme_.text, 0.85);
            overlay.polyline(render::Space::Screen, box, true, pb);
        } else if (settings_.getBool("osnap.apbox", false)) {
            // APBOX: the object snap aperture around the crosshair at point prompts.
            const auto ap = setting("drafting.osnap.aperture");
            const double h = ap ? std::clamp(std::strtod(ap->c_str(), nullptr), 1.0, 50.0) : 10.0;
            Vec2 box[4] = {
                {c.x - h, c.y - h}, {c.x + h, c.y - h}, {c.x + h, c.y + h}, {c.x - h, c.y + h}};
            render::OverlayStyle ab;
            ab.color = ui::withAlpha(cross.color, 0.8);
            overlay.polyline(render::Space::Screen, box, true, ab);
        }
    }
    // A selected or hovered hatch is a fill without strokes, so outline its boundary: otherwise
    // "1 selected" shows nothing on screen.
    {
        render::OverlayStyle outline;
        outline.widthPx = 2.0;
        auto outlineHatch = [&](EntityId id, Rgba color) {
            const Entity* e = session_->doc.find(id);
            const auto* h = e ? entityAs<HatchData>(*e) : nullptr;
            if (!h)
                return;
            outline.color = color;
            for (const HatchLoop& loop : h->loops) {
                std::vector<Vec2> pts;
                for (const geom::CurveRef& c : hatch::loopCurves(loop))
                    geom::flatten(c, camera_.pixelSizeMm(), pts);
                if (pts.size() >= 2)
                    overlay.polyline(render::Space::World, pts, true, outline);
            }
        };
        // The selected hatches' outlines only change with the selection, the drawing or a zoom
        // step, so they are kept between frames: select-all on a drawing with thousands of
        // hatches would otherwise flatten every boundary on every pan frame.
        std::uint64_t key = session_->doc.revision() * 1000003ull + selection_.size();
        for (EntityId id : selection_)
            key = (key ^ id.value) * 1099511628211ull;
        key = key * 1000003ull +
              static_cast<std::uint64_t>(
                  static_cast<std::int64_t>(std::floor(std::log2(camera_.pixelSizeMm()) * 2.0)) +
                  4096);
        if (key != hatchOutlines_.key) {
            hatchOutlines_.key = key;
            hatchOutlines_.loops.clear();
            for (EntityId id : selection_) {
                const Entity* e = session_->doc.find(id);
                const auto* h = e ? entityAs<HatchData>(*e) : nullptr;
                if (!h)
                    continue;
                for (const HatchLoop& loop : h->loops) {
                    std::vector<Vec2> pts;
                    for (const geom::CurveRef& c : hatch::loopCurves(loop))
                        geom::flatten(c, camera_.pixelSizeMm(), pts);
                    if (pts.size() >= 2)
                        hatchOutlines_.loops.push_back(std::move(pts));
                }
            }
        }
        outline.color = theme_.select;
        for (const auto& pts : hatchOutlines_.loops)
            overlay.polyline(render::Space::World, pts, true, outline);
        if (hoverId_ && !std::count(selection_.begin(), selection_.end(), hoverId_))
            outlineHatch(hoverId_, ui::withAlpha(theme_.select, 0.5));
    }
    // SELECTIONANNODISPLAY: a selected annotative object also shows how it looks at its other
    // scales, as faint dashed outlines (a few objects only; the call is cheap when the variable
    // is off or no selected object has several scales).
    if (!inPaperSpace() && !selection_.empty() && selection_.size() <= 200) {
        geom::DisplayContext dc;
        dc.pixelSizeMm = camera_.pixelSizeMm();
        render::OverlayStyle ghost;
        ghost.color = ui::withAlpha(theme_.select, 0.55);
        ghost.widthPx = 1.0;
        ghost.dashPx = 5;
        ghost.gapPx = 3;
        for (const geom::AnnotationGhost& g :
             geom::annotationGhosts(session_->doc, selection_, dc)) {
            for (const auto& item : g.display.items) {
                if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                    if (pl->pts.size() >= 2)
                        overlay.polyline(render::Space::World, pl->pts, pl->closed, ghost);
                } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
                    for (const auto& ring : fill->rings)
                        if (ring.size() >= 2)
                            overlay.polyline(render::Space::World, ring, true, ghost);
                }
            }
        }
    }
    if (inPaperSpace()) {
        // The sheet is a flat picture, so what is selected or under the pointer is outlined here.
        geom::DisplayContext dc;
        dc.pixelSizeMm = camera_.pixelSizeMm();
        auto outline = [&](EntityId id, Rgba color) {
            const Entity* e = session_->doc.find(id);
            if (!e)
                return;
            DisplayList dl;
            geom::buildDisplay(session_->doc, *e, dc, dl);
            render::OverlayStyle st;
            st.color = color;
            st.widthPx = 2.0;
            for (const auto& item : dl.items) {
                if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                    if (pl->pts.size() >= 2)
                        overlay.polyline(render::Space::World, pl->pts, pl->closed, st);
                } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
                    for (const auto& ring : fill->rings)
                        if (ring.size() >= 2)
                            overlay.polyline(render::Space::World, ring, true, st);
                }
            }
        };
        for (EntityId id : selection_)
            outline(id, theme_.select);
        for (EntityId id : runner().collected())
            outline(id, theme_.select);
        if (hoverId_ && !std::count(selection_.begin(), selection_.end(), hoverId_))
            outline(hoverId_, ui::withAlpha(theme_.select, 0.5));
    }
    if (ui) {
        PanelContext pc = makePanelContext();
        for (const auto& fn : ui->overlayDrawers())
            fn(pc, overlay);
    }
}

void AppCore::buildUnderlay(render::Overlay& underlay, const UiRegistry* ui) {
    underlay.clear();
    if (inPaperSpace())
        return;
    if (!ui)
        return;
    PanelContext pc = makePanelContext();
    for (const auto& fn : ui->underlayDrawers())
        fn(pc, underlay);
}

// ---- notices
// -------------------------------------------------------------------------------------

void AppCore::outOfAreaZoomTo() {
    if (outOfArea_)
        zoomToEntities(outOfArea_->ids);
}

void AppCore::outOfAreaUndo() {
    outOfArea_.reset();
    undo();
}

// ---- settings / IHost ----------------------------------------------------------------------

void AppCore::applyUiKeybindings(const UiRegistry& ui) {
    filterChain_.clear();
    for (const auto& f : ui.pointFilters())
        filterChain_.add(f.filter.get(), f.order);
    lineParser_ = ui.lineParser();
    ui_ = &ui;
    if (session_)
        session_->runner->setLineParser(lineParser_);
    notifySessionHooks();
    for (const auto& item : ui.menuItems())
        if (!item.actionId.empty())
            keybindings_.declare(item.actionId, item.defaultChord);
    if (auto r = keybindings_.load(paths_.keybindingsFile()); r) {
        for (const auto& w : r.value())
            print(w);
    }
}

void AppCore::notifySessionHooks() {
    if (!ui_ || !session_)
        return;
    SessionInfo info{&session_->doc, &spatialIndex(), this};
    for (const auto& hook : ui_->sessionHooks())
        hook(info);
}

void AppCore::registerBuiltinUi(UiRegistry& ui) {
    auto toggle = [&](const char* id, const char* label, const char* key, bool fallback = false) {
        std::string k = key;
        ui.addStatusToggle(
            id, label,
            [this, k, fallback] {  // also right when the value was set as text (SETVAR, config)
                const auto v = setting(k);
                return v ? isTrue(*v) : fallback;
            },
            [this, k](bool on) {
                settings_.setBool(k, on);
                // Ortho and polar tracking exclude each other.
                if (on && k == "drafting.ortho")
                    settings_.setBool("drafting.polar", false);
                if (on && k == "drafting.polar")
                    settings_.setBool("drafting.ortho", false);
            });
    };
    toggle("snap", "SNAP", "drafting.snap");
    toggle("grid", "GRID", "view.grid");
    toggle("ortho", "ORTHO", "drafting.ortho");
    toggle("polar", "POLAR", "drafting.polar");
    toggle("osnap", "OSNAP", "drafting.osnap");
    toggle("otrack", "OTRACK", "drafting.otrack");
    toggle("dyn", "DYN", "drafting.dynInput", true);
    toggle("lwt", "LWT", "view.lineweights", true);
    // Selection cycling: clicking the same spot again picks the next object under it (Alt-click
    // does this whenever the switch is off).
    toggle("selcyc", "SELCYC", "ui.selectionCycling");
    // Transparency is baked into the scene's colours, so the switch rebuilds the scene.
    ui.addStatusToggle(
        "tpy", "TPY",
        [this] {
            const auto v = setting("view.transparency");
            return v ? isTrue(*v) : true;
        },
        [this](bool on) {
            settings_.setBool("view.transparency", on);
            if (!session_)
                return;
            session_->sync->setShowTransparency(on);
            session_->sync->rebuildAll(scene_.tileSize());
            scene_.setSelected(selection_);
            ++generation_;
            lastTessellatedFor_ = camera_;
        });
    // Active-only mode lives in the drawing (Header::activeOnly); the command that owns it is
    // registered by the layers module, so this is a no-op in builds without it.
    ui.addStatusToggle(
        "activeonly", "ACTIVE", [this] { return document().header().activeOnly; },
        [this](bool on) {
            if (!prompt_)
                submitText(on ? "ACTIVEONLY On" : "ACTIVEONLY Off");
        });
    ui.addMenuItem("Tools/Settings...", "SETTINGS", "tools.settings", "");
}

bool AppCore::queueScript(std::vector<std::string> lines, int repeat) {
    if (!session_ || !session_->runner)
        return false;
    session_->runner->queueScript(std::move(lines), repeat);
    return true;
}

bool AppCore::resumeScript() {
    return session_ && session_->runner && session_->runner->resumeScript();
}

bool AppCore::repeatCommand(const std::string& name) {
    if (!session_ || !session_->runner)
        return false;
    session_->runner->setMultiple(name);
    return true;
}

double AppCore::ucsAngle() const {
    return session_ && session_->runner ? session_->runner->ucsAngle() : 0;
}

std::optional<std::string> AppCore::setting(std::string_view key) const {
    if (key == "document.file")
        return documentFile();
    if (key == "open.report")  // the reader notices of the active drawing, for DWGREPORT
        return session_ ? std::optional<std::string>(session_->openReport) : std::nullopt;
    if (key == "history.text") {
        std::string text;
        for (const auto& l : history_)
            text += l + '\n';
        return text;
    }
    if (key == "log.file")
        return std::string(logFile_.is_open() ? "1" : "0");
    if (key == "log.path")  // the file being written, else the folder a new one would go to
        return platform::pathToUtf8(logFile_.is_open() ? logPath_ : logFolder());
    if (key == "open.report.format")
        return session_ ? std::optional<std::string>(session_->openReportFormat) : std::nullopt;
    // The layout commands work on the sheet on screen, or on the one whose viewport is being
    // edited from model space.
    if (key == "layout.current") {
        if (inPaperSpace())
            return activeLayout_;
        if (!editingLayout_.empty())
            return editingLayout_;
    }
    if (key == "tmp.vports.layout")  // the tiled viewport layout of model space (VPORTS dialog)
        return tileLayout_;
    if (key == "tmp.layout.editing")  // "Layout:id" while a viewport is edited from model space
        return editingLayout_.empty()
                   ? std::nullopt
                   : std::optional<std::string>(editingLayout_ + ":" +
                                                std::to_string(editingViewport_));
    if (key.rfind("tmp.", 0) == 0) {  // hand-over between a command and its dialog; never saved
        auto it = transient_.find(std::string(key));
        return it == transient_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    switch (settings_.kindOf(key)) {
        case platform::Settings::Kind::Bool:
            return std::string(settings_.getBool(key) ? "1" : "0");
        case platform::Settings::Kind::Number:
            return formatDouble(settings_.getNumber(key));
        case platform::Settings::Kind::String:
            return settings_.getString(key);
        default:
            return std::nullopt;
    }
}

std::optional<std::string> AppCore::takeClipboardText() {
    std::optional<std::string> out;
    out.swap(clipboardOut_);
    return out;
}

void AppCore::setSetting(std::string_view key, std::string value) {
    if (key.rfind("tmp.", 0) == 0) {
        transient_[std::string(key)] = std::move(value);
        return;
    }
    if (key == "ui.showPanel") {  // a command asks the window to show a panel
        openPanel(value);
        return;
    }
    if (key == "clipboard.text")
        clipboardOut_ = value;
    if (key == "log.file") {  // LOGFILEON / LOGFILEOFF
        if (isTrue(value))
            startLogFile();
        else
            stopLogFile();
        return;
    }
    if (key == "log.folder") {  // LOGFILEPATH <folder>
        logDir_ = platform::pathFromUtf8(value);
        return;
    }
    if (key == "layout.show") {  // LAYOUT New: switch once the command is over
        pendingLayout_ = std::move(value);
        return;
    }
    switch (settings_.kindOf(key)) {
        case platform::Settings::Kind::Bool:
            settings_.setBool(key, isTrue(value));
            break;
        case platform::Settings::Kind::Number:
            settings_.setNumber(key, std::strtod(value.c_str(), nullptr));
            break;
        default:
            settings_.setString(key, std::move(value));
    }
}

void AppCore::setPreview(const DisplayList* list) {
    if (list) {
        preview_ = *list;
        hasPreview_ = true;
    } else {
        preview_.items.clear();
        hasPreview_ = false;
    }
}

void AppCore::promptChanged(const cmd::PromptInfo* prompt) {
    if (prompt)
        prompt_ = *prompt;
    else
        prompt_.reset();
}

void AppCore::feedbackChanged(const cmd::FilterFeedback& feedback) {
    feedback_ = feedback;
}

}  // namespace tbcad::ui
