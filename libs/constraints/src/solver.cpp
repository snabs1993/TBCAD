#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "tbcad/constraints/constraints.h"

namespace tbcad::constraints {

namespace {

enum class Type { Line, Circle, Arc, Point };

// Variables per entity: Line {ax, ay, bx, by}, Circle {cx, cy, r}, Arc {cx, cy, r, a0, a1},
// Point {x, y}.
struct Slot {
    EntityId id{};
    Type type = Type::Point;
    std::size_t offset = 0;
    std::size_t count = 0;
};

struct Model {
    const Document* doc = nullptr;
    std::vector<Slot> slots{};
    std::unordered_map<std::uint64_t, std::size_t> slotOf{};  // entity id -> index into slots
    std::vector<double> x{};                                  // all variables
    std::vector<Constraint> constraints{};
    std::vector<double> targets{};  // per constraint: Tangent circle/circle distance, Fix values

    // Fix constraints keep the geometry the solve started with.
    std::vector<std::vector<double>> fixValues{};

    const Slot& slot(EntityId id) const { return slots[slotOf.at(id.value)]; }

    Vec2 point(const std::vector<double>& v, const Ref& r) const {
        const Slot& s = slot(r.entity);
        const double* p = v.data() + s.offset;
        switch (s.type) {
            case Type::Point:
                return {p[0], p[1]};
            case Type::Line:
                return r.part == Part::End ? Vec2{p[2], p[3]} : Vec2{p[0], p[1]};
            case Type::Circle:
                return {p[0], p[1]};
            case Type::Arc: {
                if (r.part == Part::Start)
                    return {p[0] + p[2] * std::cos(p[3]), p[1] + p[2] * std::sin(p[3])};
                if (r.part == Part::End)
                    return {p[0] + p[2] * std::cos(p[4]), p[1] + p[2] * std::sin(p[4])};
                return {p[0], p[1]};
            }
        }
        return {};
    }
    Vec2 lineA(const std::vector<double>& v, EntityId id) const {
        const double* p = v.data() + slot(id).offset;
        return {p[0], p[1]};
    }
    Vec2 lineB(const std::vector<double>& v, EntityId id) const {
        const double* p = v.data() + slot(id).offset;
        return {p[2], p[3]};
    }
    Vec2 center(const std::vector<double>& v, EntityId id) const {
        const double* p = v.data() + slot(id).offset;
        return {p[0], p[1]};
    }
    double radius(const std::vector<double>& v, EntityId id) const {
        return v[slot(id).offset + 2];
    }
    bool isLine(EntityId id) const { return slot(id).type == Type::Line; }
};

double wrapPi(double a) {
    a = std::fmod(a, kTwoPi);
    if (a > kPi)
        a -= kTwoPi;
    else if (a <= -kPi)
        a += kTwoPi;
    return a;
}

bool addSlot(Model& m, EntityId id) {
    if (m.slotOf.count(id.value))
        return true;
    const Entity* e = m.doc->find(id);
    if (!e)
        return false;
    Slot s;
    s.id = id;
    s.offset = m.x.size();
    if (const auto* l = entityAs<LineData>(*e)) {
        s.type = Type::Line;
        m.x.insert(m.x.end(), {l->a.x, l->a.y, l->b.x, l->b.y});
    } else if (const auto* c = entityAs<CircleData>(*e)) {
        s.type = Type::Circle;
        m.x.insert(m.x.end(), {c->center.x, c->center.y, c->radius});
    } else if (const auto* a = entityAs<ArcData>(*e)) {
        s.type = Type::Arc;
        double end = a->endAngle;
        if (end < a->startAngle)
            end += kTwoPi;  // keep the sweep positive and continuous while solving
        m.x.insert(m.x.end(), {a->center.x, a->center.y, a->radius, a->startAngle, end});
    } else if (const auto* p = entityAs<PointData>(*e)) {
        s.type = Type::Point;
        m.x.insert(m.x.end(), {p->p.x, p->p.y});
    } else {
        return false;
    }
    s.count = m.x.size() - s.offset;
    m.slotOf[id.value] = m.slots.size();
    m.slots.push_back(s);
    return true;
}

Result<Model> buildModel(const Document& doc, const std::vector<Constraint>& cs) {
    Model m;
    m.doc = &doc;
    for (const Constraint& c : cs) {
        if (!addSlot(m, c.a.entity) || (c.b.entity && !addSlot(m, c.b.entity)))
            return Error{ErrorCode::NotFound, "constraint #" + std::to_string(c.id) +
                                                  " refers to an object that is gone"};
        if (auto ok = validate(doc, c); !ok)
            return ok.error();
        m.constraints.push_back(c);
    }
    // Per-constraint start values.
    m.targets.assign(cs.size(), 0);
    m.fixValues.assign(cs.size(), {});
    for (std::size_t i = 0; i < cs.size(); ++i) {
        const Constraint& c = cs[i];
        if (c.kind == Kind::Tangent && !m.isLine(c.a.entity) && !m.isLine(c.b.entity)) {
            const double d = length(m.center(m.x, c.a.entity) - m.center(m.x, c.b.entity));
            const double r1 = m.radius(m.x, c.a.entity), r2 = m.radius(m.x, c.b.entity);
            const double ext = r1 + r2, in = std::abs(r1 - r2);
            m.targets[i] = std::abs(d - ext) <= std::abs(d - in) ? ext : in;
        }
        if (c.kind == Kind::Fix) {
            if (c.a.part == Part::Whole) {
                const Slot& s = m.slot(c.a.entity);
                m.fixValues[i].assign(m.x.begin() + s.offset, m.x.begin() + s.offset + s.count);
            } else {
                const Vec2 p = m.point(m.x, c.a);
                m.fixValues[i] = {p.x, p.y};
            }
        }
    }
    return m;
}

constexpr double kTiny = 1e-12;

// Appends the residuals of constraint i to `out`.
void residuals(const Model& m, std::size_t i, const std::vector<double>& v,
               std::vector<double>& out) {
    const Constraint& c = m.constraints[i];
    switch (c.kind) {
        case Kind::Coincident: {
            const Vec2 d = m.point(v, c.a) - m.point(v, c.b);
            out.push_back(d.x);
            out.push_back(d.y);
            break;
        }
        case Kind::Horizontal:
        case Kind::Vertical: {
            Vec2 p, q;
            if (!c.b.entity) {
                p = m.lineA(v, c.a.entity);
                q = m.lineB(v, c.a.entity);
            } else {
                p = m.point(v, c.a);
                q = m.point(v, c.b);
            }
            out.push_back(c.kind == Kind::Horizontal ? p.y - q.y : p.x - q.x);
            break;
        }
        case Kind::Parallel:
        case Kind::Perpendicular: {
            const Vec2 da = m.lineB(v, c.a.entity) - m.lineA(v, c.a.entity);
            const Vec2 db = m.lineB(v, c.b.entity) - m.lineA(v, c.b.entity);
            const double n = std::max(length(da) * length(db), kTiny);
            out.push_back((c.kind == Kind::Parallel ? cross(da, db) : dot(da, db)) / n);
            break;
        }
        case Kind::Equal: {
            if (m.isLine(c.a.entity)) {
                out.push_back(length(m.lineB(v, c.a.entity) - m.lineA(v, c.a.entity)) -
                              length(m.lineB(v, c.b.entity) - m.lineA(v, c.b.entity)));
            } else {
                out.push_back(m.radius(v, c.a.entity) - m.radius(v, c.b.entity));
            }
            break;
        }
        case Kind::Tangent: {
            const bool aLine = m.isLine(c.a.entity), bLine = m.isLine(c.b.entity);
            if (aLine || bLine) {
                const EntityId le = aLine ? c.a.entity : c.b.entity;
                const EntityId ce = aLine ? c.b.entity : c.a.entity;
                const Vec2 a = m.lineA(v, le), b = m.lineB(v, le);
                const Vec2 dir = b - a;
                const double n = std::max(length(dir), kTiny);
                out.push_back(std::abs(cross(dir, m.center(v, ce) - a)) / n - m.radius(v, ce));
            } else {
                out.push_back(length(m.center(v, c.a.entity) - m.center(v, c.b.entity)) -
                              m.targets[i]);
            }
            break;
        }
        case Kind::Fix: {
            const std::vector<double>& f = m.fixValues[i];
            if (c.a.part == Part::Whole) {
                const Slot& s = m.slot(c.a.entity);
                for (std::size_t k = 0; k < s.count; ++k)
                    out.push_back(v[s.offset + k] - f[k]);
            } else {
                const Vec2 p = m.point(v, c.a);
                out.push_back(p.x - f[0]);
                out.push_back(p.y - f[1]);
            }
            break;
        }
        case Kind::Distance:
            out.push_back(length(m.point(v, c.a) - m.point(v, c.b)) - c.value);
            break;
        case Kind::Radius:
            out.push_back(m.radius(v, c.a.entity) - c.value);
            break;
        case Kind::Angle: {
            const Vec2 da = m.lineB(v, c.a.entity) - m.lineA(v, c.a.entity);
            const Vec2 db = m.lineB(v, c.b.entity) - m.lineA(v, c.b.entity);
            out.push_back(wrapPi(std::atan2(cross(da, db), dot(da, db)) - c.value));
            break;
        }
    }
}

std::size_t residualCount(const Model& m, std::size_t i) {
    std::vector<double> tmp;
    residuals(m, i, m.x, tmp);
    return tmp.size();
}

void allResiduals(const Model& m, const std::vector<double>& v, std::vector<double>& out) {
    out.clear();
    for (std::size_t i = 0; i < m.constraints.size(); ++i)
        residuals(m, i, v, out);
}

double maxAbs(const std::vector<double>& r) {
    double w = 0;
    for (double x : r)
        w = std::max(w, std::abs(x));
    return w;
}

double cost(const std::vector<double>& r) {
    double s = 0;
    for (double x : r)
        s += x * x;
    return s;
}

// Solves A d = b for a symmetric positive definite A (n x n, row major) by Cholesky; false if
// A is not positive definite.
bool cholesky(std::vector<double> A, std::vector<double> b, std::size_t n, std::vector<double>& d) {
    for (std::size_t j = 0; j < n; ++j) {
        double s = A[j * n + j];
        for (std::size_t k = 0; k < j; ++k)
            s -= A[j * n + k] * A[j * n + k];
        if (s <= 1e-300)
            return false;
        const double ljj = std::sqrt(s);
        A[j * n + j] = ljj;
        for (std::size_t i = j + 1; i < n; ++i) {
            double t = A[i * n + j];
            for (std::size_t k = 0; k < j; ++k)
                t -= A[i * n + k] * A[j * n + k];
            A[i * n + j] = t / ljj;
        }
    }
    d.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        double t = b[i];
        for (std::size_t k = 0; k < i; ++k)
            t -= A[i * n + k] * d[k];
        d[i] = t / A[i * n + i];
    }
    for (std::size_t ii = n; ii-- > 0;) {
        double t = d[ii];
        for (std::size_t k = ii + 1; k < n; ++k)
            t -= A[k * n + ii] * d[k];
        d[ii] = t / A[ii * n + ii];
    }
    return true;
}

// Rank of a dense m x n matrix (row major) by Gaussian elimination with full pivoting.
int rankOf(std::vector<double> J, std::size_t rows, std::size_t cols) {
    double big = 0;
    for (double x : J)
        big = std::max(big, std::abs(x));
    if (big == 0)
        return 0;
    const double eps = big * 1e-7;
    int rank = 0;
    std::vector<bool> rowUsed(rows, false), colUsed(cols, false);
    for (;;) {
        double best = eps;
        std::size_t br = rows, bc = cols;
        for (std::size_t r = 0; r < rows; ++r) {
            if (rowUsed[r])
                continue;
            for (std::size_t c = 0; c < cols; ++c) {
                if (!colUsed[c] && std::abs(J[r * cols + c]) > best) {
                    best = std::abs(J[r * cols + c]);
                    br = r;
                    bc = c;
                }
            }
        }
        if (br == rows)
            break;
        rowUsed[br] = colUsed[bc] = true;
        ++rank;
        for (std::size_t r = 0; r < rows; ++r) {
            if (rowUsed[r])
                continue;
            const double f = J[r * cols + bc] / J[br * cols + bc];
            if (f == 0)
                continue;
            for (std::size_t c = 0; c < cols; ++c)
                J[r * cols + c] -= f * J[br * cols + c];
        }
    }
    return rank;
}

struct Outcome {
    bool converged = false;
    int iterations = 0;
    double residual = 0;
    int dof = 0;
    std::uint32_t worst = 0;
    std::vector<double> x{};
};

// Levenberg-Marquardt over the variables of the entities that are not pinned.
Outcome run(const Model& m, const SolveOptions& opt, const std::set<EntityId>& pinned) {
    std::vector<std::size_t> free;
    for (const Slot& s : m.slots) {
        if (pinned.count(s.id))
            continue;
        for (std::size_t k = 0; k < s.count; ++k)
            free.push_back(s.offset + k);
    }
    Outcome out;
    out.x = m.x;
    std::vector<double> r;
    allResiduals(m, out.x, r);
    const std::size_t rows = r.size(), n = free.size();
    out.residual = maxAbs(r);

    auto jacobian = [&](const std::vector<double>& v, std::vector<double>& J) {
        J.assign(rows * n, 0);
        std::vector<double> w = v, rp, rm;
        for (std::size_t j = 0; j < n; ++j) {
            const double h = 1e-6 * std::max(1.0, std::abs(v[free[j]]));
            w[free[j]] = v[free[j]] + h;
            allResiduals(m, w, rp);
            w[free[j]] = v[free[j]] - h;
            allResiduals(m, w, rm);
            w[free[j]] = v[free[j]];
            for (std::size_t i = 0; i < rows; ++i)
                J[i * n + j] = (rp[i] - rm[i]) / (2 * h);
        }
    };

    double lambda = 1e-3;
    std::vector<double> J, trial, rt;
    for (; out.iterations < opt.maxIterations && n > 0 && out.residual > opt.tolerance;
         ++out.iterations) {
        jacobian(out.x, J);
        std::vector<double> A(n * n, 0), g(n, 0);
        for (std::size_t i = 0; i < rows; ++i) {
            for (std::size_t a = 0; a < n; ++a) {
                const double ja = J[i * n + a];
                if (ja == 0)
                    continue;
                g[a] -= ja * r[i];
                for (std::size_t b = 0; b <= a; ++b)
                    A[a * n + b] += ja * J[i * n + b];
            }
        }
        for (std::size_t a = 0; a < n; ++a)
            for (std::size_t b = a + 1; b < n; ++b)
                A[a * n + b] = A[b * n + a];
        bool improved = false;
        for (int attempt = 0; attempt < 12 && !improved; ++attempt) {
            std::vector<double> Ad = A, d;
            for (std::size_t a = 0; a < n; ++a)
                Ad[a * n + a] += lambda * std::max(A[a * n + a], 1e-9);
            if (cholesky(Ad, g, n, d)) {
                trial = out.x;
                for (std::size_t a = 0; a < n; ++a)
                    trial[free[a]] += d[a];
                allResiduals(m, trial, rt);
                if (cost(rt) < cost(r)) {
                    out.x = trial;
                    r = rt;
                    lambda = std::max(lambda * 0.3, 1e-12);
                    improved = true;
                    break;
                }
            }
            lambda *= 10;
        }
        out.residual = maxAbs(r);
        if (!improved)
            break;
    }
    out.converged = out.residual <= std::max(opt.tolerance, 1e-8);

    // Degrees of freedom and the worst constraint.
    if (n > 0) {
        jacobian(out.x, J);
        out.dof = static_cast<int>(n) - rankOf(J, rows, n);
    }
    std::size_t at = 0;
    double worst = -1;
    for (std::size_t i = 0; i < m.constraints.size(); ++i) {
        const std::size_t cnt = residualCount(m, i);
        for (std::size_t k = 0; k < cnt; ++k) {
            if (std::abs(r[at + k]) > worst) {
                worst = std::abs(r[at + k]);
                out.worst = m.constraints[i].id;
            }
        }
        at += cnt;
    }
    return out;
}

}  // namespace

