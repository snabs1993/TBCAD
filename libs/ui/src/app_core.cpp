#include "tbcad/ui/app_core.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "app_commands.h"
#include "tbcad/blocks/index_refresh.h"
#include "tbcad/geom/entity.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/platform/json.h"
#include "tbcad/ui/selection.h"

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
};

AppCore::AppCore(cmd::Registry& registry, platform::AppPaths paths)
    : registry_(registry), paths_(std::move(paths)), settings_(platform::Settings::withDefaults()) {
    if (auto r = settings_.load(paths_.settingsFile()); !r)
        startupWarnings_.push_back(r.error().message + " (using default settings)");

    keybindings_.declare("file.new", "Ctrl+N");
    keybindings_.declare("file.open", "Ctrl+O");
    keybindings_.declare("file.save", "Ctrl+S");
    keybindings_.declare("file.saveAs", "Ctrl+Shift+S");
    keybindings_.declare("edit.undo", "Ctrl+Z");
    keybindings_.declare("edit.redo", "Ctrl+Y");
    keybindings_.declare("edit.selectAll", "Ctrl+A");
    keybindings_.declare("edit.deselect", "Escape");
    keybindings_.declare("view.zoomExtents", "");
    keybindings_.declare("view.toggleCommandLine", "");
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

    installSession(std::make_unique<Document>(), {});
    for (const auto& w : startupWarnings_)
        print(w);
}

AppCore::~AppCore() {
    // The runner may hold coroutine frames that refer to the host and the document.
    session_.reset();
}

void AppCore::installSession(std::unique_ptr<Document> doc, fs::path file) {
    session_.reset();
    selection_.clear();
    outOfArea_.reset();
    drag_.reset();
    hasPreview_ = false;
    prompt_.reset();
    feedback_ = {};
    scene_.setSelected({});
    scene_.setHighlighted({});

    auto s = std::make_unique<Session>(std::move(doc));
    Document& d = s->doc;
    s->indexer = std::make_unique<index::DocumentIndexer>(
        d, s->index, [&d](const Entity& e) { return geom::bounds(e, d); });
    s->blockRefresher = std::make_unique<blocks::BlockIndexRefresher>(d, *s->indexer);
    s->sync = std::make_unique<SceneSync>(d, scene_);
    s->sync->setPixelSize(camera_.pixelSizeMm());
    s->sync->rebuildAll();
    s->observer = std::make_unique<AppObserver>(d, *this);
    s->runner = std::make_unique<cmd::CommandRunner>(d, s->undo, *this, registry_);
    s->runner->setFilterChain(&filterChain_);
    s->runner->setLineParser(lineParser_);
    s->undo.setMessageSink([this](std::string m) { print(m); });
    s->file = std::move(file);
    s->savedRevision = d.revision();
    session_ = std::move(s);
    ++generation_;
    lastTessellatedFor_ = camera_;
    autosavedRevision_ = ~std::uint64_t{0};
    notifySessionHooks();
    if (!session_->index.empty())
        zoomExtents();
}

