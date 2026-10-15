#include "tbcad/ui/app_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <sstream>
#include <unordered_set>

#include "app_commands.h"
#include "tbcad/blocks/index_refresh.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/geom/entity.h"
#include "tbcad/hatch/fill.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/platform/json.h"
#include "tbcad/platform/paths.h"
#include "tbcad/tmpl/tmpl.h"
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
    cmd::UndoStack undo{};
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
    // File stamp of each xref that a reload was already tried for (a failed reload is not retried
    // until the file changes again).
    std::map<BlockId, std::int64_t> xrefTried{};
    std::vector<AppCore::MissingXref> missingXrefs{};
    std::size_t xrefNoticeDismissedFor = 0;  // the notice was closed at this many missing xrefs
};

struct AppCore::DocSlot {
    std::unique_ptr<Session> session;
    std::uint64_t id = 0;
    render::Camera2D camera{};  // the view while the drawing is in the background
    std::vector<EntityId> selection{};
    bool viewSaved = false;
    std::uint64_t autosavedRevision = ~std::uint64_t{0};  // revision last written to the autosave
};

AppCore::AppCore(cmd::Registry& registry, platform::AppPaths paths)
    : registry_(registry), paths_(std::move(paths)), settings_(platform::Settings::withDefaults()) {
    if (auto r = settings_.load(paths_.settingsFile()); !r)
        startupWarnings_.push_back(r.error().message + " (using default settings)");

    keybindings_.declare("file.new", "Ctrl+N");
    keybindings_.declare("file.open", "Ctrl+O");
    keybindings_.declare("file.save", "Ctrl+S");
    keybindings_.declare("file.saveAs", "Ctrl+Shift+S");
    keybindings_.declare("file.close", "Ctrl+W");
    keybindings_.declare("view.nextDocument", "Ctrl+Tab");
    keybindings_.declare("edit.undo", "Ctrl+Z");
    keybindings_.declare("edit.redo", "Ctrl+Y");
    keybindings_.declare("edit.selectAll", "Ctrl+A");
    keybindings_.declare("edit.deselect", "Escape");
    keybindings_.declare("edit.delete", "Delete");
    keybindings_.declare("view.zoomExtents", "");
    keybindings_.declare("view.toggleCommandLine", "");
    keybindings_.declare("view.commandPalette", "Ctrl+Shift+P");
    keybindings_.declare("view.cursorHud", "F12");
    keybindings_.declare("view.expandLog", "Ctrl+L");
    keybindings_.declare("view.cleanScreen", "F11");
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

std::unique_ptr<AppCore::Session> AppCore::makeSession(std::unique_ptr<Document> doc,
                                                       fs::path file) {
    auto s = std::make_unique<Session>(std::move(doc));
    Document& d = s->doc;
    s->indexer = std::make_unique<index::DocumentIndexer>(
        d, s->index, [&d](const Entity& e) { return geom::bounds(e, d); });
    s->blockRefresher = std::make_unique<blocks::BlockIndexRefresher>(d, *s->indexer);
    s->sync = std::make_unique<SceneSync>(d, scene_);
    s->observer = std::make_unique<AppObserver>(d, *this);
    s->runner = std::make_unique<cmd::CommandRunner>(d, s->undo, *this, registry_);
    s->runner->setFilterChain(&filterChain_);
    s->runner->setLineParser(lineParser_);
    s->undo.setMessageSink([this](std::string m) { print(m); });
    s->file = std::move(file);
    d.setFilePath(platform::pathToUtf8(s->file));
    s->savedRevision = d.revision();
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
    if (activeLayout_.empty())
        return;
    const int w = camera_.viewportW, h = camera_.viewportH;
    camera_ = modelCamera_;
    camera_.viewportW = w;
    camera_.viewportH = h;
    activeLayout_.clear();
    paper_.clear();
    paperIndex_.bulkLoad(std::span<const index::IdBox>{});
    selection_.clear();
    lastTessellatedFor_ = {};
    notifySessionHooks();
    settings_.setString("plot.layout", "");  // PLOT follows what is on screen
}

bool AppCore::setActiveLayout(const std::string& name) {
    if (!session_)
        return false;
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
    camera_.fit(paper_.sheet());
    return true;
}

void AppCore::clearTransientState() {
    selection_.clear();
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
    clearTransientState();
    active_ = index;
    DocSlot& slot = slots_[index];
    session_ = slot.session.get();
    if (slot.viewSaved && !fitView) {
        const int w = camera_.viewportW, h = camera_.viewportH;
        camera_ = slot.camera;
        camera_.viewportW = w;
        camera_.viewportH = h;
        selection_ = slot.selection;
        selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                        [&](EntityId id) { return !session_->doc.find(id); }),
                         selection_.end());
    }
    session_->sync->setPixelSize(camera_.pixelSizeMm());
    session_->sync->rebuildAll(render::Scene::suggestTileSize(drawingExtents()));
    scene_.setSelected(selection_);
    ++generation_;
    lastTessellatedFor_ = camera_;
    notifySessionHooks();
    scanMissingXrefs();
    if (fitView && !session_->index.empty()) {
        zoomExtents();
        fitPending_ = 6;
    }
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

