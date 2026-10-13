#include "tbcad/plugin/plugin_host.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "dynlib.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/geom/geom.h"
#include "tbcad/plugin/tbcad_plugin.h"

namespace tbcad::plugin {

namespace {

// ---- the hand-over between the command (UI thread) and the plugin's function (own thread) ----
//
// Only one of the two threads runs at any time: the plugin thread runs while the UI thread waits
// in Call::waitForPlugin, and the other way round while a prompt is open. So neither the document
// nor the CommandContext is ever touched by two threads at once.

enum class Ask { Point, Real, Integer, String, Keyword, Selection };

struct Request {
    Ask kind = Ask::Point;
    std::string prompt{};
    std::vector<std::string> keywords{};
    bool hasBase = false;
    Vec2 base{};
};

struct Answer {
    cmd::Status status = cmd::Status::Cancel;
    Vec2 point{};
    double real = 0;
    long long integer = 0;
    std::string text{};  // string or keyword
    std::vector<EntityId> ids{};
};

struct PluginCall {
    enum class Phase { PluginRuns, Requested, Answered, Done };

    explicit PluginCall(cmd::CommandContext& c) : ctx(&c) {}

    cmd::CommandContext* ctx;
    std::mutex m{};
    std::condition_variable cv{};
    Phase phase = Phase::PluginRuns;
    bool aborted = false;
    Request req{};
    Answer ans{};
    std::thread thread{};

    // Plugin thread: ask the UI thread, wait for the answer.
    Answer ask(Request r) {
        std::unique_lock lock(m);
        if (aborted)
            return {};
        req = std::move(r);
        phase = Phase::Requested;
        cv.notify_all();
        cv.wait(lock, [&] { return phase == Phase::Answered; });
        Answer a = std::move(ans);
        phase = Phase::PluginRuns;
        return a;
    }
    // UI thread: wait until the plugin asks something or has finished.
    bool waitForPlugin() {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return phase == Phase::Requested || phase == Phase::Done; });
        return phase == Phase::Requested;
    }
    void answer(Answer a) {
        {
            std::lock_guard lock(m);
            ans = std::move(a);
            phase = Phase::Answered;
        }
        cv.notify_all();
    }
    void finish() {
        {
            std::lock_guard lock(m);
            phase = Phase::Done;
        }
        cv.notify_all();
    }
    // The command is being torn down while the plugin waits: cancel every further prompt and
    // wait for the function to return.
    void abortAndJoin() {
        {
            std::lock_guard lock(m);
            aborted = true;
            ans = {};
            phase = Phase::Answered;
        }
        cv.notify_all();
        if (thread.joinable())
            thread.join();
    }
    ~PluginCall() { abortAndJoin(); }
};

}  // namespace

}  // namespace tbcad::plugin

// The opaque handle of the C interface.
struct TbcadCommandCtx {
    tbcad::plugin::PluginCall* call;
};