Document& AppCore::document() {
    return session_->doc;
}
const Document& AppCore::document() const {
    return session_->doc;
}
const index::SpatialIndex& AppCore::spatialIndex() const {
    return session_->index;
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

std::optional<Recovery> AppCore::pendingRecovery() const {
    fs::path file = paths_.autosaveDir() / "recovery.tbcad";
    std::error_code ec;
    if (!fs::exists(file, ec))
        return std::nullopt;
    Recovery r;
    r.autosaveFile = file;
    std::ifstream in(paths_.autosaveDir() / "recovery.json", std::ios::binary);
    if (in) {
        std::ostringstream buf;
        buf << in.rdbuf();
        if (auto j = platform::parseJson(buf.str()); j && j.value().isObject())
            if (const auto* o = j.value().find("original"))
                r.originalPath = o->asString();
    }
    return r;
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

void AppCore::discardRecovery() {
    removeAutosaveFiles();
}

void AppCore::removeAutosaveFiles() {
    std::error_code ec;
    fs::remove(paths_.autosaveDir() / "recovery.tbcad", ec);
    fs::remove(paths_.autosaveDir() / "recovery.json", ec);
}

void AppCore::shutdown() {
    if (settings_.dirty())
        (void)settings_.save(paths_.settingsFile());
    removeAutosaveFiles();
}

bool AppCore::autosaveNow() {
    if (!modified() || autosavedRevision_ == session_->doc.revision())
        return false;
    if (!platform::ensureDirectory(paths_.autosaveDir()))
        return false;
    io::WriteOptions opt;
    opt.keepBackup = false;
    if (!io::writeTbcad(session_->doc, paths_.autosaveDir() / "recovery.tbcad", opt))
        return false;
    {
        platform::Json::Object o;
        o["original"] = platform::pathToUtf8(session_->file);
        std::ofstream out(paths_.autosaveDir() / "recovery.json",
                          std::ios::binary | std::ios::trunc);
        out << platform::toJson(platform::Json(std::move(o)));
    }
    autosavedRevision_ = session_->doc.revision();
    return true;
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

void AppCore::newDocument() {
    removeAutosaveFiles();
    installSession(std::make_unique<Document>(), {});
}

Result<void> AppCore::openFile(const fs::path& path) {
    io::IoReport report;
    auto doc = io::readTbcad(path, report);
    if (!doc) {
        print("Cannot open " + path.string() + ": " + doc.error().message);
        settings_.removeRecentFile(platform::pathToUtf8(path));
        return doc.error();
    }
    removeAutosaveFiles();
    installSession(std::move(doc.value()), path);
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
    if (session_->file.empty())
        return makeError(ErrorCode::InvalidArgument, "This drawing has no file name yet.");
    return saveAs(session_->file);
}

Result<void> AppCore::saveAs(const fs::path& requested) {
    fs::path path = requested;
    if (!path.has_extension())
        path += ".tbcad";
    if (runner().active()) {
        print("Finish the current command first.");
        return makeError(ErrorCode::Conflict, "A command is running.");
    }
    auto r = io::writeTbcad(session_->doc, path);
    if (!r) {
        print("Cannot save " + path.string() + ": " + r.error().message);
        return r;
    }
    session_->file = path;
    session_->savedRevision = session_->doc.revision();
    removeAutosaveFiles();
    settings_.addRecentFile(platform::pathToUtf8(path));
    (void)settings_.save(paths_.settingsFile());
    print("Saved " + path.string());
    return {};
}

std::string AppCore::displayName() const {
    return session_->file.empty() ? std::string("Untitled") : session_->file.filename().string();
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

void AppCore::submitText(std::string_view text) {
    std::string trimmed = cmd::trim(text);
    cmd::CommandRunner& run = runner();
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
    camera_.viewportW = std::max(w, 1);
    camera_.viewportH = std::max(h, 1);
}

void AppCore::setCamera(const render::Camera2D& cam) {
    camera_ = cam;
}

void AppCore::zoomAtScreen(Vec2 screen, double notches) {
    camera_.zoomAt(screen, std::pow(kZoomStep, notches));
}

void AppCore::panPixels(double dx, double dy) {
    camera_.panPixels(dx, dy);
}

void AppCore::zoomExtents() {
    zoomToBox(session_->index.bounds());
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

void AppCore::updateFrame(std::chrono::steady_clock::time_point now) {
    tickAutosave(now);
    if (render::Scene::needsRetessellation(lastTessellatedFor_, camera_)) {
        session_->sync->setPixelSize(camera_.pixelSizeMm());
        session_->sync->refreshStale(camera_);
        lastTessellatedFor_ = camera_;
    }
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
        if (auto id = pickEntity(session_->doc, session_->index, w, apertureMm()))
            hover.push_back(*id);
    }
    scene_.setHighlighted(hover);
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
}

void AppCore::applyPick(const std::vector<EntityId>& ids, bool remove) {
    std::vector<EntityId> sel;
    if (remove) {
        std::unordered_set<EntityId> drop(ids.begin(), ids.end());
        sel.reserve(selection_.size());
        for (EntityId id : selection_)
            if (!drop.count(id))
                sel.push_back(id);
    } else {
        std::unordered_set<EntityId> have(selection_.begin(), selection_.end());
        sel = selection_;
        for (EntityId id : ids)
            if (have.insert(id).second)
                sel.push_back(id);
    }
    setSelection(std::move(sel));
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
    pc.runCommand = [this](const std::string& line) { submitText(line); };
    return pc;
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
                if (auto id = pickEntity(session_->doc, session_->index, world, apertureMm()))
                    run.submit(cmd::Input::pick(*id, world));
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
    auto id = pickEntity(session_->doc, session_->index, world, apertureMm());
    if (!id) {
        if (!p.shift)
            clearSelection();
        return;
    }
    applyPick({*id}, p.shift);
}

void AppCore::doubleClick(const PointerState& p) {
    if (runner().active()) {
        click(p);
        return;
    }
    Vec2 world = makeRaw(p).worldCursor;
    auto id = pickEntity(session_->doc, session_->index, world, apertureMm());
    const Entity* e = id ? session_->doc.find(*id) : nullptr;
    if (!e || (kind(*e) != EntityKind::Text && kind(*e) != EntityKind::MText)) {
        click(p);
        return;
    }
    applyPick({*id}, false);
    submitText("TEXTEDIT");
}

void AppCore::dragSelect(Vec2 a, Vec2 b, const PointerState& p) {
    const bool crossing = dragIsCrossing(a.x, b.x);
    Vec2 wa = camera_.screenToWorld(a);
    Vec2 wb = camera_.screenToWorld(b);
    BBox2 box;
    box.expand(wa);
    box.expand(wb);
    std::vector<EntityId> ids = selectInBox(session_->doc, session_->index, box, crossing);
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
        st.color = {255, 255, 255, 255};
        st.widthPx = 1.0;
        st.dashPx = 6;
        st.gapPx = 4;
        for (const auto& item : preview_.items) {
            if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                if (pl->pts.size() >= 2)
                    overlay.polyline(render::Space::World, pl->pts, pl->closed, st);
            } else if (const auto* pts = std::get_if<DisplayPoints>(&item)) {
                for (const Vec2& pt : pts->pts)
                    overlay.marker(render::Space::World, pt, render::MarkerKind::Cross, 6,
                                   {255, 255, 255, 255});
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
            overlay.marker(render::Space::World, feedback_.markerPos, mk, 10, {255, 230, 0, 255});
        }
    }
    if (drag_) {
        overlay.selectionWindow(drag_->first, drag_->second,
                                dragIsCrossing(drag_->first.x, drag_->second.x));
    }
    if (cursorInside_ && !drag_) {
        const Vec2 c = lastPointer_.screen;
        const double arm = std::max(camera_.viewportW, camera_.viewportH);
        render::OverlayStyle cross;
        cross.color = {200, 200, 200, 160};
        cross.widthPx = 1.0;
        overlay.line(render::Space::Screen, {c.x - arm, c.y}, {c.x + arm, c.y}, cross);
        overlay.line(render::Space::Screen, {c.x, c.y - arm}, {c.x, c.y + arm}, cross);
        // Pick box, only when no command is waiting for a point.
        const bool pointPrompt = prompt_ && (prompt_->kind == cmd::PromptInfo::Kind::Point ||
                                             prompt_->kind == cmd::PromptInfo::Kind::Distance ||
                                             prompt_->kind == cmd::PromptInfo::Kind::Angle);
        if (!pointPrompt) {
            double h = settings_.getNumber("pick.aperturePx", 6);
            Vec2 box[4] = {
                {c.x - h, c.y - h}, {c.x + h, c.y - h}, {c.x + h, c.y + h}, {c.x - h, c.y + h}};
            render::OverlayStyle pb;
            pb.color = {230, 230, 230, 220};
            overlay.polyline(render::Space::Screen, box, true, pb);
        }
    }
    if (ui) {
        PanelContext pc = makePanelContext();
        for (const auto& fn : ui->overlayDrawers())
            fn(pc, overlay);
    }
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
    SessionInfo info{&session_->doc, &session_->index, this};
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

void AppCore::setSetting(std::string_view key, std::string value) {
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
