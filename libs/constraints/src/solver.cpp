#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "tbcad/constraints/constraints.h"

namespace tbcad::constraints {

namespace {

enum class Type { Line, Circle, Arc, Point, Polyline, Ellipse, Insert };

// Variables per entity: Line {ax, ay, bx, by}, Circle {cx, cy, r}, Arc {cx, cy, r, a0, a1},
// Point {x, y}, Polyline {x0, y0, x1, y1, ...}, Ellipse {cx, cy, mx, my} (major axis vector),
// Insert {x, y, rotation}.
struct Slot {
    EntityId id{};
    Type type = Type::Point;
    std::size_t offset = 0;
    std::size_t count = 0;
    bool closed = false;          // polylines
    std::vector<Vec2> anchors{};  // inserts: connection points, offsets in the insert's axes
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

    static Vec2 insertPoint(const Slot& s, const double* p, std::uint32_t index) {
        Vec2 o{0, 0};
        if (index > 0 && index < s.anchors.size())
            o = s.anchors[index];
        const double c = std::cos(p[2]), sn = std::sin(p[2]);
        return {p[0] + c * o.x - sn * o.y, p[1] + sn * o.x + c * o.y};
    }

    Vec2 point(const std::vector<double>& v, const Ref& r) const {
        const Slot& s = slot(r.entity);
        const double* p = v.data() + s.offset;
        switch (s.type) {
            case Type::Point:
                return {p[0], p[1]};
            case Type::Line:
                return r.part == Part::End ? Vec2{p[2], p[3]} : Vec2{p[0], p[1]};
            case Type::Circle:
            case Type::Ellipse:
                return {p[0], p[1]};
            case Type::Insert:
                return insertPoint(s, p, r.index);
            case Type::Polyline: {
                const std::size_t i = std::min<std::size_t>(r.index, s.count / 2 - 1);
                return {p[2 * i], p[2 * i + 1]};
            }
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
    // The two ends of a straight segment (line, polyline segment) or the x axis of an insert as a
    // unit vector from its insertion point.
    std::pair<Vec2, Vec2> seg(const std::vector<double>& v, const Ref& r) const {
        const Slot& s = slot(r.entity);
        const double* p = v.data() + s.offset;
        switch (s.type) {
            case Type::Polyline: {
                const std::size_t n = s.count / 2;
                const std::size_t i = std::min<std::size_t>(r.index, n - 1);
                const std::size_t j = (i + 1) % n;
                return {{p[2 * i], p[2 * i + 1]}, {p[2 * j], p[2 * j + 1]}};
            }
            case Type::Insert: {
                const Vec2 o = insertPoint(s, p, r.index);
                return {o, {o.x + std::cos(p[2]), o.y + std::sin(p[2])}};
            }
            default:
                return {{p[0], p[1]}, {p[2], p[3]}};
        }
    }
    Vec2 lineA(const std::vector<double>& v, const Ref& r) const { return seg(v, r).first; }
    Vec2 lineB(const std::vector<double>& v, const Ref& r) const { return seg(v, r).second; }
    Vec2 center(const std::vector<double>& v, EntityId id) const {
        const double* p = v.data() + slot(id).offset;
        return {p[0], p[1]};
    }
    double radius(const std::vector<double>& v, EntityId id) const {
        return v[slot(id).offset + 2];
    }
    bool isAxis(EntityId id) const { return isLine(id) || slot(id).type == Type::Insert; }
    bool isLine(EntityId id) const {
        return slot(id).type == Type::Line || slot(id).type == Type::Polyline;
    }
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
    } else if (const auto* pl = entityAs<PolylineData>(*e)) {
        if (pl->v.empty())
            return false;
        s.type = Type::Polyline;
        s.closed = pl->closed;
        for (const PolyVertex& pv : pl->v)
            m.x.insert(m.x.end(), {pv.p.x, pv.p.y});
    } else if (const auto* el = entityAs<EllipseData>(*e)) {
        s.type = Type::Ellipse;
        m.x.insert(m.x.end(), {el->center.x, el->center.y, el->majorAxis.x, el->majorAxis.y});
    } else if (const auto* in = entityAs<InsertData>(*e)) {
        s.type = Type::Insert;
        s.anchors = insertAnchors(*m.doc, *in);
        m.x.insert(m.x.end(), {in->position.x, in->position.y, in->rotation});
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
                p = m.lineA(v, c.a);
                q = m.lineB(v, c.a);
            } else {
                p = m.point(v, c.a);
                q = m.point(v, c.b);
            }
            out.push_back(c.kind == Kind::Horizontal ? p.y - q.y : p.x - q.x);
            break;
        }
        case Kind::Parallel:
        case Kind::Perpendicular: {
            const Vec2 da = m.lineB(v, c.a) - m.lineA(v, c.a);
            const Vec2 db = m.lineB(v, c.b) - m.lineA(v, c.b);
            const double n = std::max(length(da) * length(db), kTiny);
            out.push_back((c.kind == Kind::Parallel ? cross(da, db) : dot(da, db)) / n);
            break;
        }
        case Kind::Equal: {
            if (m.isLine(c.a.entity)) {
                out.push_back(length(m.lineB(v, c.a) - m.lineA(v, c.a)) -
                              length(m.lineB(v, c.b) - m.lineA(v, c.b)));
            } else {
                out.push_back(m.radius(v, c.a.entity) - m.radius(v, c.b.entity));
            }
            break;
        }
        case Kind::Tangent: {
            const bool aLine = m.isLine(c.a.entity), bLine = m.isLine(c.b.entity);
            if (aLine || bLine) {
                const Ref& le = aLine ? c.a : c.b;
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
            const Vec2 da = m.lineB(v, c.a) - m.lineA(v, c.a);
            const Vec2 db = m.lineB(v, c.b) - m.lineA(v, c.b);
            out.push_back(wrapPi(std::atan2(cross(da, db), dot(da, db)) - c.value));
            break;
        }
        case Kind::DistanceRange: {
            const double d = length(m.point(v, c.a) - m.point(v, c.b));
            out.push_back(d < c.value ? d - c.value : d > c.value2 ? d - c.value2 : 0.0);
            break;
        }
        case Kind::AngleRange: {
            const Vec2 da = m.lineB(v, c.a) - m.lineA(v, c.a);
            const Vec2 db = m.lineB(v, c.b) - m.lineA(v, c.b);
            const double mid = (c.value + c.value2) / 2, half = (c.value2 - c.value) / 2;
            const double off = wrapPi(std::atan2(cross(da, db), dot(da, db)) - mid);
            out.push_back(std::abs(off) > half ? off - std::copysign(half, off) : 0.0);
            break;
        }
        case Kind::Concentric: {
            const Vec2 d =
                m.point(v, {c.a.entity, Part::Center}) - m.point(v, {c.b.entity, Part::Center});
            out.push_back(d.x);
            out.push_back(d.y);
            break;
        }
        case Kind::PointOnObject: {
            const Vec2 p = m.point(v, c.a);
            if (m.isAxis(c.b.entity)) {
                const Vec2 a = m.lineA(v, c.b), b = m.lineB(v, c.b);
                out.push_back(cross(b - a, p - a) / std::max(length(b - a), kTiny));
            } else {
                out.push_back(length(p - m.center(v, c.b.entity)) - m.radius(v, c.b.entity));
            }
            break;
        }
        case Kind::Midpoint: {
            const Vec2 mid = (m.lineA(v, c.b) + m.lineB(v, c.b)) * 0.5;
            const Vec2 d = m.point(v, c.a) - mid;
            out.push_back(d.x);
            out.push_back(d.y);
            break;
        }
        case Kind::Collinear: {
            const Vec2 a = m.lineA(v, c.a), da = m.lineB(v, c.a) - a;
            const double n = std::max(length(da), kTiny);
            out.push_back(cross(da, m.lineA(v, c.b) - a) / n);
            out.push_back(cross(da, m.lineB(v, c.b) - a) / n);
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

// ---- sparse Jacobian and banded normal equations ----------------------------------------------

using SparseRow = std::vector<std::pair<std::uint32_t, double>>;

struct SparseRows {
    std::size_t cols = 0;
    std::vector<SparseRow> rows{};  // one per residual, columns ascending
};

// Every variable the residuals of constraint i can depend on: all variables of the objects it
// names.
std::vector<std::size_t> constraintVars(const Model& m, std::size_t i) {
    std::vector<std::size_t> vars;
    const Constraint& c = m.constraints[i];
    for (EntityId id : {c.a.entity, c.b.entity}) {
        if (!id)
            continue;
        const Slot& s = m.slot(id);
        for (std::size_t k = 0; k < s.count; ++k)
            vars.push_back(s.offset + k);
    }
    std::sort(vars.begin(), vars.end());
    vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
    return vars;
}

// Central differences of every residual against the variables that have a column (colOf >= 0).
// A residual only moves with the variables of the objects its constraint names, so only those are
// perturbed and only that constraint is re-evaluated.
SparseRows jacobian(const Model& m, const std::vector<double>& v, const std::vector<long>& colOf,
                    std::size_t cols, const std::vector<std::size_t>& first,
                    const std::vector<std::vector<std::size_t>>& vars) {
    SparseRows J;
    J.cols = cols;
    J.rows.assign(first.back(), {});
    std::vector<double> w = v, rp, rm;
    for (std::size_t i = 0; i < m.constraints.size(); ++i) {
        const std::size_t cnt = first[i + 1] - first[i];
        for (std::size_t var : vars[i]) {
            const long col = colOf[var];
            if (col < 0)
                continue;
            const double h = 1e-6 * std::max(1.0, std::abs(v[var]));
            w[var] = v[var] + h;
            rp.clear();
            residuals(m, i, w, rp);
            w[var] = v[var] - h;
            rm.clear();
            residuals(m, i, w, rm);
            w[var] = v[var];
            for (std::size_t r = 0; r < cnt; ++r) {
                const double d = (rp[r] - rm[r]) / (2 * h);
                if (d != 0)
                    J.rows[first[i] + r].push_back({static_cast<std::uint32_t>(col), d});
            }
        }
    }
    return J;
}

std::vector<double> toDense(const SparseRows& J) {
    std::vector<double> D(J.rows.size() * J.cols, 0.0);
    for (std::size_t i = 0; i < J.rows.size(); ++i)
        for (const auto& [c, val] : J.rows[i])
            D[i * J.cols + c] = val;
    return D;
}

// Variable order that keeps the normal equations narrow (reverse Cuthill-McKee over the columns
// that share a constraint), and the half bandwidth that order gives.
struct BandOrder {
    std::vector<std::size_t> perm{};  // position -> column
    std::vector<std::size_t> pos{};   // column -> position
    std::size_t band = 0;
};

BandOrder bandOrder(std::size_t n, const std::vector<std::vector<std::size_t>>& groups) {
    std::vector<std::vector<std::uint32_t>> adj(n);
    for (const auto& g : groups)
        for (std::size_t a : g)
            for (std::size_t b : g)
                if (a != b)
                    adj[a].push_back(static_cast<std::uint32_t>(b));
    for (auto& l : adj) {
        std::sort(l.begin(), l.end());
        l.erase(std::unique(l.begin(), l.end()), l.end());
    }
    BandOrder o;
    o.perm.reserve(n);
    std::vector<bool> seen(n, false);
    std::vector<std::size_t> byDegree(n);
    for (std::size_t i = 0; i < n; ++i)
        byDegree[i] = i;
    std::stable_sort(byDegree.begin(), byDegree.end(),
                     [&](std::size_t x, std::size_t y) { return adj[x].size() < adj[y].size(); });
    for (std::size_t root : byDegree) {
        if (seen[root])
            continue;
        std::size_t head = o.perm.size();
        o.perm.push_back(root);
        seen[root] = true;
        while (head < o.perm.size()) {
            const std::size_t u = o.perm[head++];
            std::vector<std::size_t> next;
            for (std::uint32_t w : adj[u])
                if (!seen[w]) {
                    seen[w] = true;
                    next.push_back(w);
                }
            std::stable_sort(next.begin(), next.end(), [&](std::size_t x, std::size_t y) {
                return adj[x].size() < adj[y].size();
            });
            o.perm.insert(o.perm.end(), next.begin(), next.end());
        }
    }
    std::reverse(o.perm.begin(), o.perm.end());
    o.pos.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        o.pos[o.perm[i]] = i;
    for (const auto& g : groups) {
        if (g.empty())
            continue;
        std::size_t lo = n, hi = 0;
        for (std::size_t c : g) {
            lo = std::min(lo, o.pos[c]);
            hi = std::max(hi, o.pos[c]);
        }
        o.band = std::max(o.band, hi - lo);
    }
    return o;
}

// Cholesky factor of a symmetric positive definite band matrix kept as rows of the lower triangle:
// entry (i, j), i - band <= j <= i, is at i * (band + 1) + (j - i + band). Overwrites A with L;
// false if A is not positive definite.
bool choleskyBand(std::vector<double>& A, std::size_t n, std::size_t band) {
    const std::size_t w = band + 1;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j0 = i > band ? i - band : 0;
        for (std::size_t j = j0; j <= i; ++j) {
            double s = A[i * w + (j - i + band)];
            const std::size_t k0 = std::max(j0, j > band ? j - band : 0);
            for (std::size_t k = k0; k < j; ++k)
                s -= A[i * w + (k - i + band)] * A[j * w + (k - j + band)];
            if (j == i) {
                if (s <= 1e-300)
                    return false;
                A[i * w + band] = std::sqrt(s);
            } else {
                A[i * w + (j - i + band)] = s / A[j * w + band];
            }
        }
    }
    return true;
}

// Solves L L^T x = b with the factor from choleskyBand.
std::vector<double> solveBand(const std::vector<double>& L, std::size_t n, std::size_t band,
                              const std::vector<double>& b) {
    const std::size_t w = band + 1;
    std::vector<double> x(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double t = b[i];
        for (std::size_t k = i > band ? i - band : 0; k < i; ++k)
            t -= L[i * w + (k - i + band)] * x[k];
        x[i] = t / L[i * w + band];
    }
    for (std::size_t i = n; i-- > 0;) {
        double t = x[i];
        for (std::size_t k = i + 1; k < n && k <= i + band; ++k)
            t -= L[k * w + (i - k + band)] * x[k];
        x[i] = t / L[i * w + band];
    }
    return x;
}

// Gaussian elimination with full pivoting on a dense rows x cols matrix (row major). A pivot must
// exceed 1e-7 of the largest entry. Row operations only touch rows that have an entry in the pivot
// column, and the best entry of each row is kept, so a sparse matrix costs little more than its
// non-zeros. With `trackRows` the same operations are applied to an identity matrix (L), so the
// rows left without a pivot are combinations of the original rows that cancel.
struct Elimination {
    int rank = 0;
    std::vector<std::size_t> pivRow{}, pivCol{};  // in pivot order
    std::vector<double> M{};                      // reduced matrix; pivot rows are upper triangular
    std::vector<double> L{};                      // rows x rows, empty unless tracked
};

Elimination eliminate(std::vector<double> J, std::size_t rows, std::size_t cols, bool trackRows,
                      double scale = 0, double relTol = 1e-7) {
    Elimination e;
    if (trackRows) {
        e.L.assign(rows * rows, 0.0);
        for (std::size_t i = 0; i < rows; ++i)
            e.L[i * rows + i] = 1.0;
    }
    double big = 0;
    for (double x : J)
        big = std::max(big, std::abs(x));
    if (scale > 0)
        big = scale;  // the caller's yardstick for what counts as zero
    if (big == 0) {
        e.M = std::move(J);
        return e;
    }
    const double eps = big * relTol;
    std::vector<bool> rowUsed(rows, false), colUsed(cols, false);
    std::vector<double> rowBest(rows, 0.0);
    std::vector<std::size_t> rowCol(rows, 0);
    auto rescan = [&](std::size_t r) {
        double best = eps;
        rowBest[r] = 0;
        for (std::size_t c = 0; c < cols; ++c) {
            if (!colUsed[c] && std::abs(J[r * cols + c]) > best) {
                best = std::abs(J[r * cols + c]);
                rowBest[r] = best;
                rowCol[r] = c;
            }
        }
    };
    for (std::size_t r = 0; r < rows; ++r)
        rescan(r);
    for (;;) {
        double best = eps;
        std::size_t br = rows;
        for (std::size_t r = 0; r < rows; ++r) {
            if (!rowUsed[r] && rowBest[r] > best) {
                best = rowBest[r];
                br = r;
            }
        }
        if (br == rows)
            break;
        const std::size_t bc = rowCol[br];
        rowUsed[br] = colUsed[bc] = true;
        ++e.rank;
        e.pivRow.push_back(br);
        e.pivCol.push_back(bc);
        const double pv = J[br * cols + bc];
        for (std::size_t r = 0; r < rows; ++r) {
            if (rowUsed[r])
                continue;
            const double f = J[r * cols + bc] / pv;
            if (f != 0) {
                for (std::size_t c = 0; c < cols; ++c)
                    J[r * cols + c] -= f * J[br * cols + c];
                if (trackRows)
                    for (std::size_t c = 0; c < rows; ++c)
                        e.L[r * rows + c] -= f * e.L[br * rows + c];
                rescan(r);
            } else if (rowBest[r] > 0 && rowCol[r] == bc) {
                rescan(r);
            }
        }
    }
    e.M = std::move(J);
    return e;
}

// Rank of a dense m x n matrix (row major).
int rankOf(std::vector<double> J, std::size_t rows, std::size_t cols, double scale = 0,
           double relTol = 1e-7) {
    return eliminate(std::move(J), rows, cols, false, scale, relTol).rank;
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
    std::vector<long> colOf(m.x.size(), -1);
    std::size_t n = 0;
    for (const Slot& s : m.slots) {
        if (pinned.count(s.id))
            continue;
        for (std::size_t k = 0; k < s.count; ++k)
            colOf[s.offset + k] = static_cast<long>(n++);
    }
    std::vector<std::size_t> free(n);
    for (std::size_t v = 0; v < colOf.size(); ++v)
        if (colOf[v] >= 0)
            free[static_cast<std::size_t>(colOf[v])] = v;
    Outcome out;
    out.x = m.x;
    std::vector<double> r;
    allResiduals(m, out.x, r);
    const std::size_t rows = r.size();
    out.residual = maxAbs(r);

    std::vector<std::size_t> first(m.constraints.size() + 1, 0);
    std::vector<std::vector<std::size_t>> vars(m.constraints.size());
    std::vector<std::vector<std::size_t>> groups;  // the free columns each constraint touches
    for (std::size_t i = 0; i < m.constraints.size(); ++i) {
        first[i + 1] = first[i] + residualCount(m, i);
        vars[i] = constraintVars(m, i);
        std::vector<std::size_t> g;
        for (std::size_t var : vars[i])
            if (colOf[var] >= 0)
                g.push_back(static_cast<std::size_t>(colOf[var]));
        groups.push_back(std::move(g));
    }
    const BandOrder order = bandOrder(n, groups);
    const std::size_t band = order.band, w = band + 1;

    double lambda = 1e-3;
    std::vector<double> trial, rt;
    for (; out.iterations < opt.maxIterations && n > 0 && out.residual > opt.tolerance;
         ++out.iterations) {
        const SparseRows J = jacobian(m, out.x, colOf, n, first, vars);
        std::vector<double> A(n * w, 0.0), g(n, 0.0);
        for (std::size_t i = 0; i < rows; ++i) {
            const SparseRow& row = J.rows[i];
            for (std::size_t p = 0; p < row.size(); ++p) {
                const auto [ca, ja] = row[p];
                g[order.pos[ca]] -= ja * r[i];
                for (std::size_t q = 0; q <= p; ++q) {
                    const auto [cb, jb] = row[q];
                    std::size_t hi = order.pos[ca], lo = order.pos[cb];
                    if (hi < lo)
                        std::swap(hi, lo);
                    A[hi * w + (lo - hi + band)] += ja * jb;
                }
            }
        }
        bool improved = false;
        for (int attempt = 0; attempt < 12 && !improved; ++attempt) {
            std::vector<double> Ad = A;
            for (std::size_t a = 0; a < n; ++a)
                Ad[a * w + band] += lambda * std::max(A[a * w + band], 1.0);
            if (choleskyBand(Ad, n, band)) {
                const std::vector<double> d = solveBand(Ad, n, band, g);
                trial = out.x;
                for (std::size_t a = 0; a < n; ++a)
                    trial[free[order.perm[a]]] += d[a];
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
        const SparseRows J = jacobian(m, out.x, colOf, n, first, vars);
        out.dof = static_cast<int>(n) - rankOf(toDense(J), rows, n);
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
            const Vec2 da = m.lineB(m.x, c.a) - m.lineA(m.x, c.a);
            const Vec2 db = m.lineB(m.x, c.b) - m.lineA(m.x, c.b);
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
            } else if (auto* pl = entityAs<PolylineData>(e)) {
                for (std::size_t k = 0; k < pl->v.size() && 2 * k + 1 < s.count; ++k)
                    pl->v[k].p = {p[2 * k], p[2 * k + 1]};
            } else if (auto* el = entityAs<EllipseData>(e)) {
                el->center = {p[0], p[1]};
                el->majorAxis = {p[2], p[3]};
            } else if (auto* in = entityAs<InsertData>(e)) {
                in->position = {p[0], p[1]};
                in->rotation = normalizeAngle(p[2]);
            }
        });
        if (!edited)
            return edited.error();
        ++res.changedEntities;
    }
    return res;
}

// Dependencies and null vectors are computed from a finite-difference Jacobian by elimination, so
// their entries carry noise; anything below this (against a largest entry of 1) is zero.
constexpr double kDerivedTol = 1e-5;

Result<Analysis> analyze(const Document& doc, const ConstraintSet& set) {
    Analysis a;
    if (set.empty()) {
        a.converged = true;
        return a;
    }
    auto model = buildModel(doc, set.all());
    if (!model)
        return model.error();
    const Model& m = model.value();
    const Outcome out = run(m, SolveOptions{}, {});
    a.converged = out.converged;
    a.worst = out.converged ? 0 : out.worst;
    const std::size_t n = m.x.size();

    // Jacobian at the solution (or where the solver stopped), all variables free.
    std::vector<double> r0;
    allResiduals(m, out.x, r0);
    const std::size_t rows = r0.size();
    std::vector<long> colOf(n);
    for (std::size_t j = 0; j < n; ++j)
        colOf[j] = static_cast<long>(j);
    std::vector<std::size_t> first(m.constraints.size() + 1, 0);
    std::vector<std::vector<std::size_t>> vars(m.constraints.size());
    for (std::size_t i = 0; i < m.constraints.size(); ++i) {
        first[i + 1] = first[i] + residualCount(m, i);
        vars[i] = constraintVars(m, i);
    }
    const std::vector<double> J = toDense(jacobian(m, out.x, colOf, n, first, vars));
    const Elimination el = eliminate(J, rows, n, true);
    const int rank = el.rank;
    a.rows = static_cast<int>(rows);
    a.rank = rank;
    a.degreesOfFreedom = static_cast<int>(n) - rank;

    // A constraint is redundant when taking its rows away keeps the rank. The rows that found no
    // pivot give the dependencies (combinations of rows that cancel); the constraint's rows can all
    // be dropped when those combinations, seen on its rows alone, still have full rank.
    std::vector<bool> hasPivot(rows, false);
    for (std::size_t r : el.pivRow)
        hasPivot[r] = true;
    std::vector<std::size_t> dependent;
    double depScale = 0;
    for (std::size_t r = 0; r < rows; ++r) {
        if (hasPivot[r])
            continue;
        dependent.push_back(r);
    }
    depScale = 1.0;  // every dependency below is scaled to a largest entry of 1
    std::vector<double> depNorm(dependent.size(), 1.0);
    for (std::size_t k = 0; k < dependent.size(); ++k) {
        double big = 0;
        for (std::size_t c = 0; c < rows; ++c)
            big = std::max(big, std::abs(el.L[dependent[k] * rows + c]));
        depNorm[k] = big > 0 ? 1.0 / big : 1.0;
    }
    for (std::size_t i = 0; i < m.constraints.size(); ++i) {
        ConstraintStatus st;
        st.id = m.constraints[i].id;
        if (out.converged) {
            const std::size_t cnt = first[i + 1] - first[i];
            std::vector<double> W(dependent.size() * cnt);
            for (std::size_t k = 0; k < dependent.size(); ++k)
                for (std::size_t c = 0; c < cnt; ++c)
                    W[k * cnt + c] = depNorm[k] * el.L[dependent[k] * rows + first[i] + c];
            st.redundant = static_cast<std::size_t>(rankOf(std::move(W), dependent.size(), cnt,
                                                           depScale, kDerivedTol)) == cnt;
        }
        a.constraints.push_back(st);
    }
    if (!out.converged) {
        for (ConstraintStatus& st : a.constraints)
            st.conflicting = st.id == out.worst;
    }

    // Free degrees of freedom of each object: how many independent ways its own variables can
    // move, which is the rank of its rows in a basis of the null space of the Jacobian.
    std::vector<bool> isPivotCol(n, false);
    for (std::size_t c : el.pivCol)
        isPivotCol[c] = true;
    std::vector<std::size_t> freeCols;
    for (std::size_t c = 0; c < n; ++c)
        if (!isPivotCol[c])
            freeCols.push_back(c);
    const std::size_t d = freeCols.size();
    double nullScale = 0;
    std::vector<double> N(n * d, 0.0);  // null space basis, one column per free variable
    for (std::size_t t = 0; t < d; ++t) {
        N[freeCols[t] * d + t] = 1.0;
        for (std::size_t k = el.pivCol.size(); k-- > 0;) {
            const double* row = el.M.data() + el.pivRow[k] * n;
            double sum = row[freeCols[t]];
            for (std::size_t j = k + 1; j < el.pivCol.size(); ++j)
                sum += row[el.pivCol[j]] * N[el.pivCol[j] * d + t];
            N[el.pivCol[k] * d + t] = -sum / row[el.pivCol[k]];
        }
    }
    for (std::size_t t = 0; t < d; ++t) {  // every null vector scaled to a largest entry of 1
        double big = 0;
        for (std::size_t v = 0; v < n; ++v)
            big = std::max(big, std::abs(N[v * d + t]));
        if (big > 0)
            for (std::size_t v = 0; v < n; ++v)
                N[v * d + t] /= big;
    }
    nullScale = 1.0;
    for (const Slot& s : m.slots) {
        EntityStatus es;
        es.entity = s.id;
        std::vector<double> block(
            N.begin() + static_cast<std::ptrdiff_t>(s.offset * d),
            N.begin() + static_cast<std::ptrdiff_t>((s.offset + s.count) * d));
        es.degreesOfFreedom =
            d == 0 ? 0 : rankOf(std::move(block), s.count, d, nullScale, kDerivedTol);
        es.degreesOfFreedom = std::clamp(es.degreesOfFreedom, 0, static_cast<int>(s.count));
        for (const Constraint& c : m.constraints)
            es.constraints += (c.a.entity == s.id || c.b.entity == s.id) ? 1 : 0;
        a.entities.push_back(es);
    }
    return a;
}

}  // namespace tbcad::constraints