namespace tbcad::plugin {

namespace {

// ---- the C API ---------------------------------------------------------------------------------

struct Registration {
    std::string name;
    std::string description;
    TbcadCommandFn fn;
    void* user;
};

// Set while tbcad_plugin_init runs.
struct LoadState {
    std::vector<Registration>* commands = nullptr;
    std::vector<std::pair<std::string, std::string>>* menus = nullptr;
    std::vector<std::string>* log = nullptr;
    cmd::Registry* registry = nullptr;
};
LoadState* g_load = nullptr;

bool validName(const char* name) {
    if (!name || !*name)
        return false;
    for (const char* p = name; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return false;
    return true;
}

int api_register_command(const char* name, const char* description, TbcadCommandFn fn, void* user) {
    if (!g_load || !validName(name) || !fn)
        return 1;
    if (g_load->registry->find(name))
        return 2;
    for (const Registration& r : *g_load->commands)
        if (r.name == name)
            return 2;
    g_load->commands->push_back({name, description ? description : "", fn, user});
    return 0;
}

void api_add_menu_item(const char* path, const char* command) {
    if (g_load && path && command)
        g_load->menus->emplace_back(path, command);
}

void api_log(const char* message) {
    if (g_load && message)
        g_load->log->push_back(message);
}

bool live(const TbcadCommandCtx* c) {
    return c && c->call && !c->call->aborted;
}

void api_message(TbcadCommandCtx* c, const char* text) {
    if (live(c) && text)
        c->call->ctx->message(text);
}

int toRc(cmd::Status s) {
    switch (s) {
        case cmd::Status::Ok:
            return TBCAD_OK;
        case cmd::Status::Cancel:
            return TBCAD_CANCEL;
        case cmd::Status::Keyword:
            return TBCAD_KEYWORD;
        case cmd::Status::None:
            return TBCAD_NONE;
    }
    return TBCAD_ERROR;
}

int copyText(const std::string& s, char* out, size_t cap) {
    if (out && cap > 0) {
        const size_t n = std::min(s.size(), cap - 1);
        std::memcpy(out, s.data(), n);
        out[n] = 0;
    }
    return 0;
}

std::vector<std::string> splitKeywords(const char* list) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = list ? list : ""; *p; ++p) {
        if (*p == '/') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

int api_get_point(TbcadCommandCtx* c, const char* prompt, const double* base, double* x,
                  double* y) {
    if (!live(c))
        return TBCAD_CANCEL;
    Request r;
    r.kind = Ask::Point;
    r.prompt = prompt ? prompt : "Specify point";
    if (base) {
        r.hasBase = true;
        r.base = {base[0], base[1]};
    }
    Answer a = c->call->ask(std::move(r));
    if (a.status == cmd::Status::Ok) {
        if (x)
            *x = a.point.x;
        if (y)
            *y = a.point.y;
    }
    return toRc(a.status);
}

int api_get_real(TbcadCommandCtx* c, const char* prompt, double* value) {
    if (!live(c))
        return TBCAD_CANCEL;
    Request r;
    r.kind = Ask::Real;
    r.prompt = prompt ? prompt : "Enter a number";
    Answer a = c->call->ask(std::move(r));
    if (a.status == cmd::Status::Ok && value)
        *value = a.real;
    return toRc(a.status);
}

int api_get_integer(TbcadCommandCtx* c, const char* prompt, int64_t* value) {
    if (!live(c))
        return TBCAD_CANCEL;
    Request r;
    r.kind = Ask::Integer;
    r.prompt = prompt ? prompt : "Enter a whole number";
    Answer a = c->call->ask(std::move(r));
    if (a.status == cmd::Status::Ok && value)
        *value = a.integer;
    return toRc(a.status);
}

int api_get_string(TbcadCommandCtx* c, const char* prompt, char* out, size_t cap) {
    if (!live(c))
        return TBCAD_CANCEL;
    Request r;
    r.kind = Ask::String;
    r.prompt = prompt ? prompt : "Enter text";
    Answer a = c->call->ask(std::move(r));
    if (a.status == cmd::Status::Ok)
        copyText(a.text, out, cap);
    return toRc(a.status);
}

int api_get_keyword(TbcadCommandCtx* c, const char* prompt, const char* keywords, char* out,
                    size_t cap) {
    if (!live(c))
        return TBCAD_CANCEL;
    Request r;
    r.kind = Ask::Keyword;
    r.prompt = prompt ? prompt : "Choose an option";
    r.keywords = splitKeywords(keywords);
    if (r.keywords.empty())
        return TBCAD_ERROR;
    Answer a = c->call->ask(std::move(r));
    if (a.status == cmd::Status::Keyword)
        copyText(a.text, out, cap);
    return toRc(a.status);
}

int api_get_selection(TbcadCommandCtx* c, const char* prompt, uint64_t* ids, size_t cap,
                      size_t* count) {
    if (!live(c))
        return TBCAD_CANCEL;
    Request r;
    r.kind = Ask::Selection;
    r.prompt = prompt ? prompt : "Select objects";
    Answer a = c->call->ask(std::move(r));
    if (a.status == cmd::Status::Ok) {
        if (count)
            *count = a.ids.size();
        for (size_t i = 0; ids && i < cap && i < a.ids.size(); ++i)
            ids[i] = a.ids[i].value;
    }
    return toRc(a.status);
}

int addEntity(TbcadCommandCtx* c, EntityData data, uint64_t* idOut) {
    if (!live(c))
        return TBCAD_CANCEL;
    auto added = c->call->ctx->tx().add(Entity{{}, std::move(data)});
    if (!added)
        return TBCAD_ERROR;
    if (idOut)
        *idOut = added.value().value;
    return TBCAD_OK;
}

int api_add_line(TbcadCommandCtx* c, double x1, double y1, double x2, double y2, uint64_t* id) {
    return addEntity(c, LineData{{x1, y1}, {x2, y2}}, id);
}

int api_add_circle(TbcadCommandCtx* c, double cx, double cy, double r, uint64_t* id) {
    if (!(r > 0))
        return TBCAD_ERROR;
    return addEntity(c, CircleData{{cx, cy}, r}, id);
}

int api_add_polyline(TbcadCommandCtx* c, const double* xy, size_t count, int closed, uint64_t* id) {
    if (!xy || count < 2)
        return TBCAD_ERROR;
    PolylineData pl;
    for (size_t i = 0; i < count; ++i) {
        PolyVertex v;
        v.p = {xy[2 * i], xy[2 * i + 1]};
        pl.v.push_back(v);
    }
    pl.closed = closed != 0;
    return addEntity(c, std::move(pl), id);
}

int api_erase(TbcadCommandCtx* c, uint64_t id) {
    if (!live(c))
        return TBCAD_CANCEL;
    return c->call->ctx->tx().erase(EntityId{id}) ? TBCAD_OK : TBCAD_ERROR;
}

int api_move(TbcadCommandCtx* c, uint64_t id, double dx, double dy) {
    if (!live(c))
        return TBCAD_CANCEL;
    const Entity* e = c->call->ctx->doc().find(EntityId{id});
    if (!e)
        return TBCAD_ERROR;
    auto moved = geom::transformed(*e, Transform2::translate({dx, dy}));
    if (!moved)
        return TBCAD_ERROR;
    Entity result = std::move(moved).value();
    auto r = c->call->ctx->tx().modify(EntityId{id},
                                       [&](Entity& tgt) { tgt.data = std::move(result.data); });
    return r ? TBCAD_OK : TBCAD_ERROR;
}

int kindCode(EntityKind k) {
    switch (k) {
        case EntityKind::Line:
            return TBCAD_ENTITY_LINE;
        case EntityKind::Circle:
            return TBCAD_ENTITY_CIRCLE;
        case EntityKind::Arc:
            return TBCAD_ENTITY_ARC;
        case EntityKind::Polyline:
            return TBCAD_ENTITY_POLYLINE;
        case EntityKind::Point:
            return TBCAD_ENTITY_POINT;
        case EntityKind::Ellipse:
            return TBCAD_ENTITY_ELLIPSE;
        case EntityKind::Spline:
            return TBCAD_ENTITY_SPLINE;
        case EntityKind::Text:
            return TBCAD_ENTITY_TEXT;
        case EntityKind::MText:
            return TBCAD_ENTITY_MTEXT;
        case EntityKind::Hatch:
            return TBCAD_ENTITY_HATCH;
        case EntityKind::Insert:
            return TBCAD_ENTITY_INSERT;
        case EntityKind::Dimension:
            return TBCAD_ENTITY_DIMENSION;
        case EntityKind::Leader:
            return TBCAD_ENTITY_LEADER;
        default:
            return TBCAD_ENTITY_OTHER;
    }
}

int api_get_entity(TbcadCommandCtx* c, uint64_t id, TbcadEntityInfo* info) {
    if (!live(c))
        return TBCAD_CANCEL;
    const Document& doc = c->call->ctx->doc();
    const Entity* e = doc.find(EntityId{id});
    if (!e || !info)
        return TBCAD_ERROR;
    *info = TbcadEntityInfo{};
    info->kind = kindCode(kind(*e));
    const BBox2 box = geom::bounds(*e, doc);
    if (!box.empty()) {
        info->min_x = box.min.x;
        info->min_y = box.min.y;
        info->max_x = box.max.x;
        info->max_y = box.max.y;
    }
    if (const Layer* layer = doc.layer(e->common.layer))
        std::snprintf(info->layer, sizeof info->layer, "%s", layer->name.c_str());
    return TBCAD_OK;
}

int api_list_entities(TbcadCommandCtx* c, uint64_t* ids, size_t cap, size_t* count) {
    if (!live(c))
        return TBCAD_CANCEL;
    const Document& doc = c->call->ctx->doc();
    size_t n = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (!e.common.visible || doc.ownerBlock(e.common.id))
            return;
        if (ids && n < cap)
            ids[n] = e.common.id.value;
        ++n;
    });
    if (count)
        *count = n;
    return TBCAD_OK;
}

TbcadHost makeHostTable() {
    TbcadHost h{};
    h.abi_version = TBCAD_PLUGIN_ABI_VERSION;
    h.size = sizeof(TbcadHost);
    h.register_command = api_register_command;
    h.add_menu_item = api_add_menu_item;
    h.log = api_log;
    h.message = api_message;
    h.get_point = api_get_point;
    h.get_real = api_get_real;
    h.get_integer = api_get_integer;
    h.get_string = api_get_string;
    h.get_keyword = api_get_keyword;
    h.get_selection = api_get_selection;
    h.add_line = api_add_line;
    h.add_circle = api_add_circle;
    h.add_polyline = api_add_polyline;
    h.erase = api_erase;
    h.move = api_move;
    h.get_entity = api_get_entity;
    h.list_entities = api_list_entities;
    return h;
}

// ---- the command a plugin registers
// --------------------------------------------------------------

class PluginCommand final : public cmd::Command {
public:
    PluginCommand(Registration r) : reg_(std::move(r)) {}

