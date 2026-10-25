#include "tbcad/lispcmd/lispcmd.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <thread>

#include "tbcad/cmd/cmd.h"
#include "tbcad/lisp/lisp.h"

namespace tbcad::lispcmd {

namespace {

using lisp::Args;
using lisp::Error;
using lisp::Interp;
using lisp::Value;
using K = Value::Kind;

// ---- talking to the command framework
// -------------------------------------------------------------- A program runs on its own thread
// so (getpoint) can wait for the user while the command coroutine does the prompting. Only one of
// the two threads runs at any moment.

struct Ask {
    enum class Kind { Point, Distance, Angle, Real, Int, String, Keyword, Selection, Entity };
    Kind kind = Kind::Point;
    std::string prompt{};
    std::optional<Vec2> base{};
    std::vector<std::string> keywords{};
    bool spaces = false;
};

struct Reply {
    bool cancelled = false;  // Esc
    bool none = false;       // Enter
    Vec2 point{};
    double real = 0;
    long long integer = 0;
    std::string text{};
    std::vector<EntityId> ids{};
    EntityId entity{};
};

class Run {
public:
    enum class State { Interp, Asking, Done };

    explicit Run(std::function<Value()> program) : program_(std::move(program)) {
        thread_ = std::thread([this] { body(); });
    }
    ~Run() {
        {
            std::lock_guard lock(m_);
            cancel_ = true;
            if (state_ == State::Asking)
                state_ = State::Interp;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    // Coroutine side: blocks until the program asks the user something or ends.
    State wait() {
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return state_ != State::Interp; });
        return state_;
    }
    const Ask& ask() const { return ask_; }
    void answer(Reply r) {
        {
            std::lock_guard lock(m_);
            reply_ = std::move(r);
            state_ = State::Interp;
        }
        cv_.notify_all();
    }
    // Program side.
    Reply askUser(Ask a) {
        std::unique_lock lock(m_);
        if (cancel_)
            throw lisp::Abort();
        ask_ = std::move(a);
        state_ = State::Asking;
        cv_.notify_all();
        cv_.wait(lock, [&] { return state_ == State::Interp; });
        if (cancel_)
            throw lisp::Abort();
        return std::move(reply_);
    }

    // Valid once wait() returned Done.
    const Value& result() const { return result_; }
    const std::string& error() const { return error_; }
    bool aborted() const { return aborted_; }

private:
    void body() {
        try {
            result_ = program_();
        } catch (const lisp::Abort&) {
            aborted_ = true;
        } catch (const Error& e) {
            error_ = e.what();
        } catch (const std::exception& e) {
            error_ = e.what();
        }
        {
            std::lock_guard lock(m_);
            state_ = State::Done;
        }
        cv_.notify_all();
    }

    std::function<Value()> program_;
    std::mutex m_;
    std::condition_variable cv_;
    State state_ = State::Interp;
    bool cancel_ = false;
    Ask ask_{};
    Reply reply_{};
    Value result_{};
    std::string error_{};
    bool aborted_ = false;
    std::thread thread_{};
};

// ---- drawing access
// ----------------------------------------------------------------------------------

std::string upperCase(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const char* kindName(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
            return "LINE";
        case EntityKind::Circle:
            return "CIRCLE";
        case EntityKind::Arc:
            return "ARC";
        case EntityKind::Polyline:
            return "LWPOLYLINE";
        case EntityKind::Point:
            return "POINT";
        case EntityKind::Ellipse:
            return "ELLIPSE";
        case EntityKind::Spline:
            return "SPLINE";
        case EntityKind::Text:
            return "TEXT";
        case EntityKind::MText:
            return "MTEXT";
        case EntityKind::Hatch:
            return "HATCH";
        case EntityKind::Insert:
            return "INSERT";
        case EntityKind::Dimension:
            return "DIMENSION";
        case EntityKind::Leader:
            return "LEADER";
        case EntityKind::Table:
            return "ACAD_TABLE";
        case EntityKind::Ray:
            return "RAY";
        case EntityKind::MLine:
            return "MLINE";
        case EntityKind::MLeader:
            return "MULTILEADER";
        default:
            return "PROXY";
    }
}

Value pt3(Vec2 p) {
    return Value::list({Value::real(p.x), Value::real(p.y), Value::real(0)});
}
Value group(long long code, Value v) {
    return Value::cons(Value::integer(code), std::move(v));
}
Value groupPt(long long code, Vec2 p) {
    return Value::cons(Value::integer(code), pt3(p));
}

Vec2 toPoint(const Value& v) {
    auto c = lisp::toVector(v);
    if (c.size() < 2 || !c[0].isNumber() || !c[1].isNumber())
        throw Error("bad point argument");
    return {c[0].number(), c[1].number()};
}

double normAngle(double a) {
    a = std::fmod(a, 2 * std::numbers::pi);
    return a < 0 ? a + 2 * std::numbers::pi : a;
}

}  // namespace

// ---- the session
// -------------------------------------------------------------------------------------

struct Session : std::enable_shared_from_this<Session> {
    Interp interp{};
    cmd::Registry* registry = nullptr;
    cmd::CommandContext* ctx = nullptr;  // set while a program runs from a command
    Run* run = nullptr;
    std::vector<std::string> queued{};  // lines (command ...) wants typed once the program ends
    std::filesystem::path settings{};
    bool startupDone = false;
    std::vector<std::string> initgetKeywords{};
    std::function<void(const std::string&)> print{};
    std::function<void(const std::string&)> makeCommand{};  // registers (defun c:NAME ...)
    std::string outBuf{};