Result<double> measure(const Document& doc, const Constraint& c) {
    Constraint probe = c;  // the value is what we are asked for, so any valid one will do
    probe.value = c.kind == Kind::Radius ? 1 : 0;
    auto model = buildModel(doc, {probe});
    if (!model)
        return model.error();
    const Model& m = model.value();
    switch (c.kind) {
        case Kind::Distance:
            return length(m.point(m.x, c.a) - m.point(m.x, c.b));
        case Kind::Radius:
            return m.radius(m.x, c.a.entity);
        case Kind::Angle: {
            const Vec2 da = m.lineB(m.x, c.a.entity) - m.lineA(m.x, c.a.entity);
            const Vec2 db = m.lineB(m.x, c.b.entity) - m.lineA(m.x, c.b.entity);
            return std::atan2(cross(da, db), dot(da, db));
        }
        default:
            return Error{ErrorCode::InvalidArgument, "only dimensional constraints have a value"};
    }
}

Result<SolveResult> solve(Transaction& tx, const ConstraintSet& set, const SolveOptions& opt) {
    SolveResult res;
    if (set.empty()) {
        res.converged = true;
        return res;
    }
    const Document& doc = tx.document();
    auto model = buildModel(doc, set.all());
    if (!model)
        return model.error();
    const Model& m = model.value();

    Outcome out = run(m, opt, opt.pinned);
    res.converged = out.converged;
    res.iterations = out.iterations;
    res.residual = out.residual;
    res.degreesOfFreedom = out.dof;
    res.worst = out.converged ? 0 : out.worst;
    if (!out.converged)
        return res;

    for (const Slot& s : m.slots) {
        if (opt.pinned.count(s.id))
            continue;
        const double* p = out.x.data() + s.offset;
        const double* q = m.x.data() + s.offset;
        bool same = true;
        for (std::size_t k = 0; k < s.count; ++k)
            same = same && std::abs(p[k] - q[k]) <= 1e-12;
        if (same)
            continue;
        auto edited = tx.modify(s.id, [&](Entity& e) {
            if (auto* l = entityAs<LineData>(e)) {
                l->a = {p[0], p[1]};
                l->b = {p[2], p[3]};
            } else if (auto* c = entityAs<CircleData>(e)) {
                c->center = {p[0], p[1]};
                c->radius = p[2];
            } else if (auto* a = entityAs<ArcData>(e)) {
                a->center = {p[0], p[1]};
                a->radius = p[2];
                a->startAngle = normalizeAngle(p[3]);
                a->endAngle = normalizeAngle(p[4]);
            } else if (auto* pt = entityAs<PointData>(e)) {
                pt->p = {p[0], p[1]};
            }
        });
        if (!edited)
            return edited.error();
        ++res.changedEntities;
    }
    return res;
}

}  // namespace tbcad::constraints