    std::string_view name() const override { return reg_.name; }
    std::string_view description() const override { return reg_.description; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        PluginCall call(ctx);
        TbcadCommandCtx handle{&call};
        call.thread = std::thread([this, &handle, &call] {
            (void)reg_.fn(&handle, reg_.user);
            call.finish();
        });
        while (call.waitForPlugin()) {
            Request r = call.req;  // the plugin is blocked until we answer
            Answer a;
            switch (r.kind) {
                case Ask::Point: {
                    cmd::PointOptions o;
                    if (r.hasBase)
                        o.base = r.base;
                    o.allowNone = true;
                    auto res = co_await ctx.getPoint(r.prompt, std::move(o));
                    a.status = res.status;
                    a.point = res.point;
                    break;
                }
                case Ask::Real: {
                    auto res = co_await ctx.getReal(r.prompt);
                    a.status = res.status;
                    a.real = res.value;
                    break;
                }
                case Ask::Integer: {
                    auto res = co_await ctx.getInteger(r.prompt);
                    a.status = res.status;
                    a.integer = res.value;
                    break;
                }
                case Ask::String: {
                    cmd::StringOptions o;
                    o.allowEmpty = true;
                    auto res = co_await ctx.getString(r.prompt, std::move(o));
                    a.status = res.status;
                    a.text = res.text;
                    break;
                }
                case Ask::Keyword: {
                    cmd::KeywordOptions o;
                    o.keywords = r.keywords;
                    auto res = co_await ctx.getKeyword(r.prompt, std::move(o));
                    a.status = res.status;
                    a.text = res.keyword;
                    break;
                }
                case Ask::Selection: {
                    auto res = co_await ctx.getSelection(r.prompt);
                    a.status = res.status;
                    a.ids = std::move(res.ids);
                    break;
                }
            }
            call.answer(std::move(a));
        }
        call.abortAndJoin();
    }

private:
    Registration reg_;
};

class PluginsCommand final : public cmd::Command {
public:
    PluginsCommand(const std::vector<PluginInfo>* infos, std::filesystem::path folder)
        : infos_(infos), folder_(std::move(folder)) {}
    std::string_view name() const override { return "PLUGINS"; }
    std::string_view description() const override { return "List the loaded plugins"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        if (infos_->empty()) {
            ctx.message("No plugins loaded. Put plugin libraries (" +
                        std::string(libraryExtension()) + ") in " + folder_.string() +
                        " and restart.");
            co_return;
        }
        for (const PluginInfo& p : *infos_) {
            std::string line = p.name;
            if (!p.version.empty())
                line += " " + p.version;
            if (!p.error.empty()) {
                line += ": not loaded (" + p.error + ")";
            } else {
                line += ":";
                for (const std::string& c : p.commands)
                    line += " " + c;
            }
            ctx.message(line);
        }
    }

private:
    const std::vector<PluginInfo>* infos_;
    std::filesystem::path folder_;
};

}  // namespace