    // princ and friends arrive in pieces; the command line shows whole lines.
    void emit(const std::string& piece) {
        outBuf += piece;
        for (auto nl = outBuf.find('\n'); nl != std::string::npos; nl = outBuf.find('\n')) {
            if (print)
                print(outBuf.substr(0, nl));
            outBuf.erase(0, nl + 1);
        }
    }
    void flushOut() {
        if (!outBuf.empty() && print)
            print(outBuf);
        outBuf.clear();
    }

    Document& doc() {
        if (!ctx)
            throw Error("no drawing is open");
        return ctx->doc();
    }
    Transaction& tx() {
        if (!ctx)
            throw Error("no drawing is open");
        return ctx->tx();
    }

    Session();
    void defineDrawingFunctions();
    Value entityList(EntityId id);
    Value entMake(const Value& list);
    Value entMod(const Value& list);
    Value select(const Value& mode, const Value& filter, Run* r);
    LayerId layerNamed(const std::string& name);
    std::filesystem::path resolve(const std::string& name) const;
    bool readFile(const std::string& name, std::string& out) const;
    Value loadFile(const std::string& name);
};

namespace {

// Group codes of an entmake / entmod list: first value of each code, and all of them in order.
struct Groups {
    std::vector<std::pair<long long, Value>> all{};
    const Value* find(long long code) const {
        for (const auto& g : all)
            if (g.first == code)
                return &g.second;
        return nullptr;
    }
};

Groups readGroups(const Value& list) {
    Groups g;
    for (const auto& item : lisp::toVector(list)) {
        if (item.kind != K::Pair || item.pair->car.kind != K::Int)
            throw Error("bad argument type");
        g.all.emplace_back(item.pair->car.i, item.pair->cdr);
    }
    return g;
}

double groupReal(const Value* v, double fallback = 0) {
    if (!v)
        return fallback;
    if (!v->isNumber())
        throw Error("bad argument type");
    return v->number();
}

Vec2 groupPoint(const Value* v) {
    if (!v)
        throw Error("missing group code");
    return toPoint(*v);
}

}  // namespace

Session::Session() {
    interp.out = [this](const std::string& s) { emit(s); };
    interp.loader = [this](const std::string& name, std::string& out) {
        return readFile(name, out);
    };
    interp.onCommandDefined = [this](const std::string& name) {
        if (registry && !registry->find(name) && makeCommand)
            makeCommand(name);
    };
    defineDrawingFunctions();
}

LayerId Session::layerNamed(const std::string& name) {
    if (const Layer* l = doc().layerByName(name))
        return l->id;
    Layer row;
    row.name = name;
    auto id = tx().addLayer(row);
    if (!id)
        throw Error(id.error().message);
    return id.value();
}

Value Session::entityList(EntityId id) {
    const Entity* e = doc().find(id);
    if (!e)
        return Value::nil();
    std::vector<Value> out;
    out.push_back(group(-1, Value::ename(id.value)));
    out.push_back(group(0, Value::str(kindName(*e))));
    const Layer* layer = doc().layer(e->common.layer);
    out.push_back(group(8, Value::str(layer ? layer->name : "0")));
    if (e->common.color.kind == ColorKind::Index)
        out.push_back(group(62, Value::integer(e->common.color.indexValue())));
    if (const auto* d = entityAs<LineData>(*e)) {
        out.push_back(groupPt(10, d->a));
        out.push_back(groupPt(11, d->b));
    } else if (const auto* c = entityAs<CircleData>(*e)) {
        out.push_back(groupPt(10, c->center));
        out.push_back(group(40, Value::real(c->radius)));
    } else if (const auto* a = entityAs<ArcData>(*e)) {
        out.push_back(groupPt(10, a->center));
        out.push_back(group(40, Value::real(a->radius)));
        out.push_back(group(50, Value::real(a->startAngle)));
        out.push_back(group(51, Value::real(a->endAngle)));
    } else if (const auto* p = entityAs<PointData>(*e)) {
        out.push_back(groupPt(10, p->p));
    } else if (const auto* t = entityAs<TextData>(*e)) {
        out.push_back(group(1, Value::str(t->utf8)));
        out.push_back(groupPt(10, t->position));
        out.push_back(group(40, Value::real(t->height)));
        out.push_back(group(50, Value::real(t->rotation)));
    } else if (const auto* m = entityAs<MTextData>(*e)) {
        out.push_back(group(1, Value::str(m->utf8)));
        out.push_back(groupPt(10, m->position));
    } else if (const auto* pl = entityAs<PolylineData>(*e)) {
        out.push_back(group(90, Value::integer(static_cast<long long>(pl->v.size()))));
        out.push_back(group(70, Value::integer(pl->closed ? 1 : 0)));
        for (const auto& v : pl->v) {
            out.push_back(Value::cons(Value::integer(10),
                                      Value::list({Value::real(v.p.x), Value::real(v.p.y)})));
            if (v.bulge != 0)
                out.push_back(group(42, Value::real(v.bulge)));
        }
    } else if (const auto* ins = entityAs<InsertData>(*e)) {
        const BlockDef* b = doc().block(ins->block);
        out.push_back(group(2, Value::str(b ? b->name : "")));
        out.push_back(groupPt(10, ins->position));
        out.push_back(group(41, Value::real(ins->scale.x)));
        out.push_back(group(42, Value::real(ins->scale.y)));
        out.push_back(group(50, Value::real(ins->rotation)));
    }
    return Value::list(out);
}

namespace {

void applyCommon(Session& s, EntityCommon& c, const Groups& g) {
    if (const Value* layer = g.find(8)) {
        if (layer->kind != K::Str)
            throw Error("bad argument type");
        c.layer = s.layerNamed(layer->s);
    }
    if (const Value* color = g.find(62)) {
        if (color->kind != K::Int)
            throw Error("bad argument type");
        if (color->i == 256)
            c.color = Color::byLayer();
        else if (color->i == 0)
            c.color = Color::byBlock();
        else if (color->i > 0 && color->i < 256)
            c.color = Color::index(static_cast<std::uint8_t>(color->i));
        else
            throw Error("invalid color");
    }
}

}  // namespace

Value Session::entMake(const Value& list) {
    Groups g = readGroups(list);
    const Value* type = g.find(0);
    if (!type || type->kind != K::Str)
        return Value::nil();
    const std::string t = upperCase(type->s);
    Entity e;
    if (t == "LINE") {
        e.data = LineData{groupPoint(g.find(10)), groupPoint(g.find(11))};
    } else if (t == "CIRCLE") {
        double r = groupReal(g.find(40));
        if (!(r > 0))
            return Value::nil();
        e.data = CircleData{groupPoint(g.find(10)), r};
    } else if (t == "ARC") {
        double r = groupReal(g.find(40));
        if (!(r > 0))
            return Value::nil();
        e.data = ArcData{groupPoint(g.find(10)), r, normAngle(groupReal(g.find(50))),
                         normAngle(groupReal(g.find(51)))};
    } else if (t == "POINT") {
        e.data = PointData{groupPoint(g.find(10))};
    } else if (t == "TEXT") {
        TextData d;
        d.position = groupPoint(g.find(10));
        d.height = groupReal(g.find(40), 2.5);
        d.rotation = groupReal(g.find(50));
        if (const Value* s = g.find(1); s && s->kind == K::Str)
            d.utf8 = s->s;
        e.data = Indirect<TextData>(std::move(d));
    } else if (t == "LWPOLYLINE") {
        PolylineData d;
        for (const auto& [code, v] : g.all) {
            if (code == 10)
                d.v.push_back(PolyVertex{toPoint(v), 0, 0, 0});
            else if (code == 42 && !d.v.empty() && v.isNumber())
                d.v.back().bulge = v.number();
        }
        if (d.v.size() < 2)
            return Value::nil();
        if (const Value* f = g.find(70); f && f->kind == K::Int)
            d.closed = (f->i & 1) != 0;
        e.data = std::move(d);
    } else {
        return Value::nil();
    }
    applyCommon(*this, e.common, g);
    auto id = tx().add(std::move(e));
    if (!id)
        return Value::nil();
    return list;
}

Value Session::entMod(const Value& list) {
    Groups g = readGroups(list);
    const Value* name = g.find(-1);
    if (!name || name->kind != K::Ename)
        return Value::nil();
    EntityId id{static_cast<std::uint64_t>(name->i)};
    if (!doc().contains(id))
        return Value::nil();
    // Resolve the layer first: creating it needs the transaction outside the modify callback.
    if (const Value* l = g.find(8); l && l->kind == K::Str)
        (void)layerNamed(l->s);
    std::string failure;
    auto r = tx().modify(id, [&](Entity& e) {
        try {
            applyCommon(*this, e.common, g);
            if (auto* d = entityAs<LineData>(e)) {
                if (g.find(10))
                    d->a = groupPoint(g.find(10));
                if (g.find(11))
                    d->b = groupPoint(g.find(11));
            } else if (auto* c = entityAs<CircleData>(e)) {
                if (g.find(10))
                    c->center = groupPoint(g.find(10));
                if (g.find(40) && groupReal(g.find(40)) > 0)
                    c->radius = groupReal(g.find(40));
            } else if (auto* a = entityAs<ArcData>(e)) {
                if (g.find(10))
                    a->center = groupPoint(g.find(10));
                if (g.find(40) && groupReal(g.find(40)) > 0)
                    a->radius = groupReal(g.find(40));
                if (g.find(50))
                    a->startAngle = normAngle(groupReal(g.find(50)));
                if (g.find(51))
                    a->endAngle = normAngle(groupReal(g.find(51)));
            } else if (auto* p = entityAs<PointData>(e)) {
                if (g.find(10))
                    p->p = groupPoint(g.find(10));
            } else if (auto* t = entityAs<TextData>(e)) {
                if (g.find(10))
                    t->position = groupPoint(g.find(10));
                if (g.find(40) && groupReal(g.find(40)) > 0)
                    t->height = groupReal(g.find(40));
                if (g.find(50))
                    t->rotation = groupReal(g.find(50));
                if (const Value* s = g.find(1); s && s->kind == K::Str)
                    t->utf8 = s->s;
            } else if (auto* m = entityAs<MTextData>(e)) {
                if (g.find(10))
                    m->position = groupPoint(g.find(10));
                if (const Value* s = g.find(1); s && s->kind == K::Str)
                    m->utf8 = s->s;
            } else if (auto* pl = entityAs<PolylineData>(e)) {
                std::vector<PolyVertex> verts;
                for (const auto& [code, v] : g.all) {
                    if (code == 10)
                        verts.push_back(PolyVertex{toPoint(v), 0, 0, 0});
                    else if (code == 42 && !verts.empty() && v.isNumber())
                        verts.back().bulge = v.number();
                }
                if (verts.size() >= 2)
                    pl->v = std::move(verts);
                if (const Value* f = g.find(70); f && f->kind == K::Int)
                    pl->closed = (f->i & 1) != 0;
            }
        } catch (const Error& err) {
            failure = err.what();
        }
    });
    if (!r)
        return Value::nil();
    if (!failure.empty())
        throw Error(failure);
    return list;
}

namespace {

bool matchesFilter(Session& s, EntityId id, const Groups& f) {
    const Entity* e = s.doc().find(id);
    if (!e)
        return false;
    for (const auto& [code, v] : f.all) {
        if (code == 0) {
            if (v.kind != K::Str || !lisp::wildcardMatch(v.s, kindName(*e)))
                return false;
        } else if (code == 8) {
            const Layer* l = s.doc().layer(e->common.layer);
            if (v.kind != K::Str || !lisp::wildcardMatch(v.s, l ? l->name : "0"))
                return false;
        } else if (code == 62) {
            const Color& c = e->common.color;
            long long idx = c.kind == ColorKind::Index ? c.indexValue()
                                                       : (c.kind == ColorKind::ByBlock ? 0 : 256);
            if (v.kind != K::Int || v.i != idx)
                return false;
        }
    }
    return true;
}

Groups filterGroups(const Value& filter) {
    Groups f;
    for (const auto& item : lisp::toVector(filter)) {
        if (item.kind != K::Pair || item.pair->car.kind != K::Int)
            throw Error("bad argument type");
        f.all.emplace_back(item.pair->car.i, item.pair->cdr);
    }
    return f;
}

}  // namespace

Value Session::select(const Value& mode, const Value& filter, Run* r) {
    Groups f = filterGroups(filter);
    std::vector<std::uint64_t> ids;
    bool all = mode.kind == K::Str && (upperCase(mode.s) == "X" || upperCase(mode.s) == "_X");
    bool previous = mode.kind == K::Str && (upperCase(mode.s) == "P" || upperCase(mode.s) == "_P");
    if (all) {
        for (EntityId id : doc().modelSpace())
            if (matchesFilter(*this, id, f))
                ids.push_back(id.value);
    } else if (previous || (mode.kind == K::Str && upperCase(mode.s) == "I")) {
        if (ctx)
            for (EntityId id : ctx->host().selection())
                if (matchesFilter(*this, id, f))
                    ids.push_back(id.value);
    } else {
        if (!r)
            throw Error("ssget needs a command line to pick objects");
        Ask a;
        a.kind = Ask::Kind::Selection;
        a.prompt = "Select objects";
        Reply rep = r->askUser(std::move(a));
        if (rep.cancelled)
            throw lisp::Abort();
        for (EntityId id : rep.ids)
            if (matchesFilter(*this, id, f))
                ids.push_back(id.value);
    }
    if (ids.empty())
        return Value::nil();
    return Value::pickSet(std::move(ids));
}

void Session::defineDrawingFunctions() {
    interp.defineSubr("ENTGET", [this](Interp&, Args& a) {
        if (a.empty() || a[0].kind != K::Ename)
            throw Error("bad argument type");
        return entityList(EntityId{static_cast<std::uint64_t>(a[0].i)});
    });
    interp.defineSubr("ENTMAKE", [this](Interp&, Args& a) {
        if (a.empty())
            throw Error("too few arguments");
        return entMake(a[0]);
    });
    interp.defineSubr("ENTMOD", [this](Interp&, Args& a) {
        if (a.empty())
            throw Error("too few arguments");
        return entMod(a[0]);
    });
    interp.defineSubr("ENTDEL", [this](Interp&, Args& a) {
        if (a.empty() || a[0].kind != K::Ename)
            throw Error("bad argument type");
        auto r = tx().erase(EntityId{static_cast<std::uint64_t>(a[0].i)});
        return r ? a[0] : Value::nil();
    });
    interp.defineSubr("ENTLAST", [this](Interp&, Args&) {
        auto ids = doc().modelSpace();
        return ids.empty() ? Value::nil() : Value::ename(ids.back().value);
    });
    interp.defineSubr("ENTNEXT", [this](Interp&, Args& a) {
        auto ids = doc().modelSpace();
        if (a.empty() || a[0].isNil())
            return ids.empty() ? Value::nil() : Value::ename(ids.front().value);
        if (a[0].kind != K::Ename)
            throw Error("bad argument type");
        for (std::size_t k = 0; k + 1 < ids.size(); ++k)
            if (static_cast<long long>(ids[k].value) == a[0].i)
                return Value::ename(ids[k + 1].value);
        return Value::nil();
    });
    interp.defineSubr("SSGET", [this](Interp&, Args& a) {
        Value mode, filter;
        if (!a.empty() && a[0].kind == K::Str) {
            mode = a[0];
            // ("X" filter) or ("W" p1 p2 filter): the filter is the last list.
            if (a.size() > 1 && (a.back().kind == K::Pair))
                filter = a.back();
        } else if (!a.empty()) {
            filter = a[0];
        }
        return select(mode, filter, run);
    });
    interp.defineSubr("SSLENGTH", [](Interp&, Args& a) {
        if (a.empty() || a[0].kind != K::PickSet)
            throw Error("bad argument type");
        return Value::integer(static_cast<long long>(a[0].set->size()));
    });
    interp.defineSubr("SSNAME", [](Interp&, Args& a) {
        if (a.size() < 2 || a[0].kind != K::PickSet || a[1].kind != K::Int)
            throw Error("bad argument type");
        if (a[1].i < 0 || a[1].i >= static_cast<long long>(a[0].set->size()))
            return Value::nil();
        return Value::ename((*a[0].set)[static_cast<std::size_t>(a[1].i)]);
    });
    interp.defineSubr("SSADD", [](Interp&, Args& a) {
        if (a.empty() || a[0].kind != K::Ename)
            throw Error("bad argument type");
        std::vector<std::uint64_t> ids;
        if (a.size() > 1 && a[1].kind == K::PickSet)
            ids = *a[1].set;
        auto id = static_cast<std::uint64_t>(a[0].i);
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
        return Value::pickSet(std::move(ids));
    });
    interp.defineSubr("SSDEL", [](Interp&, Args& a) {
        if (a.size() < 2 || a[0].kind != K::Ename || a[1].kind != K::PickSet)
            throw Error("bad argument type");
        std::vector<std::uint64_t> ids = *a[1].set;
        ids.erase(std::remove(ids.begin(), ids.end(), static_cast<std::uint64_t>(a[0].i)),
                  ids.end());
        return Value::pickSet(std::move(ids));
    });
    interp.defineSubr("SSMEMB", [](Interp&, Args& a) {
        if (a.size() < 2 || a[0].kind != K::Ename || a[1].kind != K::PickSet)
            throw Error("bad argument type");
        const auto& ids = *a[1].set;
        return std::find(ids.begin(), ids.end(), static_cast<std::uint64_t>(a[0].i)) == ids.end()
                   ? Value::nil()
                   : a[0];
    });

    // ---- user input ----
    auto prompter = [this](Ask::Kind kind) {
        return [this, kind](Interp&, Args& a) {
            if (!run)
                throw Error("no command line to ask on");
            Ask ask;
            ask.kind = kind;
            for (const auto& v : a) {
                if (v.kind == K::Str)
                    ask.prompt = v.s;
                else if (v.kind == K::Pair && !ask.base)
                    ask.base = toPoint(v);
            }
            if (kind == Ask::Kind::String && !a.empty() && a[0].kind == K::Int)
                ask.spaces = a[0].i != 0;
            if (kind == Ask::Kind::String && !a.empty() && !a[0].isNil() && a[0].kind != K::Str)
                ask.spaces = true;
            if (kind == Ask::Kind::Keyword)
                ask.keywords = initgetKeywords;
            Reply r = run->askUser(std::move(ask));
            if (r.cancelled)
                throw lisp::Abort();
            if (r.none)
                return Value::nil();
            switch (kind) {
                case Ask::Kind::Point:
                    return pt3(r.point);
                case Ask::Kind::Distance:
                case Ask::Kind::Angle:
                case Ask::Kind::Real:
                    return Value::real(r.real);
                case Ask::Kind::Int:
                    return Value::integer(r.integer);
                case Ask::Kind::String:
                case Ask::Kind::Keyword:
                    return Value::str(r.text);
                case Ask::Kind::Entity:
                    if (!r.entity)
                        return Value::nil();
                    return Value::list({Value::ename(r.entity.value), pt3(r.point)});
                default:
                    return Value::nil();
            }
        };
    };
    interp.defineSubr("GETPOINT", prompter(Ask::Kind::Point));
    interp.defineSubr("GETCORNER", prompter(Ask::Kind::Point));
    interp.defineSubr("GETDIST", prompter(Ask::Kind::Distance));
    interp.defineSubr("GETANGLE", prompter(Ask::Kind::Angle));
    interp.defineSubr("GETORIENT", prompter(Ask::Kind::Angle));
    interp.defineSubr("GETREAL", prompter(Ask::Kind::Real));
    interp.defineSubr("GETINT", prompter(Ask::Kind::Int));
    interp.defineSubr("GETSTRING", prompter(Ask::Kind::String));
    interp.defineSubr("GETKWORD", prompter(Ask::Kind::Keyword));
    interp.defineSubr("ENTSEL", prompter(Ask::Kind::Entity));
    interp.defineSubr("INITGET", [this](Interp&, Args& a) {
        initgetKeywords.clear();
        for (const auto& v : a) {
            if (v.kind != K::Str)
                continue;
            // The keyword list is the string with spaces between words; a bit-code int is ignored.
            std::istringstream words(v.s);
            std::string w;
            while (words >> w)
                initgetKeywords.push_back(w);
        }
        return Value::nil();
    });

    // ---- COMMAND: lines typed once this program ends ----
    interp.defineSubr("COMMAND", [this](Interp&, Args& a) {
        bool first = true;
        for (const auto& v : a) {
            std::string line;
            switch (v.kind) {
                case K::Str:
                    line = v.s;
                    if (first) {
                        while (!line.empty() && (line[0] == '_' || line[0] == '.'))
                            line.erase(0, 1);
                    } else if (line.size() > 1 && line[0] == '_' &&
                               std::isalpha(static_cast<unsigned char>(line[1])) != 0) {
                        line.erase(0, 1);
                    }
                    break;
                case K::Int:
                    line = std::to_string(v.i);
                    break;
                case K::Real: {
                    std::ostringstream os;
                    os.precision(15);
                    os << v.r;
                    line = os.str();
                    break;
                }
                case K::Pair: {
                    auto c = lisp::toVector(v);
                    if (c.size() < 2 || !c[0].isNumber() || !c[1].isNumber())
                        throw Error("bad argument type");
                    std::ostringstream os;
                    os.precision(15);
                    os << c[0].number() << ',' << c[1].number();
                    line = os.str();
                    break;
                }
                case K::Ename:
                    continue;  // object picks are not supported through the script path
                default:
                    continue;
            }
            queued.push_back(std::move(line));
            first = false;
        }
        return Value::nil();
    });
}

std::filesystem::path Session::resolve(const std::string& name) const {
    std::filesystem::path p{std::u8string(name.begin(), name.end())};
    if (p.extension().empty())
        p += ".lsp";
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec))
        return p;
    if (!settings.empty() && p.is_relative()) {
        auto q = settings / p;
        if (std::filesystem::is_regular_file(q, ec))
            return q;
    }
    return {};
}