void AppCore::removeAutosaveFiles() {
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

bool AppCore::autosaveNow() {
    bool wrote = false;
    for (DocSlot& slot : slots_) {
        Session& ses = *slot.session;
        if (ses.doc.revision() == ses.savedRevision || slot.autosavedRevision == ses.doc.revision())
            continue;
        if (!platform::ensureDirectory(paths_.autosaveDir()))
            return wrote;
        const std::string base = "recovery-" + std::to_string(slot.id);
        io::WriteOptions opt;
        opt.keepBackup = false;
        if (!io::writeTbcad(ses.doc, paths_.autosaveDir() / (base + ".tbcad"), opt))
            continue;
        {
            platform::Json::Object o;
            o["original"] = platform::pathToUtf8(ses.file);
            std::ofstream out(paths_.autosaveDir() / (base + ".json"),
                              std::ios::binary | std::ios::trunc);
            out << platform::toJson(platform::Json(std::move(o)));
        }
        slot.autosavedRevision = ses.doc.revision();
        wrote = true;
    }
    return wrote;
}

void AppCore::tickAutosave(std::chrono::steady_clock::time_point now) {
    if (!autosaveClockStarted_) {
        lastAutosave_ = now;
        autosaveClockStarted_ = true;
        return;
    }
    if (!settings_.getBool("autosave.enabled", true))
        return;
    double minutes = std::max(0.1, settings_.getNumber("autosave.minutes", 10));
    if (now - lastAutosave_ < std::chrono::duration<double, std::ratio<60>>(minutes))
        return;
    lastAutosave_ = now;
    autosaveNow();
}

// ---- files -------------------------------------------------------------------------------------

std::unique_ptr<Document> AppCore::defaultDocument(std::string* problem) {
    const std::string name = settings_.getString("template.default", "");
    if (!name.empty() && name != tmpl::kBuiltinTemplate) {
        const fs::path file =
            templateFolder() /
            platform::pathFromUtf8(tmpl::sanitizeTemplateName(name) + tmpl::kTemplateExtension);
        std::error_code ec;
        std::string why;
        if (!fs::exists(file, ec)) {
            why = "No template named \"" + name + "\".";
        } else if (auto doc = tmpl::loadTemplate(file)) {
            return std::move(doc.value());
        } else {
            why = "Cannot read the template: " + doc.error().message;
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

std::vector<std::string> AppCore::templateNames() const {
    std::vector<std::string> out{tmpl::kBuiltinTemplate};
    for (const auto& t : tmpl::listTemplates(templateFolder()))
        out.push_back(t.name);
    return out;
}

std::string AppCore::newFromTemplate(std::string_view name) {
    if (name.empty() || name == tmpl::kBuiltinTemplate) {
        addSession(tmpl::builtinTemplate(), {});
        return {};
    }
    const fs::path file =
        templateFolder() / platform::pathFromUtf8(tmpl::sanitizeTemplateName(std::string(name)) +
                                                  tmpl::kTemplateExtension);
    std::error_code ec;
    if (!fs::exists(file, ec))
        return "No template named \"" + std::string(name) + "\".";
    auto doc = tmpl::loadTemplate(file);
    if (!doc)
        return "Cannot read the template: " + doc.error().message;
    addSession(std::move(doc.value()), {});
    return {};
}

std::string AppCore::saveAsTemplate(std::string_view name) {
    const std::string clean = tmpl::sanitizeTemplateName(std::string(name));
    if (clean == tmpl::kBuiltinTemplate)
        return "\"Built-in\" is reserved; choose another name.";
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

Result<void> AppCore::openDxf(const fs::path& path) {
    std::ifstream in(platform::longPath(path), std::ios::binary);
    if (!in) {
        printOpenFailure("Cannot open " + path.string());
        settings_.removeRecentFile(platform::pathToUtf8(path));
        return makeError(ErrorCode::Io, "Cannot read " + path.string());
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    dxf::Report report;
    auto result = dxf::importDxf(bytes, {}, report);
    if (!result) {
        printOpenFailure("Cannot open " + path.string() + ": " + result.error().message);
        return makeError(ErrorCode::Parse, result.error().message);
    }
    auto& imported = result.value();
    std::size_t total = 0;
    for (const auto& [name, count] : imported.summary.entities)
        total += count.imported;
    addSession(std::move(imported.document), path);
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

Result<void> AppCore::saveDxf(const fs::path& path) {
    dxf::Report report;
    dxf::ExportOptions options;
    auto out = dxf::exportDxf(session_->doc, options, report);
    if (!out) {
        print("Cannot save " + path.string() + ": " + out.error().message);
        return makeError(ErrorCode::InvalidArgument, out.error().message);
    }
    std::error_code ec;
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f)
            f.write(out.value().bytes.data(),
                    static_cast<std::streamsize>(out.value().bytes.size()));
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            print("Cannot save " + path.string());
            return makeError(ErrorCode::Io, "Cannot write " + path.string());
        }
    }
    if (fs::exists(path, ec)) {
        fs::path bak = path;
        bak += ".bak";
        fs::remove(bak, ec);
        fs::rename(path, bak, ec);
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        print("Cannot save " + path.string() + ": " + ec.message());
        return makeError(ErrorCode::Io, ec.message());
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
    for (const auto& m : r.messages)
        print(m);
    settings_.addRecentFile(platform::pathToUtf8(path));
    (void)settings_.save(paths_.settingsFile());
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
    if (auto it = foreignReaders_.find(lowerExtension(path)); it != foreignReaders_.end())
        return finishForeignOpen(path, it->first, it->second.readOnly, it->second.read(path));
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
    if (!doc) {
        printOpenFailure("Cannot open " + path.string() + ": " + doc.error().message);
        settings_.removeRecentFile(platform::pathToUtf8(path));
        return doc.error();
    }
    addSession(std::move(doc.value()), path);
    for (const auto& w : report.warnings)
        print("Warning: " + w);
    if (report.unknownEntityKinds > 0)
        print(std::to_string(report.unknownEntityKinds) +
              " object(s) from a newer version are kept read-only.");
    settings_.addRecentFile(platform::pathToUtf8(path));
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
    return saveAs(session_->file);
}

Result<void> AppCore::saveAs(const fs::path& requested) {
    fs::path path = requested;
    if (!path.has_extension())
        path += settings_.getString("files.defaultFormat", "tbcad") == "dxf" ? ".dxf" : ".tbcad";
    if (runner().active()) {
        print("Finish the current command first.");
        return makeError(ErrorCode::Conflict, "A command is running.");
    }
    const std::string ext = lowerExtension(path);
    if (ext != ".tbcad" && ext != ".dxf") {
        return makeError(ErrorCode::Unsupported,
                         "TBCAD can save as .tbcad or .dxf; it never writes " + ext + " files.");
    }
    auto r = isDxfPath(path) ? saveDxf(path) : io::writeTbcad(session_->doc, path);
    if (!r) {
        print("Cannot save " + path.string() + ": " + r.error().message);
        return r;
    }
    session_->file = path;
    session_->doc.setFilePath(platform::pathToUtf8(path));
    session_->readOnlyFormat.clear();
    session_->savedRevision = session_->doc.revision();
    removeAutosaveOf(slots_[active_].id);
    if (!anyDocumentModified())
        removeAutosaveFiles();
    settings_.addRecentFile(platform::pathToUtf8(path));
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
    history_.emplace_back(message);
    if (history_.size() > kMaxHistory)
        history_.erase(history_.begin(),
                       history_.begin() + static_cast<std::ptrdiff_t>(kMaxHistory / 4));
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
    print(promptText() + trimmed);
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
    if (cmd::SubmitResult r = run.submit(cmd::Input::typed(words[0]));
        r == cmd::SubmitResult::UnknownCommand)
        return;
    // Remaining words answer the prompts of the command that just started.
    for (std::size_t i = 1; i < words.size() && run.active(); ++i)
        run.submit(cmd::Input::typed(words[i]));
}

void AppCore::pressEscape() {
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

void AppCore::setViewportSize(int w, int h) {
    const int nw = std::max(w, 1), nh = std::max(h, 1);
    const bool changed = nw != camera_.viewportW || nh != camera_.viewportH;
    camera_.viewportW = nw;
    camera_.viewportH = nh;
    // A drawing just opened was fitted to the size the window had then; while the user has not
    // touched the view, fit it again when the panels settle into their real size.
    if (changed && fitPending_ > 0 && session_) {
        --fitPending_;
        zoomExtents();
    }
}

void AppCore::setCamera(const render::Camera2D& cam) {
    camera_ = cam;
    fitPending_ = 0;
}

void AppCore::zoomAtScreen(Vec2 screen, double notches) {
    fitPending_ = 0;
    camera_.zoomAt(screen, std::pow(kZoomStep, notches));
}

void AppCore::panPixels(double dx, double dy) {
    fitPending_ = 0;
    camera_.panPixels(dx, dy);
}

// Rays and xlines have no end, so they do not count; if they are all there is, use them.
BBox2 AppCore::drawingExtents() const {
    BBox2 box;
    std::vector<BBox2> boxes;
    const Document& doc = session_->doc;
    doc.forEachEntity([&](const Entity& e) {
        if (!doc.ownerBlock(e.common.id) && geom::countsForExtents(e)) {
            const BBox2 b = session_->index.boxOf(e.common.id);
            box.merge(b);
            if (!b.empty())
                boxes.push_back(b);
        }
    });
    // A few stray objects far from everything else (common in imported files) would shrink the
    // real drawing to a speck: when the outermost 5% of the objects stretch the view more than
    // fivefold, frame the rest.
    if (boxes.size() >= 10) {
        std::vector<double> xs, ys;
        for (const BBox2& b : boxes) {
            xs.push_back(b.center().x);
            ys.push_back(b.center().y);
        }
        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());
        const std::size_t cut = std::max<std::size_t>(1, boxes.size() / 20);
        const double spanX = xs[xs.size() - 1 - cut] - xs[cut];
        const double spanY = ys[ys.size() - 1 - cut] - ys[cut];
        BBox2 core;
        for (const BBox2& b : boxes) {
            const Vec2 c = b.center();
            if (c.x >= xs[cut] - spanX && c.x <= xs[xs.size() - 1 - cut] + spanX &&
                c.y >= ys[cut] - spanY && c.y <= ys[ys.size() - 1 - cut] + spanY)
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
    zoomToBox(viewExtents());
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
    if (!session_ || session_->runner->active())
        return;
    bool stale = false;
    for (BlockId id : xref::xrefs(session_->doc)) {
        if (xref::state(session_->doc, id, session_->file) != xref::State::Changed)
            continue;
        const BlockDef* def = session_->doc.block(id);
        std::int64_t stamp = xref::fileStamp(xref::resolvePath(def->xrefPath, session_->file));
        auto tried = session_->xrefTried.find(id);
        if (tried != session_->xrefTried.end() && tried->second == stamp)
            continue;
        session_->xrefTried[id] = stamp;
        stale = true;
    }
    if (!stale || !registry_.find("XRELOAD"))
        return;
    print("External reference changed on disk; reloading");
    xrefReloadWait_ = 1;  // one frame with the notice on screen, then the blocking reload
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

void AppCore::applyPendingLayout() {
    if (pendingLayout_.empty() || runner().active())
        return;
    const std::string name = std::move(pendingLayout_);
    pendingLayout_.clear();
    setActiveLayout(name);
}

void AppCore::updateFrame(std::chrono::steady_clock::time_point now) {
    applyPendingLayout();
    tickAutosave(now);
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
    retileIfFragmented();
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
    if (render::Scene::needsRetessellation(lastTessellatedFor_, camera_)) {
        session_->sync->setPixelSize(camera_.pixelSizeMm());
        session_->sync->refreshStale(camera_);
        lastTessellatedFor_ = camera_;
    }
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
    std::string out = k < std::size(kNames) ? kNames[k] : "Object";
    if (const Layer* l = session_->doc.layer(e->common.layer))
        out += ", layer " + l->name;
    char buf[64];
    if (const auto* c = std::get_if<CircleData>(&e->data)) {
        std::snprintf(buf, sizeof buf, ", radius %.2f mm", c->radius);
        out += buf;
    } else if (const double len = geom::length(*e); len > 0) {
        std::snprintf(buf, sizeof buf, ", length %.2f mm", len);
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
        scene_.setSelected(selection_);
    } else {
        setSelection(std::move(sel));
    }
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
    pc.runCommand = [this](const std::string& line) { submitText(line); };
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
                break;
            case cmd::PromptInfo::Kind::Entity:
            case cmd::PromptInfo::Kind::Selection:
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
    applyPick({*id}, p.shift);
}

void AppCore::doubleClick(const PointerState& p) {
    const bool onSheetObject =
        inPaperSpace() && !runner().active() &&
        pickEntity(session_->doc, spatialIndex(), camera_.screenToWorld(p.screen), apertureMm());
    if (inPaperSpace() && !onSheetObject && !runner().active()) {
        // A double click inside a viewport goes to model space, showing what the viewport shows
        // there, so the drawing can be edited from the sheet.
        const Vec2 at = camera_.screenToWorld(p.screen);
        if (auto lay = layout::findLayout(session_->doc, activeLayout_)) {
            for (const layout::Viewport& vp : lay->viewports) {
                if (!vp.on || !vp.paper.contains(at))
                    continue;
                const double halfW = vp.paper.width() * vp.scaleN / 2;
                const double halfH = vp.paper.height() * vp.scaleN / 2;
                leavePaper();
                zoomToBox({{vp.center.x - halfW, vp.center.y - halfH},
                           {vp.center.x + halfW, vp.center.y + halfH}});
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
    if (kind(*e) == EntityKind::Text || kind(*e) == EntityKind::MText)
        submitText("TEXTEDIT");
    else
        openPanel("properties");  // like AutoCAD: double click an object to see its properties
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
        if (prompt_ && prompt_->kind == cmd::PromptInfo::Kind::Selection && !ids.empty())
            run.submit(cmd::Input::select(std::move(ids)));
        return;
    }
    if (p.shift)
        applyPick(ids, true);
    else
        applyPick(ids, false);
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
        if (prompt_ && prompt_->kind == cmd::PromptInfo::Kind::Selection && !ids.empty())
            run.submit(cmd::Input::select(std::move(ids)));
        return;
    }
    applyPick(ids, p.shift);
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
    scene_.setSelected(selection_);
}

void AppCore::selectAll() {
    std::vector<EntityId> all;
    for (EntityId id : session_->doc.modelSpace()) {
        const Entity* e = session_->doc.find(id);
        if (e && isSelectable(session_->doc, *e))
            all.push_back(id);
    }
    setSelection(std::move(all));
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
        // view.crosshair: "full" (across the viewport), "short" (5 % of its width) or "off".
        const std::string mode = settings_.getString("view.crosshair", "full");
        const double arm = mode == "short" ? std::max(8.0, camera_.viewportW * 0.05)
                                           : std::max(camera_.viewportW, camera_.viewportH);
        render::OverlayStyle cross;
        cross.color = inPaperSpace() ? Rgba{110, 110, 110, 200} : ui::withAlpha(theme_.text, 0.6);
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
        for (EntityId id : selection_)
            outlineHatch(id, theme_.select);
        if (hoverId_ && !std::count(selection_.begin(), selection_.end(), hoverId_))
            outlineHatch(hoverId_, ui::withAlpha(theme_.select, 0.5));
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
    auto toggle = [&](const char* id, const char* label, const char* key) {
        std::string k = key;
        ui.addStatusToggle(
            id, label, [this, k] { return settings_.getBool(k, false); },
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

std::optional<std::string> AppCore::setting(std::string_view key) const {
    if (key == "document.file")
        return documentFile();
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
    if (key == "clipboard.text")
        clipboardOut_ = value;
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