struct PluginHost::Impl {
    std::vector<DynLib> libs{};
    std::vector<PluginInfo> infos{};
    std::vector<std::string> log{};
    std::vector<std::pair<std::string, std::string>> menus{};
};

PluginHost::PluginHost() : impl_(std::make_unique<Impl>()) {}
PluginHost::~PluginHost() = default;

const std::vector<PluginInfo>& PluginHost::plugins() const {
    return impl_->infos;
}

std::vector<std::string> PluginHost::takeLog() {
    return std::exchange(impl_->log, {});
}

void PluginHost::loadFolder(const std::filesystem::path& folder, cmd::Registry& registry) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == libraryExtension())
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files)
        loadFile(f, registry);
}

void PluginHost::loadFile(const std::filesystem::path& file, cmd::Registry& registry) {
    PluginInfo info;
    info.file = file;
    info.name = file.stem().string();

    DynLib lib;
    if (std::string err = lib.open(file); !err.empty()) {
        info.error = err;
        impl_->log.push_back("Plugin " + info.name + " could not be loaded: " + err);
        impl_->infos.push_back(std::move(info));
        return;
    }
    using InitFn = int (*)(const TbcadHost*);
    using InfoFn = const TbcadPluginInfo* (*)();
    auto init = reinterpret_cast<InitFn>(lib.symbol("tbcad_plugin_init"));
    if (!init) {
        info.error = "tbcad_plugin_init is missing";
        impl_->log.push_back("Plugin " + info.name +
                             " is not a TBCAD plugin (no tbcad_plugin_init).");
        impl_->infos.push_back(std::move(info));
        return;
    }
    if (auto infoFn = reinterpret_cast<InfoFn>(lib.symbol("tbcad_plugin_info"))) {
        if (const TbcadPluginInfo* pi = infoFn()) {
            if (pi->abi_version != TBCAD_PLUGIN_ABI_VERSION) {
                info.error = "built for plugin API version " + std::to_string(pi->abi_version) +
                             ", this program has " + std::to_string(TBCAD_PLUGIN_ABI_VERSION);
                impl_->log.push_back("Plugin " + info.name + " skipped: " + info.error);
                impl_->infos.push_back(std::move(info));
                return;
            }
            if (pi->name && *pi->name)
                info.name = pi->name;
            if (pi->version)
                info.version = pi->version;
            if (pi->author)
                info.author = pi->author;
        }
    }

    std::vector<Registration> commands;
    std::vector<std::pair<std::string, std::string>> menus;
    std::vector<std::string> log;
    LoadState state{&commands, &menus, &log, &registry};
    g_load = &state;
    static const TbcadHost host = makeHostTable();
    const int rc = init(&host);
    g_load = nullptr;
    for (std::string& l : log)
        impl_->log.push_back(info.name + ": " + l);
    if (rc != 0) {
        info.error = "initialisation failed (code " + std::to_string(rc) + ")";
        impl_->log.push_back("Plugin " + info.name + " failed to start: " + info.error);
        impl_->infos.push_back(std::move(info));
        return;  // `lib` unloads; its commands were never added
    }
    for (Registration& r : commands) {
        info.commands.push_back(r.name);
        (void)registry.add(std::make_unique<PluginCommand>(std::move(r)));
    }
    for (auto& m : menus)
        impl_->menus.push_back(std::move(m));
    impl_->log.push_back("Plugin " + info.name + " loaded (" +
                         std::to_string(info.commands.size()) + " command(s)).");
    impl_->libs.push_back(std::move(lib));
    impl_->infos.push_back(std::move(info));
}

void PluginHost::registerListCommand(cmd::Registry& registry,
                                     std::filesystem::path pluginFolder) const {
    (void)registry.add(std::make_unique<PluginsCommand>(&impl_->infos, std::move(pluginFolder)));
}

void PluginHost::registerUi(ui::UiRegistry& ui) const {
    for (const auto& [path, command] : impl_->menus)
        ui.addMenuItem(path, command);
}

}  // namespace tbcad::plugin