bool Session::readFile(const std::string& name, std::string& out) const {
    auto p = resolve(name);
    if (p.empty())
        return false;
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    // A byte order mark is not part of the program.
    if (out.rfind("\xEF\xBB\xBF", 0) == 0)
        out.erase(0, 3);
    return true;
}

Value Session::loadFile(const std::string& name) {
    std::string src;
    if (!readFile(name, src))
        throw Error("cannot open \"" + name + "\"");
    return interp.evalText(src);
}

namespace {

// ---- commands
// ------------------------------------------------------------------------------------------

std::string stripColon(std::string p) {
    while (!p.empty() && (p.back() == ' ' || p.back() == ':'))
        p.pop_back();
    return p.empty() ? "Specify" : p;
}

class LispCommandBase : public cmd::Command {
public:
    explicit LispCommandBase(std::shared_ptr<Session> s) : session_(std::move(s)) {}

protected:
    enum class Mode { Lisp, AppLoad, User };

    // Runs a program and answers whatever it asks. Parameters are by value: they must outlive
    // the suspended coroutine.
    cmd::Task drive(cmd::CommandContext& ctx, Mode mode, std::string userName) {
        Session& s = *session_;
        s.registry = &ctx.registry();
        s.settings = ctx.host().settingsFolder();
        s.print = [&ctx](const std::string& t) { ctx.message(t); };
        std::string text;
        bool startup = false;
        if (mode == Mode::Lisp) {
            auto r = co_await ctx.getString("Enter a LISP expression");
            if (!r.ok())
                co_return;
            text = r.text;
        } else if (mode == Mode::AppLoad) {
            auto r = co_await ctx.getString(
                "File to load (.lsp), add ,S to add it to the startup suite");
            if (!r.ok())
                co_return;
            text = r.text;
            auto comma = text.rfind(',');
            if (comma != std::string::npos) {
                std::string tail = upperCase(text.substr(comma + 1));
                tail.erase(std::remove(tail.begin(), tail.end(), ' '), tail.end());
                if (tail == "S" || tail == "STARTUP") {
                    startup = true;
                    text.erase(comma);
                }
            }
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
            if (text.size() > 1 && text.front() == '"' && text.back() == '"')
                text = text.substr(1, text.size() - 2);
        }

        s.queued.clear();
        s.ctx = &ctx;
        std::function<Value()> program;
        if (mode == Mode::Lisp)
            program = [&s, text] { return s.interp.evalText(text); };
        else if (mode == Mode::AppLoad)
            program = [&s, text] { return s.loadFile(text); };
        else
            program = [&s, userName] { return s.interp.evalText("(C:" + userName + ")"); };
        auto run = std::make_unique<Run>(std::move(program));
        s.run = run.get();

        for (;;) {
            if (run->wait() == Run::State::Done)
                break;
            const Ask ask = run->ask();
            Reply rep;
            const std::string prompt = stripColon(ask.prompt);
            switch (ask.kind) {
                case Ask::Kind::Point: {
                    cmd::PointOptions o;
                    o.base = ask.base;
                    auto r = co_await ctx.getPoint(prompt, std::move(o));
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None;
                    rep.point = r.point;
                    break;
                }
                case Ask::Kind::Distance: {
                    cmd::DistanceOptions o;
                    o.base = ask.base;
                    auto r = co_await ctx.getDistance(prompt, std::move(o));
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None;
                    rep.real = r.value;
                    break;
                }
                case Ask::Kind::Angle: {
                    cmd::AngleOptions o;
                    o.base = ask.base;
                    auto r = co_await ctx.getAngle(prompt, std::move(o));
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None;
                    rep.real = r.value;
                    break;
                }
                case Ask::Kind::Real: {
                    auto r = co_await ctx.getReal(prompt);
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None;
                    rep.real = r.value;
                    break;
                }
                case Ask::Kind::Int: {
                    auto r = co_await ctx.getInteger(prompt);
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None;
                    rep.integer = r.value;
                    break;
                }
                case Ask::Kind::String: {
                    cmd::StringOptions o;
                    o.allowEmpty = true;
                    auto r = co_await ctx.getString(prompt, std::move(o));
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None || (r.ok() && r.text.empty());
                    rep.text = r.text;
                    break;
                }
                case Ask::Kind::Keyword: {
                    if (ask.keywords.empty()) {
                        rep.none = true;
                        break;
                    }
                    cmd::KeywordOptions o;
                    o.keywords = ask.keywords;
                    auto r = co_await ctx.getKeyword(prompt, std::move(o));
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None;
                    rep.text = r.keyword;
                    break;
                }
                case Ask::Kind::Selection: {
                    auto r = co_await ctx.getSelection(prompt);
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None || (r.ok() && r.ids.empty());
                    rep.ids = r.ids;
                    break;
                }
                case Ask::Kind::Entity: {
                    auto r = co_await ctx.getEntity(prompt);
                    rep.cancelled = r.status == cmd::Status::Cancel;
                    rep.none = r.status == cmd::Status::None;
                    rep.entity = r.id;
                    rep.point = r.pickPoint;
                    break;
                }
            }
            run->answer(std::move(rep));
        }

        s.run = nullptr;
        s.ctx = nullptr;
        s.flushOut();
        const bool aborted = run->aborted();
        const std::string error = run->error();
        const Value result = run->result();
        run.reset();  // joins the program thread

        if (aborted) {
            ctx.message("*Cancel*");
            co_return;
        }
        if (!error.empty()) {
            ctx.message("; error: " + error);
            co_return;
        }
        if (mode == Mode::Lisp) {
            ctx.message(lisp::toString(result, true));
        } else if (mode == Mode::AppLoad) {
            ctx.message("Loaded " + text);
            if (startup) {
                auto file = s.settings / "startup-suite.txt";
                std::error_code ec;
                std::filesystem::create_directories(s.settings, ec);
                std::ofstream out(file, std::ios::app | std::ios::binary);
                if (out) {
                    out << text << "\n";
                    ctx.message("Added to the startup suite.");
                } else {
                    ctx.message("Could not write the startup suite.");
                }
            }
        }
        if (!s.queued.empty() && !ctx.host().queueScript(s.queued, 1))
            ctx.message("(command ...) needs a command line; nothing was run.");
        s.queued.clear();
    }

    std::shared_ptr<Session> session_;
};

class LispCommand final : public LispCommandBase {
public:
    using LispCommandBase::LispCommandBase;
    std::string_view name() const override { return "LISP"; }
    std::vector<std::string> defaultAliases() const override { return {"VLISP"}; }
    std::string_view description() const override {
        return "Evaluate an AutoLISP expression (or type it in parentheses on the command line)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override { return drive(ctx, Mode::Lisp, {}); }
    Session& session() { return *session_; }
};

class AppLoadCommand final : public LispCommandBase {
public:
    using LispCommandBase::LispCommandBase;
    std::string_view name() const override { return "APPLOAD"; }
    std::vector<std::string> defaultAliases() const override { return {"AP"}; }
    std::string_view description() const override { return "Load an AutoLISP (.lsp) file"; }
    cmd::Task run(cmd::CommandContext& ctx) override { return drive(ctx, Mode::AppLoad, {}); }
};

// One per (defun c:NAME ...): typing NAME runs the function.
class UserCommand final : public LispCommandBase {
public:
    UserCommand(std::shared_ptr<Session> s, std::string name)
        : LispCommandBase(std::move(s)), name_(std::move(name)) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return "Defined by an AutoLISP program"; }
    cmd::Task run(cmd::CommandContext& ctx) override { return drive(ctx, Mode::User, name_); }

private:
    std::string name_;
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    auto session = std::make_shared<Session>();
    session->makeCommand = [weak = std::weak_ptr<Session>(session)](const std::string& name) {
        auto s = weak.lock();
        if (!s || !s->registry)
            return;
        auto r = s->registry->add(std::make_unique<UserCommand>(s, name));
        if (!r)
            s->emit("; cannot make \"" + name + "\" a command: " + r.error().message + "\n");
    };
    (void)registry.add(std::make_unique<LispCommand>(session));
    (void)registry.add(std::make_unique<AppLoadCommand>(session));
}

void loadStartup(cmd::Registry& registry, const std::filesystem::path& settingsFolder,
                 const std::function<void(const std::string&)>& print) {
    auto* lispCommand = dynamic_cast<LispCommand*>(registry.find("LISP"));
    if (!lispCommand)
        return;
    Session& s = lispCommand->session();
    s.registry = &registry;
    s.settings = settingsFolder;
    s.print = print;
    std::vector<std::string> files = {"acad.lsp"};
    {
        std::ifstream list(settingsFolder / "startup-suite.txt", std::ios::binary);
        for (std::string line; std::getline(list, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty())
                files.push_back(line);
        }
    }
    for (const auto& f : files) {
        if (s.resolve(f).empty())
            continue;
        try {
            (void)s.loadFile(f);
        } catch (const std::exception& e) {
            s.emit("; startup file " + f + ": " + e.what() + "\n");
        }
    }
    s.queued.clear();
    s.flushOut();
}

}  // namespace tbcad::lispcmd
