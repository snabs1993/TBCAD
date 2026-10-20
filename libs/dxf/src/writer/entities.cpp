// DXF export: entity records.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <future>
#include <memory>
#include <numbers>
#include <thread>

#include "keep.h"
#include "tbcad/blocks/array.h"
#include "tbcad/blocks/blocks.h"
#include "tbcad/blocks/explode.h"
#include "tbcad/dim/dim.h"
#include "tbcad/gdt/gdt.h"
#include "tbcad/geom/entity.h"
#include "tbcad/mline/mline.h"
#include "tbcad/table/table.h"
#include "tbcad/text/field.h"
#include "tbcad/text/text.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"
#include "writer_impl.h"

namespace tbcad::dxf::detail {

namespace {

constexpr double kDeg = 180.0 / kPi;

// Plain text of TBCAD markup: line breaks kept as '\n', formatting codes and groups removed.
std::string plainLines(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char k = s[i + 1];
            if (k == '\\' || k == '{' || k == '}') {
                out.push_back(k);
                ++i;
            } else if (k == 'P') {
                out.push_back('\n');
                ++i;
            } else if (k == 'L' || k == 'l' || k == 'O' || k == 'o') {
                ++i;  // underline / overline switches carry no argument
            } else if (k == 'S') {
                // Stacked fraction: written flat as num/den.
                std::size_t semi = s.find(';', i);
                std::string_view body =
                    s.substr(i + 2, semi == std::string_view::npos ? s.size() : semi - i - 2);
                for (char ch : body)
                    if (ch != '\\')
                        out.push_back(ch == '#' || ch == '^' ? '/' : ch);
                i = semi == std::string_view::npos ? s.size() : semi;
            } else {
                // Formatting code: skip to its ';'.
                std::size_t semi = s.find(';', i);
                i = semi == std::string_view::npos ? s.size() : semi;
            }
        } else if (c == '{' || c == '}' || c == '\r') {
            continue;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// TBCAD markup -> DXF MTEXT markup (before the text is escaped for the file's encoding).
std::string toDxfMarkup(std::string_view s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') {
            out += "\\P";
            ++i;
        } else if (c == '\r') {
            ++i;
        } else if (c == '\\' && i + 1 < s.size()) {
            char k = s[i + 1];
            if ((k == 'b' || k == 'i') && i + 3 < s.size() + 0 &&
                (s[i + 2] == '0' || s[i + 2] == '1') && s[i + 3] == ';') {
                // Gather adjacent bold/italic codes into one font code.
                bool bold = false, italic = false;
                while (i + 3 < s.size() && s[i] == '\\' && (s[i + 1] == 'b' || s[i + 1] == 'i') &&
                       (s[i + 2] == '0' || s[i + 2] == '1') && s[i + 3] == ';') {
                    (s[i + 1] == 'b' ? bold : italic) = s[i + 2] == '1';
                    i += 4;
                }
                out += std::string("\\fArial|b") + (bold ? "1" : "0") + "|i" +
                       (italic ? "1" : "0") + "|c0|p0;";
            } else if (k == 'C') {
                std::size_t semi = s.find(';', i);
                if (semi == std::string_view::npos) {
                    i = s.size();
                    continue;
                }
                int idx = std::atoi(std::string(s.substr(i + 2, semi - i - 2)).c_str());
                out += "\\C" + std::to_string(aciForIndex(idx).aci) + ";";
                i = semi + 1;
            } else {
                out.push_back(c);
                out.push_back(k);
                i += 2;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

// Splits an already escaped string in pieces of at most `n` characters without cutting a \U+XXXX.
std::vector<std::string> chunks(const std::string& s, std::size_t n) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t len = std::min(n, s.size() - pos);
        if (pos + len < s.size()) {
            // Do not end inside an escape: look back for a backslash in the last 6 characters.
            for (std::size_t back = 1; back <= 6 && back <= len; ++back) {
                if (s[pos + len - back] == '\\') {
                    if (back < 7 && s.compare(pos + len - back, 3, "\\U+") == 0 && back < 7)
                        len -= back;
                    break;
                }
            }
            if (len == 0)
                len = std::min(n, s.size() - pos);
        }
        out.push_back(s.substr(pos, len));
        pos += len;
    }
    if (out.empty())
        out.push_back("");
    return out;
}

DataItem noteText(std::string s) {
    DataItem it;
    it.code = 1000;
    it.text = std::move(s);
    return it;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Common parts

Props Writer::propsOf(const EntityCommon& c) {
    Props p;
    p.layer = layerNameOf(c.layer);
    p.linetype = linetypeNameOf(c.linetype);
    p.color = c.color;
    p.lineweight = c.lineweight;
    p.transparency = c.transparency;
    if (c.linetypeScale == 1.0f) {
        p.ltScale = 1.0;
    } else {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.7g", static_cast<double>(c.linetypeScale));
        p.ltScale = std::strtod(buf, nullptr);
    }
    p.visible = c.visible;
    if (anyEntityPlotStyle_)
        p.plotStyle = entityPlotStyle(doc_.header(), c.id);
    if (!c.ext.get().dict.empty())
        p.dict = &c.ext.get().dict;
    return p;
}

Props Writer::propsOfStyle(const DisplayStyle& st, const std::string& fallbackLayer) {
    Props p;
    p.layer = st.layer ? layerNameOf(st.layer) : fallbackLayer;
    p.trueColor = st.color;
    int hundredths = static_cast<int>(std::lround(st.lineweight * 100.0));
    p.lineweight = LineWeight::hundredths(static_cast<std::int16_t>(validLineweight(hundredths)));
    if (st.color.a < 255 && !isBackgroundColor(st.color))
        p.transparency = static_cast<std::int8_t>(
            std::clamp<long>(std::lround(100.0 * (255 - st.color.a) / 255.0), 0, kTransparencyMax));
    if (st.pattern) {
        auto it = ltByPattern_.find(st.pattern);
        if (it != ltByPattern_.end()) {
            p.linetype = it->second;
            p.ltScale = st.patternScale;
        }
    }
    return p;
}

void Writer::sub(std::string_view subclass) {
    if (!r12_ && !subclass.empty())
        L().o->raw(100, subclass);
}

void Writer::colorGroups(const Props& p) {
    Out& o = *L().o;
    if (p.trueColor) {
        AciChoice c = aciForRgba(*p.trueColor);
        o.i(62, c.aci);
        if (!r12_ && c.rgb)
            o.i(420, *c.rgb);
        return;
    }
    switch (p.color.kind) {
        case ColorKind::ByBlock:
            o.i(62, 0);
            break;
        case ColorKind::Index:
        case ColorKind::Rgb: {
            AciChoice c = aciFor(p.color);
            o.i(62, c.aci);
            if (!r12_ && c.rgb)
                o.i(420, *c.rgb);
            break;
        }
        default:
            break;  // ByLayer is the default
    }
}

void Writer::head(std::string_view type, const Props& p, std::uint64_t owner,
                  std::string_view subclass) {
    Out& o = *L().o;
    o.zero(type);
    if (r12_ && owner == paperRec_ && owner != 0)
        o.i(67, 1);
    if (!r12_) {
        const std::uint64_t self = L().entHandle ? L().entHandle : newHandle();
        if (L().entHandle)
            L().handlesWritten.insert(L().entHandle);
        L().entHandle = 0;
        L().lastHead = self;
        o.h(5, self);
        if (p.dict) {
            PendingDict pd;
            pd.handle = newHandle();
            pd.owner = self;
            pd.records = *p.dict;
            o.raw(102, "{ACAD_XDICTIONARY");
            o.h(360, pd.handle);
            o.raw(102, "}");
            L().pendingDicts.push_back(std::move(pd));
        }
        o.h(330, owner);
        o.raw(100, "AcDbEntity");
    }
    o.str(8, p.layer);
    if (!p.linetype.empty())
        o.str(6, p.linetype);
    colorGroups(p);
    if (!r12_) {
        if (p.lineweight.isValue())
            o.i(370, validLineweight(p.lineweight.v));
        else if (p.lineweight.isByBlock())
            o.i(370, -2);
        else if (p.lineweight.isDefault())
            o.i(370, -3);
        if (p.transparency != kTransparencyByLayer)
            o.i(440, transparencyGroup(p.transparency));
        if (!p.plotStyle.empty()) {
            o.i(380, 3);
            o.h(390, pstyleHandle(p.plotStyle));
        }
    }
    if (p.ltScale != 1 && p.ltScale > 0)
        o.d(48, p.ltScale);
    if (!p.visible)
        o.i(60, 1);
    sub(subclass);
}

void Writer::skipEntity(std::string_view what, const std::string& why) {
    ++summary_.skipped;
    rep_.warn(wcode::kSkippedEntity, std::string(what) + " not written: " + why);
}

void Writer::downgrade(std::string_view what, const std::string& why) {
    ++summary_.downgraded;
    if (warnedOnce_.insert(std::string(what) + why).second) {
        rep_.warn(wcode::kDowngraded, std::string(what) + " " + why);
    }
}

// ---------------------------------------------------------------------------------------------
// Dispatch

// The handles the entities had in the source file. New handles start above the highest of them.
void Writer::loadSourceHandles() {
    if (r12_)
        return;
    const auto& custom = doc_.header().custom;
    auto it = custom.find(kKeptHandlesKey);
    if (it == custom.end())
        return;
    constexpr std::uint64_t kLimit = std::uint64_t{1} << 48;
    for (const auto& [id, handle] : unpackHandles(it->second)) {
        if (handle == 0 || handle >= kLimit || !doc_.find(EntityId{id}))
            continue;
        if (!srcHandleReserved_.insert(handle).second)
            continue;  // two entities with one handle: the first keeps it
        srcHandle_[id] = handle;
        srcHandleTop_ = std::max(srcHandleTop_, handle);
    }
}

// Objects a dimension or leader names (associative points, DIMBREAK Auto) take their handle now,
// so TBCAD_DIM can name them whether they are written before or after it.
void Writer::reserveNamedHandles() {
    if (r12_)
        return;
    const auto reserve = [&](EntityId id) {
        if (!id || srcHandle_.count(id.value) || !doc_.find(id))
            return;
        const std::uint64_t h = newHandle();
        srcHandleReserved_.insert(h);
        srcHandle_[id.value] = h;
    };
    doc_.forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<DimensionData>(e)) {
            for (const DimAssoc& a : d->assoc)
                if (a.kind != DimAssoc::Kind::None)
                    reserve(a.target);
            for (EntityId id : d->breakWith)
                reserve(id);
        } else if (const auto* l = entityAs<LeaderData>(e)) {
            for (EntityId id : l->breakWith)
                reserve(id);
        } else if (const auto* h = entityAs<HatchData>(e)) {
            for (EntityId id : h->sourceBoundary)
                reserve(id);
        } else if (const auto* m = entityAs<MLeaderData>(e);
                   m && opt_.version == DxfVersion::R2007 && !m->attribs.empty()) {
            // A MULTILEADER names the ATTDEFs of its block.
            if (const BlockDef* b = doc_.block(m->block))
                for (EntityId id : b->entities)
                    if (const Entity* ad = doc_.find(id))
                        if (const auto* t = entityAs<TextData>(*ad); t && t->attdef)
                            reserve(id);
        }
    });
}

std::string Writer::handleRef(EntityId id) const {
    const auto it = srcHandle_.find(id.value);
    if (it == srcHandle_.end())
        return "0";
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llX", static_cast<unsigned long long>(it->second));
    return buf;
}

void Writer::writeDimExtras(const std::vector<DimBreak>& breaks, const std::vector<EntityId>& with,
                            std::uint32_t autoBreaks, const DimensionData* dim,
                            const LeaderData* leader) {
    std::vector<std::string> items;
    const auto num = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", std::isfinite(v) ? v : 0.0);
        return std::string(buf);
    };
    const auto pt = [&](Vec2 p) { return num(p.x) + "," + num(p.y); };
    for (const DimBreak& b : breaks)
        items.push_back("b=" + pt(b.a) + "," + pt(b.b));
    if (!with.empty() && !r12_) {
        items.push_back("auto=" + std::to_string(autoBreaks));
        for (EntityId id : with)
            items.push_back("w=" + handleRef(id));
    }
    if (leader && leaderKeepsTextPos(*leader))
        items.push_back("tp=" + pt(leader->textPos));
    if (dim) {
        for (const Vec2& j : dim->jogs)
            items.push_back("j=" + pt(j));
        if (dim->flipArrow1 || dim->flipArrow2)
            items.push_back(std::string("flip=") + (dim->flipArrow1 ? "1" : "0") +
                            (dim->flipArrow2 ? "1" : "0"));
        for (std::size_t k = 0; k < dim->assoc.size() && !r12_; ++k)
            if (const DimAssoc& a = dim->assoc[k]; a.kind != DimAssoc::Kind::None)
                items.push_back("as" + std::to_string(k) + "=" +
                                std::to_string(static_cast<int>(a.kind)) + ":" +
                                handleRef(a.target) + ":" + std::to_string(a.index));
    }
    if (items.empty())
        return;
    L().o->raw(1001, "TBCAD_DIM");
    for (const std::string& it : items)
        L().o->raw(1000, it);
}

void Writer::pickEntityHandle(EntityId id) {
    L().entHandle = 0;
    auto it = srcHandle_.find(id.value);
    if (it != srcHandle_.end())
        L().entHandle = it->second;
    if (!groupMembers_.count(id.value))
        return;
    if (!L().entHandle && !r12_)
        L().entHandle = newHandle();
    groupHandle_[id.value] = L().entHandle;
}

const gdt::FrameTable& Writer::frames() {
    if (!frames_)
        frames_ = gdt::readFrames(doc_.header());
    return *frames_;
}

bool Writer::plainBlock(BlockId id) {
    auto [it, fresh] = plainBlock_.try_emplace(id.value, false);
    if (!fresh)
        return it->second;
    const BlockDef* bd = doc_.block(id);
    if (!bd || bd->dynamic || bd->assembly || blockNameOf(id).empty())
        return false;
    if (!bd->xrefPath.empty() && underlay::isImagePath(bd->xrefPath))
        return false;
    if (frames().count(bd->name))
        return false;
    for (EntityId eid : bd->entities)
        if (const Entity* e = doc_.find(eid))
            if (const auto* t = entityAs<TextData>(*e); t && t->attdef && !t->attdef->constant)
                return false;  // its attributes are records of their own
    return it->second = true;
}

bool Writer::plainEntity(const Entity& e) {
    if (r12_)
        return false;
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Point:
            break;
        case EntityKind::Polyline:
            if (entityAs<PolylineData>(e)->v.size() < 2)
                return false;  // skipped with a warning
            if (e.common.wipeout)
                return false;  // a WIPEOUT, which needs its class
            break;
        case EntityKind::Text: {
            const auto* t = entityAs<TextData>(e);
            if (t->attdef || text::hasFields(t->utf8))
                return false;
            break;
        }
        case EntityKind::MText:
            if (text::hasFields(entityAs<MTextData>(e)->utf8))
                return false;
            break;
        case EntityKind::Insert: {
            const auto* d = entityAs<InsertData>(e);
            if (d->array || d->clip.size() >= 3 || !plainBlock(d->block))
                return false;
            break;
        }
        default:
            return false;
    }
    if (!e.common.ext.get().dict.empty())
        return false;  // takes a handle of its own for the dictionary
    if (anyEntityPlotStyle_ && !entityPlotStyle(doc_.header(), e.common.id).empty())
        return false;  // plot style names get their handles in order of first use
    return groupMembers_.empty() || !groupMembers_.count(e.common.id.value);
}

void Writer::writeEntityList(const std::vector<EntityId>& ids, std::uint64_t owner) {
    // A long list: the plain records are set aside with the handles they will have, and written
    // together at the end on several threads.
    constexpr std::size_t kDeferMin = 40000;
    constexpr std::size_t kUnit = 4096;
    const std::size_t hw = opt_.maxThreads ? opt_.maxThreads : std::thread::hardware_concurrency();
    const bool defer = !tlLane_ && !r12_ && hw >= 2 && ids.size() >= kDeferMin;
    std::vector<PlainItem> items;
    std::vector<PlainUnit> units;
    for (std::size_t i = 0; i < ids.size();) {
        const Entity* e = doc_.find(ids[i]);
        if (!e) {
            ++i;
            continue;
        }
        if (entityAs<DimensionData>(*e)) {
            i += writeDimension(ids, i, owner);
            L().entHandle = 0;
            writeAnnotativeData(*e);
            continue;
        }
        if (defer && plainEntity(*e)) {
            PlainItem item;
            item.e = e;
            auto it = srcHandle_.find(ids[i].value);
            if (it != srcHandle_.end())
                item.kept = it->second;
            else
                item.fresh = newHandle();
            const std::size_t at = L().o->buf.size();
            if (units.empty() || units.back().at != at ||
                units.back().to - units.back().from >= kUnit)
                units.push_back({at, items.size(), items.size()});
            items.push_back(item);
            units.back().to = items.size();
            ++i;
            continue;
        }
        pickEntityHandle(ids[i]);
        writeEntity(*e, owner);
        L().entHandle = 0;
        ++i;
    }
    if (!items.empty())
        flushPlain(items, units, owner);
}

void Writer::flushPlain(std::vector<PlainItem>& items, std::vector<PlainUnit>& units,
                        std::uint64_t owner) {
    const std::size_t hw = opt_.maxThreads ? opt_.maxThreads : std::thread::hardware_concurrency();
    const std::size_t lanes = std::clamp<std::size_t>(std::min(hw, units.size()), 1, 16);
    // Several threads read the document: the lookups that fill a cache on first use are done now.
    for (const auto& [id, layer] : doc_.layers())
        (void)doc_.effective(id);
    std::vector<std::string> blobs(units.size());
    struct Worker {
        Lane lane;
        Out out;
        Worker(bool utf8, bool r12) : out(utf8, r12) {}
    };
    std::vector<std::unique_ptr<Worker>> workers;
    for (std::size_t c = 0; c < lanes; ++c)
        workers.push_back(std::make_unique<Worker>(utf8_, r12_));
    std::atomic<std::size_t> next{0};
    auto work = [&](std::size_t c) {
        Worker& w = *workers[c];
        w.lane.o = &w.out;
        Lane* saved = tlLane_;
        tlLane_ = &w.lane;
        try {
            text::FontScope fonts;  // texts are written without waiting for the other threads
            for (std::size_t u = next++; u < units.size(); u = next++) {
                for (std::size_t k = units[u].from; k < units[u].to; ++k) {
                    const PlainItem& item = items[k];
                    L().entHandle = item.kept;
                    if (!item.kept)
                        L().nextHandle = item.fresh;  // newHandle() hands out exactly this one
                    writeEntity(*item.e, owner);
                    L().entHandle = 0;
                }
                blobs[u] = std::move(w.out.buf);
                w.out.buf = std::string();
            }
        } catch (...) {
            tlLane_ = saved;
            throw;
        }
        tlLane_ = saved;
    };
    std::vector<std::future<void>> jobs;
    for (std::size_t c = 1; c < lanes; ++c)
        jobs.push_back(std::async(std::launch::async, work, c));
    std::exception_ptr failure;
    try {
        work(0);
    } catch (...) {
        failure = std::current_exception();
    }
    for (auto& j : jobs) {
        try {
            j.get();
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    }
    if (failure)
        std::rethrow_exception(failure);

    // Each unit's text goes where it was set aside; what the workers counted is added up.
    Out& dest = *L().o;
    std::size_t total = dest.buf.size();
    for (const std::string& b : blobs)
        total += b.size();
    std::string merged;
    // (some room to spare: the model's entities get the rest of the file put in front of them
    // in place, see Writer::run)
    merged.reserve(total + total / 16 + (std::size_t{1} << 20));
    std::size_t done = 0;
    for (std::size_t u = 0; u < units.size(); ++u) {
        merged.append(dest.buf, done, units[u].at - done);
        done = units[u].at;
        merged += blobs[u];
        std::string().swap(blobs[u]);
    }
    merged.append(dest.buf, done, std::string::npos);
    dest.buf = std::move(merged);
    for (auto& w : workers) {
        dest.badNumbers += w->out.badNumbers;
        for (const auto& [type, count] : w->lane.counts)
            L().counts[type] += count;
        L().handlesWritten.insert(w->lane.handlesWritten.begin(), w->lane.handlesWritten.end());
    }
}

void Writer::setLevelTag(const EntityCommon& c) {
    L().levelTag.clear();
    if (!tagLevels_)
        return;
    if (const Level* l = doc_.level(c.level))
        L().levelTag = l->name;
}

void Writer::emitLevelTag() {
    if (L().levelTag.empty() || !L().o)
        return;
    openTbcadXdata();
    L().o->str(1000, "level=" + L().levelTag);
    L().levelTag.clear();
}

void Writer::writeEntity(const Entity& e, std::uint64_t owner) {
    Props p = propsOf(e.common);
    L().cur = &e.common;
    setLevelTag(e.common);
    switch (kind(e)) {
        case EntityKind::Line:
            writeLine(p, *entityAs<LineData>(e), owner);
            break;
        case EntityKind::Circle:
            writeCircle(p, *entityAs<CircleData>(e), owner);
            break;
        case EntityKind::Arc:
            writeArc(p, *entityAs<ArcData>(e), owner);
            break;
        case EntityKind::Polyline:
            if (!e.common.wipeout || !writeWipeout(p, *entityAs<PolylineData>(e), owner))
                writePolyline(p, *entityAs<PolylineData>(e), owner);
            break;
        case EntityKind::Point:
            writePoint(p, *entityAs<PointData>(e), owner);
            break;
        case EntityKind::Ellipse:
            writeEllipse(p, *entityAs<EllipseData>(e), e, owner);
            break;
        case EntityKind::Spline:
            writeSpline(p, *entityAs<SplineData>(e), e, owner);
            break;
        case EntityKind::Text:
            writeText(p, *entityAs<TextData>(e), owner);
            break;
        case EntityKind::MText:
            writeMText(p, *entityAs<MTextData>(e), owner);
            break;
        case EntityKind::Hatch: {
            const HatchData& h = *entityAs<HatchData>(e);
            if (p.color.kind == ColorKind::ByLayer && h.fill.kind != ColorKind::ByLayer)
                p.color = h.fill;
            writeHatch(p, h, owner);
            break;
        }
        case EntityKind::Insert: {
            const auto* ins = entityAs<InsertData>(e);
            const BlockDef* bd = doc_.block(ins->block);
            if (bd && (bd->dynamic || bd->assembly)) {
                // DXF has no dynamic blocks: the current state is written as plain objects.
                downgrade("INSERT",
                          "a dynamic block or assembly is written as its current geometry");
                if (auto parts = blocks::explode(doc_, e)) {
                    for (const Entity& part : parts.value().entities)
                        if (kind(part) != EntityKind::Dimension)
                            writeEntity(part, owner);
                }
                break;
            }
            writeInsert(p, *ins, owner);
            break;
        }
        case EntityKind::Dimension: {
            std::vector<EntityId> one{e.common.id};
            writeDimension(one, 0, owner);
            break;
        }
        case EntityKind::Leader:
            writeLeader(p, *entityAs<LeaderData>(e), owner);
            break;
        case EntityKind::MLeader: {
            // DXF 2007 and later have MULTILEADER; older versions get its pieces.
            if (opt_.version == DxfVersion::R2007 &&
                writeMLeader(p, e, *entityAs<MLeaderData>(e), owner))
                break;
            downgrade("MULTILEADER",
                      "is written as leaders, a line for the dogleg, and the note or block");
            if (auto parts = dim::mleaderAsLeaders(doc_, e)) {
                // Each piece carries a TBCAD_MLEADER note, the first one the multileader itself,
                // so our importer makes them one multileader again (see docs/dxf-export.md).
                std::vector<Entity>& v = parts.value();
                const std::string head = "a=" + std::to_string(nextMleaderId_++) +
                                         ";n=" + std::to_string(v.size()) + ";i=";
                for (std::size_t i = 0; i < v.size(); ++i) {
                    ExtData x = v[i].common.ext.get();
                    XdataApp app{"TBCAD_MLEADER", {noteText(head + std::to_string(i))}};
                    if (i == 0)
                        mleaderNote(*entityAs<MLeaderData>(e), app.items);
                    x.xdata.push_back(std::move(app));
                    v[i].common.ext = ExtDataPtr(std::move(x));
                    writeEntity(v[i], owner);
                }
            }
            break;
        }
        case EntityKind::Proxy:
            writeProxy(p, *entityAs<ProxyData>(e), owner);
            break;
        case EntityKind::Table:
            writeTable(p, e, owner);
            break;
        case EntityKind::Ray:
            writeRay(p, *entityAs<RayData>(e), e, owner);
            break;
        case EntityKind::MLine:
            writeMline(p, *entityAs<MlineData>(e), e, owner);
            break;
    }
    if (kind(e) != EntityKind::Insert)
        writeAnnotativeData(e);
}

// The multileader a TBCAD_MLEADER note carries: content kind and dogleg, text height, block scale
// and rotation, landing and content point, the lines (count, then per line its point count and
// x, y), the dimension style's name and the note as hex UTF-8 in pieces.
void Writer::mleaderNote(const MLeaderData& m, std::vector<DataItem>& items) {
    auto real = [&](double v) {
        DataItem it;
        it.code = 1040;
        it.real = v;
        items.push_back(it);
    };
    auto count = [&](std::size_t n) {
        DataItem it;
        it.code = 1071;
        it.integer = static_cast<std::int64_t>(n);
        items.push_back(it);
    };
    items.push_back(noteText("c=" + std::to_string(static_cast<int>(m.content)) +
                             ";d=" + (m.dogleg ? "1" : "0")));
    real(m.textHeight);
    real(m.blockScale);
    real(m.blockRotation);
    for (Vec2 p : {m.landing, m.contentPos}) {
        real(p.x);
        real(p.y);
    }
    count(m.lines.size());
    for (const auto& line : m.lines) {
        count(line.size());
        for (Vec2 p : line) {
            real(p.x);
            real(p.y);
        }
    }
    if (const DimStyle* ds = doc_.dimStyle(m.style))
        items.push_back(noteText("style=" + ds->name));
    static constexpr char kDigits[] = "0123456789ABCDEF";
    constexpr std::size_t kPiece = 120;  // bytes per 1000 group (240 hex digits, under 255)
    for (std::size_t at = 0; at < m.utf8.size(); at += kPiece) {
        std::string hex = "t=";
        for (std::size_t k = at; k < std::min(m.utf8.size(), at + kPiece); ++k) {
            const auto b = static_cast<unsigned char>(m.utf8[k]);
            hex.push_back(kDigits[b >> 4]);
            hex.push_back(kDigits[b & 15]);
        }
        items.push_back(noteText(hex));
    }
}

// The multileader style a multileader names: one MLEADERSTYLE per dimension style used, written
// with the OBJECTS section ("Standard" always exists).
std::uint64_t Writer::mleaderStyleHandle(DimStyleId style) {
    if (!mleaderStdStyle_)
        mleaderStdStyle_ = newHandle();
    if (!style || style == kDimStyleStandard)
        return mleaderStdStyle_;
    auto [it, fresh] = mleaderStyles_.try_emplace(style.value, 0);
    if (fresh)
        it->second = newHandle();
    return it->second;
}

std::uint64_t Writer::textStyleHandle(TextStyleId id) {
    if (!id || !doc_.textStyle(id))
        id = kTextStyleStandard;
    std::uint64_t& h = tsHandle_[id.value];
    if (h == 0)
        h = newHandle();
    return h;
}

std::uint64_t Writer::byBlockLinetypeHandle() {
    if (!byBlockLt_)
        byBlockLt_ = newHandle();
    return byBlockLt_;
}

// A MULTILEADER as AutoCAD 2007 and later write it: the context data (scale, note or block, the
// leader with its lines), then the entity's own copy of the style properties, all marked as
// overridden so the record shows the same whatever its style says. A TBCAD_MLEADER note carries
// the multileader exactly for our importer (as on the pieces older versions get).
bool Writer::writeMLeader(const Props& p, const Entity& e, const MLeaderData& m,
                          std::uint64_t owner) {
    std::vector<std::vector<Vec2>> lines;
    for (const auto& l : m.lines)
        if (l.size() >= 2)
            lines.push_back(l);
    if (lines.empty())
        return false;
    const BlockDef* block =
        m.content == MLeaderData::Content::Block ? doc_.block(m.block) : nullptr;
    if (block && (blockNameOf(m.block).empty() || !blockRec_.count(m.block.value)))
        return false;
    const DimStyle& st = dim::styleOf(doc_, m.style);
    const double f = annotationFactor(doc_.header(), e.common);
    const double k = (st.overallScale > 0 ? st.overallScale : 1.0) * f;
    const dim::Layout lay = dim::layoutMLeader(doc_, m, 0.01, f);
    const double th =
        lay.text.valid() ? lay.text.height : (m.textHeight > 0 ? m.textHeight : st.textHeight) * k;
    const bool text = m.content == MLeaderData::Content::Text && !m.utf8.empty();
    const std::uint64_t textStyle = textStyleHandle(st.textStyle);
    constexpr long long kByBlock = -1056964608;  // colour ByBlock, as raw true colour
    const Vec2 dogleg = m.dogleg ? m.contentPos - m.landing : Vec2{};
    const double doglegLen = length(dogleg);
    Vec2 dir = doglegLen > 1e-12 ? dogleg / doglegLen : Vec2{1, 0};
    if (doglegLen <= 1e-12 && lines.front().size() >= 2) {
        const Vec2 from = lines.front()[lines.front().size() - 2];
        dir = {m.landing.x >= from.x ? 1.0 : -1.0, 0};
    }
    const Vec2 base = m.dogleg ? m.contentPos : m.landing;

    head("MULTILEADER", p, owner, "AcDbMLeader");
    Out& o = *L().o;
    o.i(270, 2);
    o.raw(300, "CONTEXT_DATA{");
    o.d(40, k);
    o.pt(10, base);
    o.d(41, th);
    o.d(140, st.arrowSize * k);
    o.d(145, st.textGap * k);
    o.i(174, 1);
    o.i(175, 1);
    o.i(176, 0);
    o.i(177, 0);
    o.i(290, text ? 1 : 0);
    if (text) {
        // The note's top line is centred on the end of the dogleg ("middle of top line").
        const bool left = lay.text.valid() && lay.text.attachRight;
        const Vec2 at = lay.text.valid() ? lay.text.center + Vec2{0, th / 2}
                                         : base + Vec2{st.textGap * k, th / 2};
        o.raw(304, encodeText(toDxfMarkup(text::expandFields(doc_, m.utf8)), utf8_));
        o.pt(11, {0, 0}, 1);
        o.h(340, textStyle);
        o.pt(12, at);
        o.pt(13, {1, 0});
        o.d(42, 0);
        o.d(43, 0);
        o.d(44, 0);
        o.d(45, 1);
        o.i(170, 1);
        o.i(90, kByBlock);
        o.i(171, left ? 3 : 1);
        o.i(172, 1);
        o.i(91, -939524096);
        o.d(141, 1.5);
        o.i(92, 0);
        o.i(291, 0);
        o.i(292, 0);
        o.i(173, 0);
        o.i(293, 0);
        o.d(142, 0);
        o.d(143, 0);
        o.i(294, 0);
        o.i(295, 1);
    }
    o.i(296, block ? 1 : 0);
    const double bs = m.blockScale * f;
    if (block) {
        // The block's insertion point is the end of the dogleg (connection by insertion point).
        o.h(341, blockRec_[m.block.value]);
        o.pt(14, {0, 0}, 1);
        o.pt(15, m.contentPos);
        o.d(16, bs);
        o.d(26, bs);
        o.d(36, 1);
        o.d(46, m.blockRotation);
        o.i(93, kByBlock);
        const double c = std::cos(m.blockRotation), s = std::sin(m.blockRotation);
        for (double v : {bs * c, -bs * s, 0.0, m.contentPos.x, bs * s, bs * c, 0.0, m.contentPos.y,
                         0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0})
            o.d(47, v);
    }
    o.pt(110, {0, 0});
    o.pt(111, {1, 0});
    o.pt(112, {0, 1});
    o.i(297, 0);
    // One leader: every line ends at the landing, which is the leader's last point.
    o.raw(302, "LEADER{");
    o.i(290, 1);
    o.i(291, 1);
    o.pt(10, m.landing);
    o.pt(11, dir);
    o.i(90, 0);
    o.d(40, doglegLen);
    int index = 0;
    for (const auto& l : lines) {
        o.raw(304, "LEADER_LINE{");
        for (std::size_t i = 0; i + 1 < l.size(); ++i)
            o.pt(10, l[i]);
        o.i(91, index++);
        o.i(92, kByBlock);
        o.raw(305, "}");
    }
    o.raw(303, "}");
    o.raw(301, "}");

    o.h(340, mleaderStyleHandle(m.style));
    o.i(90, 2147483647);  // every property below overrides the style
    o.i(170, 1);
    o.i(91, kByBlock);
    o.h(341, byBlockLinetypeHandle());
    o.i(171, -2);
    o.i(290, 1);
    o.i(291, m.dogleg ? 1 : 0);
    o.d(41, doglegLen);
    o.d(42, st.arrowSize * k);
    o.i(172, block ? 1 : text ? 2 : 0);
    o.h(343, textStyle);
    o.i(173, 1);
    o.i(95, 1);
    o.i(174, 1);
    o.i(175, 0);
    o.i(92, kByBlock);
    o.i(292, 0);
    if (block)
        o.h(344, blockRec_[m.block.value]);
    o.i(93, kByBlock);
    o.pt(10, {m.blockScale, m.blockScale}, 1);
    o.d(43, m.blockRotation);
    o.i(176, 1);
    o.i(293, e.common.annotative ? 1 : 0);
    if (block) {
        // The attribute values, by the handle of their ATTDEF in the block.
        int at = 0;
        for (EntityId id : block->entities) {
            const Entity* ad = doc_.find(id);
            const auto* t = ad ? entityAs<TextData>(*ad) : nullptr;
            if (!t || !t->attdef || t->attdef->constant)
                continue;
            const AttDefInfo& info = *t->attdef;
            const std::string handle = handleRef(id);
            const int idx = at++;
            if (handle == "0")
                continue;
            std::string value = info.defaultValue;
            for (const AttribValue& a : m.attribs)
                if (a.tag == info.tag)
                    value = a.value;
            o.raw(330, handle);
            o.i(177, idx);
            o.d(44, 1);
            o.str(302, value);
        }
    }
    o.i(294, 0);
    o.i(178, 0);
    o.i(179, 1);
    o.d(45, k);

    XdataApp note{"TBCAD_MLEADER",
                  {noteText("a=" + std::to_string(nextMleaderId_++) + ";n=1;i=0")}};
    mleaderNote(m, note.items);
    o.raw(1001, note.app);
    for (const DataItem& it : note.items) {
        if (it.code == 1040)
            o.d(it.code, it.real);
        else if (it.code == 1071)
            o.i(it.code, it.integer);
        else
            o.str(it.code, it.text);
    }
    count("MULTILEADER");
    return true;
}

// Annotative objects carry a TBCAD extended-data note; other programs ignore it and our importer
// turns it back into the flag. Written after the last group of the record.
void Writer::writeAnnotativeData(const Entity& e) {
    writeTbcadFlags(e);
    if (const auto* d = entityAs<DimensionData>(e))
        writeDimExtras(d->breaks, d->breakWith, d->autoBreaks, d);
    else if (const auto* h = entityAs<HatchData>(e); h && hatchNamesSources(*h)) {
        // The objects the hatch was made from, by handle.
        L().o->raw(1001, "TBCAD_DIM");
        for (EntityId id : h->sourceBoundary)
            if (doc_.find(id))
                L().o->raw(1000, "src=" + handleRef(id));
    }
    writeExtXdata(e.common);
}

// XDATA of other applications that the entity came with, written after the last group of its
// record. Items are written as stored; the 1001 group opens each application.
void Writer::writeExtXdata(const EntityCommon& c) {
    for (const XdataApp& a : c.ext.get().xdata) {
        L().o->str(1001, a.app);
        for (const DataItem& it : a.items) {
            switch (dataKindOf(it.code)) {
                case DataKind::Text:
                    L().o->str(it.code, it.text);
                    break;
                case DataKind::Integer:
                    L().o->i(it.code, it.integer);
                    break;
                case DataKind::Real:
                    L().o->d(it.code, it.real);
                    break;
            }
        }
    }
}

void Writer::openTbcadXdata() {
    if (L().o->xdataApp != "TBCAD")
        L().o->raw(1001, "TBCAD");
}

void Writer::writeTbcadFlags(const Entity& e) {
    // The source of a text with fields, and the hash of the group 1 (and 3) text written for it.
    std::string source, written;
    if (const auto* t = entityAs<TextData>(e); t && !t->attdef) {
        source = t->utf8;
        if (source.find("%<") != std::string::npos)
            written = encodeText(text::expandFields(doc_, source), utf8_);
    } else if (const auto* m = entityAs<MTextData>(e); m && !r12_) {
        source = m->utf8;
        if (source.find("%<") != std::string::npos)
            written = encodeText(toDxfMarkup(text::expandFields(doc_, source)), utf8_);
    }
    if (!written.empty() && written != encodeText(source, utf8_)) {
        openTbcadXdata();
        L().o->raw(1000, "fieldof=" + fieldHash(written) + ";" +
                             fieldHash(text::expandFields(doc_, source, kFieldCheckDate)));
        for (const std::string& piece : fieldPieces(source))
            L().o->raw(1000, piece);
    }
    writeCommonFlags(e.common);
}

void Writer::writeCommonFlags(const EntityCommon& c) {
    if (!c.annotative && !c.wipeout)
        return;
    openTbcadXdata();
    if (c.wipeout)
        L().o->str(1000, "wipeout=1");
    if (!c.annotative)
        return;
    L().o->str(1000, "annotative=1");
    if (!c.annoScales.empty()) {
        std::string s = "annoscales=";
        for (double v : c.annoScales) {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.12g", v);
            s += (s.back() == '=' ? "" : ",") + std::string(buf);
        }
        L().o->str(1000, s);
    }
}

// A table is an INSERT of an anonymous *T block that holds its drawn grid and texts, so any
// program shows it. Our own extended data carries the table itself; the importer rebuilds the
// table from it.
void Writer::writeTable(const Props& p, const Entity& e, std::uint64_t owner) {
    const TableData& t = *entityAs<TableData>(e);
    DimBlock block;
    do {
        block.name = "*TB" + std::to_string(++dimCounter_);  // ours: the reader skips it
    } while (blockSet_.has(block.name));
    blockSet_.add(block.name);
    block.rec = newHandle();
    {
        Out bo(utf8_, r12_);
        Out* saved = L().o;
        L().o = &bo;
        auto parts = table::explodeTable(doc_, e);
        if (parts) {
            for (const Entity& part : parts.value())
                writeEntity(part, block.rec);
        }
        L().o = saved;
        block.body = std::move(bo.buf);
    }
    const std::string blockName = block.name;
    dimBlocks_.push_back(std::move(block));

    head("INSERT", p, owner, "AcDbBlockReference");
    Out& o = *L().o;
    o.str(2, blockName);
    o.pt(10, Vec2{0, 0});
    o.d(41, 1);
    o.d(42, 1);
    o.d(43, 1);
    count("INSERT");
    o.raw(1001, "TBCAD");
    const std::string text = table::serialize(t);
    for (std::size_t at = 0; at < text.size(); at += 200)
        o.str(1000, text.substr(at, 200));
}

// ---------------------------------------------------------------------------------------------
// Simple geometry

void Writer::writeLine(const Props& p, const LineData& d, std::uint64_t owner) {
    head("LINE", p, owner, "AcDbLine");
    L().o->pt(10, d.a);
    L().o->pt(11, d.b);
    count("LINE");
}

void Writer::writeRay(const Props& p, const RayData& d, const Entity& e, std::uint64_t owner) {
    if (r12_) {
        downgrade(d.infinite ? "XLINE" : "RAY", "has no R12 form; written as a long line");
        writeLine(p, LineData{rayStart(d), rayEnd(d)}, owner);
        return;
    }
    (void)e;
    const char* name = d.infinite ? "XLINE" : "RAY";
    head(name, p, owner, d.infinite ? "AcDbXline" : "AcDbRay");
    L().o->pt(10, d.origin);
    L().o->pt(11, d.dir);
    count(name);
}

// A multiline is an INSERT of an anonymous *M block holding its lines, so any program shows it.
// Our extended data carries the multiline and its style; the importer rebuilds it from that.
void Writer::writeMline(const Props& p, const MlineData& d, const Entity& e, std::uint64_t owner) {
    DimBlock block;
    do {
        block.name = "*M" + std::to_string(++dimCounter_);
    } while (blockSet_.has(block.name));
    blockSet_.add(block.name);
    block.rec = newHandle();
    {
        Out bo(utf8_, r12_);
        Out* saved = L().o;
        L().o = &bo;
        auto parts = mline::explodeMline(doc_, e);
        if (parts) {
            for (const Entity& part : parts.value())
                writeEntity(part, block.rec);
        }
        L().o = saved;
        block.body = std::move(bo.buf);
    }
    const std::string blockName = block.name;
    dimBlocks_.push_back(std::move(block));

    head("INSERT", p, owner, "AcDbBlockReference");
    Out& o = *L().o;
    o.str(2, blockName);
    o.pt(10, Vec2{0, 0});
    o.d(41, 1);
    o.d(42, 1);
    o.d(43, 1);
    count("INSERT");
    o.raw(1001, "TBCAD");
    const auto style = mline::findStyle(doc_, d.style);
    const std::string text = mline::serialize(d, style ? *style : mline::standardStyle());
    for (std::size_t at = 0; at < text.size(); at += 200)
        o.str(1000, text.substr(at, 200));
}

void Writer::writePoint(const Props& p, const PointData& d, std::uint64_t owner) {
    head("POINT", p, owner, "AcDbPoint");
    L().o->pt(10, d.p);
    count("POINT");
}

void Writer::writeCircle(const Props& p, const CircleData& d, std::uint64_t owner) {
    head("CIRCLE", p, owner, "AcDbCircle");
    L().o->pt(10, d.center);
    L().o->d(40, d.radius);
    count("CIRCLE");
}

void Writer::writeArc(const Props& p, const ArcData& d, std::uint64_t owner) {
    head("ARC", p, owner, "AcDbCircle");
    L().o->pt(10, d.center);
    L().o->d(40, d.radius);
    sub("AcDbArc");
    L().o->d(50, d.startAngle * kDeg);
    L().o->d(51, d.endAngle * kDeg);
    count("ARC");
}

void Writer::writeVertexPolyline(const Props& p, const std::vector<PolyVertex>& v, bool closed,
                                 double constantWidth, std::uint64_t owner, bool runsOn) {
    // R12 has no LWPOLYLINE: a POLYLINE followed by VERTEX records and SEQEND.
    head("POLYLINE", p, owner, "AcDb2dPolyline");
    Out& o = *L().o;
    o.i(66, 1);
    o.pt(10, {0, 0});
    if (constantWidth > 0) {
        o.d(40, constantWidth);
        o.d(41, constantWidth);
    }
    o.i(70, (closed ? 1 : 0) | (runsOn ? 128 : 0));
    emitLevelTag();  // XDATA of the polyline comes before its vertices
    for (const PolyVertex& pv : v) {
        o.zero("VERTEX");
        o.str(8, p.layer);
        o.pt(10, pv.p);
        if (pv.startWidth != 0 || pv.endWidth != 0) {
            o.d(40, pv.startWidth);
            o.d(41, pv.endWidth);
        }
        if (pv.bulge != 0)
            o.d(42, pv.bulge);
    }
    o.zero("SEQEND");
    o.str(8, p.layer);
    count("POLYLINE");
}

// A wipeout is a WIPEOUT: a one-pixel picture frame stretched over the shape's bounding box, its
// clip boundary in pixel units counted from the top-left (y down), from -0.5 to 0.5. Arc segments
// are written as short lines. False when the shape has no area to frame.
bool Writer::writeWipeout(const Props& p, const PolylineData& d, std::uint64_t owner) {
    if (r12_ || !d.closed || d.v.size() < 2)
        return false;
    std::vector<Vec2> ring;
    for (std::size_t i = 0; i < d.v.size(); ++i) {
        const Vec2 a = d.v[i].p, b = d.v[(i + 1) % d.v.size()].p;
        ring.push_back(a);
        const double bulge = d.v[i].bulge;
        if (bulge == 0 || tol::isZeroLength(b - a))
            continue;
        // the arc through a and b: sweep 4 atan(bulge), split into pieces of at most 5 degrees
        const double sweep = 4 * std::atan(bulge);
        const int n = std::max(2, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 36))));
        const Vec2 mid = (a + b) * 0.5, chord = b - a;
        const Vec2 normal{-chord.y, chord.x};
        // centre = mid + normal * (1 - bulge^2) / (4 bulge)
        const Vec2 centre = mid + normal * ((1 - bulge * bulge) / (4 * bulge));
        const double r = distance(centre, a);
        const double a0 = std::atan2(a.y - centre.y, a.x - centre.x);
        for (int k = 1; k < n; ++k) {
            const double t = a0 + sweep * k / n;
            ring.push_back({centre.x + r * std::cos(t), centre.y + r * std::sin(t)});
        }
    }
    BBox2 box;
    for (const Vec2& q : ring)
        box.expand(q);
    const double w = box.max.x - box.min.x, h = box.max.y - box.min.y;
    if (!(w > tol::kLinear) || !(h > tol::kLinear) || !std::isfinite(w) || !std::isfinite(h))
        return false;
    head("WIPEOUT", p, owner, "AcDbWipeout");
    Out& o = *L().o;
    o.i(90, 0);
    o.pt(10, box.min);
    o.pt(11, Vec2{w, 0});
    o.pt(12, Vec2{0, h});
    o.d(13, 1);
    o.d(23, 1);
    o.h(340, 0);
    o.i(70, 7);  // shown, shown when not aligned, clipped
    o.i(280, 1);
    o.i(281, 50);
    o.i(282, 50);
    o.i(283, 0);
    o.h(360, 0);
    o.i(71, 2);
    o.i(91, static_cast<long long>(ring.size()) + 1);
    auto px = [&](Vec2 q) {
        return Vec2{(q.x - box.min.x) / w - 0.5, 0.5 - (q.y - box.min.y) / h};
    };
    for (const Vec2& q : ring)
        o.pt2(14, px(q));
    o.pt2(14, px(ring.front()));
    wipeouts_ = true;
    count("WIPEOUT");
    return true;
}

void Writer::writePolyline(const Props& p, const PolylineData& d, std::uint64_t owner) {
    if (d.v.size() < 2) {
        skipEntity("LWPOLYLINE", "fewer than two vertices");
        return;
    }
    if (r12_) {
        writeVertexPolyline(p, d.v, d.closed, d.constantWidth, owner, !d.restartLinetype);
        return;
    }
    head("LWPOLYLINE", p, owner, "AcDbPolyline");
    Out& o = *L().o;
    o.i(90, static_cast<long long>(d.v.size()));
    o.i(70, (d.closed ? 1 : 0) | (d.restartLinetype ? 0 : 128));  // 128: linetype runs on
    if (d.constantWidth != 0)
        o.d(43, d.constantWidth);
    for (const PolyVertex& v : d.v) {
        o.pt2(10, v.p);
        if (v.startWidth != 0 || v.endWidth != 0) {
            o.d(40, v.startWidth);
            o.d(41, v.endWidth);
        }
        if (v.bulge != 0)
            o.d(42, v.bulge);
    }
    count("LWPOLYLINE");
}

void Writer::writeCurveFallback(const Entity& e, const Props& p, std::uint64_t owner) {
    geom::DisplayContext ctx;
    ctx.chordTolMm = 0.01;
    DisplayList list;
    geom::buildDisplay(doc_, e, ctx, list);
    for (const DisplayItem& item : list.items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
            std::vector<PolyVertex> v;
            for (Vec2 pt : pl->pts) {
                PolyVertex pv;
                pv.p = pt;
                v.push_back(pv);
            }
            if (v.size() >= 2)
                writeVertexPolyline(p, v, pl->closed, 0, owner);
        }
    }
}

void Writer::writeEllipse(const Props& p, const EllipseData& d, const Entity& e,
                          std::uint64_t owner) {
    if (r12_) {
        downgrade("ELLIPSE", "has no R12 form; written as a polyline");
        writeCurveFallback(e, p, owner);
        return;
    }
    head("ELLIPSE", p, owner, "AcDbEllipse");
    Out& o = *L().o;
    o.pt(10, d.center);
    o.pt(11, d.majorAxis);
    o.d(210, 0);
    o.d(220, 0);
    o.d(230, 1);
    o.d(40, d.ratio);
    double sweep = d.endParam - d.startParam;
    if (sweep >= kTwoPi - 1e-9 || sweep <= 0) {
        o.d(41, 0);
        o.d(42, kTwoPi);
    } else {
        double s = normalizeAngle(d.startParam);
        o.d(41, s);
        o.d(42, s + sweep > kTwoPi ? s + sweep - kTwoPi : s + sweep);
    }
    count("ELLIPSE");
}

void Writer::writeSpline(const Props& p, const SplineData& d, const Entity& e,
                         std::uint64_t owner) {
    if (r12_) {
        downgrade("SPLINE", "has no R12 form; written as a polyline");
        writeCurveFallback(e, p, owner);
        return;
    }
    if (d.control.empty() && d.fit.size() < 2) {
        skipEntity("SPLINE", "no control or fit points");
        return;
    }
    head("SPLINE", p, owner, "AcDbSpline");
    Out& o = *L().o;
    o.d(210, 0);
    o.d(220, 0);
    o.d(230, 1);

    std::vector<double> knots = d.knots;
    bool haveControl = !d.control.empty();
    if (haveControl && knots.size() != d.control.size() + static_cast<std::size_t>(d.degree) + 1) {
        // Clamped uniform knot vector.
        std::size_t n = d.control.size();
        std::size_t deg = static_cast<std::size_t>(std::max(d.degree, 1));
        knots.clear();
        std::size_t interior = n > deg ? n - deg - 1 : 0;
        for (std::size_t i = 0; i <= deg; ++i)
            knots.push_back(0);
        for (std::size_t i = 1; i <= interior; ++i)
            knots.push_back(static_cast<double>(i));
        for (std::size_t i = 0; i <= deg; ++i)
            knots.push_back(static_cast<double>(interior + 1));
    }
    bool rational = haveControl && d.weights.size() == d.control.size();
    int flags = (d.closed ? 1 : 0) | (d.periodic ? 2 : 0) | (rational ? 4 : 0) | 8;
    o.i(70, flags);
    o.i(71, d.degree);
    o.i(72, haveControl ? static_cast<long long>(knots.size()) : 0);
    o.i(73, static_cast<long long>(d.control.size()));
    o.i(74, static_cast<long long>(d.fit.size()));
    o.d(42, 1e-7);
    o.d(43, 1e-7);
    o.d(44, 1e-10);
    if (haveControl) {
        for (double k : knots)
            o.d(40, k);
        if (rational) {
            for (double w : d.weights)
                o.d(41, w);
        }
        for (Vec2 c : d.control)
            o.pt(10, c);
    }
    for (Vec2 f : d.fit)
        o.pt(11, f);
    count("SPLINE");
}

// ---------------------------------------------------------------------------------------------
// Text

void Writer::writeText(const Props& p, const TextData& d, std::uint64_t owner) {
    writeTextRecord(p, d, owner, d.attdef ? "ATTDEF" : "TEXT", text::expandFields(doc_, d.utf8));
}

// TEXT, ATTDEF (inside a block) and ATTRIB (after an INSERT) share the text groups; only the
// record type, the string in group 1 and the attribute subclass differ.
void Writer::writeTextRecord(const Props& p, const TextData& d, std::uint64_t owner,
                             std::string_view type, const std::string& shown) {
    head(type, p, owner, "AcDbText");
    Out& o = *L().o;
    o.pt(10, d.position);
    o.d(40, d.height);
    o.str(1, type == "ATTDEF" ? d.attdef->defaultValue : shown);
    if (d.rotation != 0)
        o.d(50, d.rotation * kDeg);
    {
        // Group 41 is the object's full factor; the style's own is only the default for new text.
        const TextStyle* ts = doc_.textStyle(d.style);
        const double styleWf = ts ? styleWidthWritten(*ts) : 1.0;
        const double full = std::round(d.widthFactor * styleWf * 1e9) / 1e9;
        if (full != 1 || styleWf != 1)
            o.d(41, full);
    }
    if (d.oblique != 0)
        o.d(51, d.oblique * kDeg);
    o.str(7, textStyleNameOf(d.style));
    {
        const TextStyle* gts = doc_.textStyle(d.style);
        const bool back = d.backward != (gts && gts->backward);
        const bool down = d.upsideDown != (gts && gts->upsideDown);
        if (back || down)
            o.i(71, (back ? 2 : 0) | (down ? 4 : 0));
    }
    bool aligned = d.h != HAlign::Left || d.v != VAlign::Baseline;
    if (d.h != HAlign::Left)
        o.i(72, static_cast<int>(d.h));
    if (aligned)
        o.pt(11, d.alignPoint);
    if (type == "TEXT") {
        sub("AcDbText");
        if (d.v != VAlign::Baseline)
            o.i(73, static_cast<int>(d.v));
    } else {
        const AttDefInfo& a = *d.attdef;
        sub(type == "ATTDEF" ? "AcDbAttributeDefinition" : "AcDbAttribute");
        if (type == "ATTDEF")
            o.str(3, a.prompt);
        o.str(2, a.tag);
        o.i(70,
            (a.invisible ? 1 : 0) | (a.constant ? 2 : 0) | (a.verify ? 4 : 0) | (a.preset ? 8 : 0));
        if (d.v != VAlign::Baseline)
            o.i(74, static_cast<int>(d.v));
    }
    count(type);
}

void Writer::writeMText(const Props& p, const MTextData& d, std::uint64_t owner) {
    if (r12_) {
        downgrade("MTEXT", "has no R12 form; written as one TEXT per line");
        std::string plain = plainLines(text::expandFields(doc_, d.utf8));
        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (true) {
            std::size_t nl = plain.find('\n', pos);
            lines.push_back(plain.substr(pos, nl == std::string::npos ? nl : nl - pos));
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
        double step = 1.5 * d.height;
        Vec2 down{std::sin(d.rotation), -std::cos(d.rotation)};
        int col = static_cast<int>(d.attach) % 3;  // 0 left, 1 center, 2 right
        int row = static_cast<int>(d.attach) / 3;  // 0 top, 1 middle, 2 bottom
        double n = static_cast<double>(lines.size());
        for (std::size_t k = 0; k < lines.size(); ++k) {
            double shift = row == 0 ? -static_cast<double>(k) * step
                                    : (row == 1 ? ((n - 1) / 2 - static_cast<double>(k)) * step
                                                : (n - 1 - static_cast<double>(k)) * step);
            Vec2 at = d.position + down * (-shift);
            head("TEXT", p, owner, "AcDbText");
            Out& o = *L().o;
            o.pt(10, at);
            o.d(40, d.height);
            o.str(1, lines[k]);
            if (d.rotation != 0)
                o.d(50, d.rotation * kDeg);
            o.str(7, textStyleNameOf(d.style));
            o.i(72, col);
            o.pt(11, at);
            o.i(73, row == 0 ? 3 : (row == 1 ? 2 : 1));
            count("TEXT");
        }
        return;
    }
    head("MTEXT", p, owner, "AcDbMText");
    Out& o = *L().o;
    o.pt(10, d.position);
    o.d(40, d.height);
    o.d(41, d.width);
    o.i(71, static_cast<int>(d.attach) + 1);
    o.i(72, 1);
    std::vector<std::string> parts =
        chunks(encodeText(toDxfMarkup(text::expandFields(doc_, d.utf8)), utf8_), 250);
    for (std::size_t i = 0; i + 1 < parts.size(); ++i)
        o.raw(3, parts[i]);
    o.raw(1, parts.back());
    o.str(7, textStyleNameOf(d.style));
    if (d.rotation != 0)
        o.pt(11, {std::cos(d.rotation), std::sin(d.rotation)});
    if (d.lineSpacing != 1.0) {
        o.i(73, 1);  // at least: the line pitch follows the text
        o.d(44, d.lineSpacing);
    }
    if (d.bgMode) {
        o.i(90, d.bgMode == 2 ? 1 : 3);
        if (d.bgMode == 2)
            o.i(63, d.bgColor);
        o.d(45, d.bgScale);
    }
    if (d.columns > 1 && d.width > 0) {
        const bool fixed = d.columnHeight > 0;
        o.i(75, fixed ? 1 : 2);  // static columns, or dynamic ones of automatic height
        o.i(76, d.columns);
        o.i(79, fixed ? 0 : 1);
        o.d(48, d.width);
        o.d(49, d.columnGap > 0 ? d.columnGap : 2 * d.height);
        if (fixed)
            o.d(46, d.columnHeight);
    }
    count("MTEXT");
}

// ---------------------------------------------------------------------------------------------
// Inserts, leaders, proxies, solids

// The picture's size in pixels is the frame polyline of its block (pixel units, origin
// bottom-left).
bool Writer::writeImage(const Props& p, const InsertData& d, std::uint64_t owner) {
    const BlockDef* bd = doc_.block(d.block);
    if (!bd || bd->xrefPath.empty() || !underlay::isImagePath(bd->xrefPath))
        return false;
    if (r12_) {
        skipEntity("IMAGE", "this DXF version has no pictures");
        return true;
    }
    double w = 0, h = 0;
    for (EntityId id : bd->entities)
        if (const Entity* e = doc_.find(id))
            if (const auto* pl = entityAs<PolylineData>(*e))
                for (const PolyVertex& v : pl->v) {
                    w = std::max(w, v.p.x);
                    h = std::max(h, v.p.y);
                }
    if (!(w >= 1) || !(h >= 1)) {
        skipEntity("IMAGE", "the picture \"" + bd->name + "\" has no frame to take its size from");
        return true;
    }
    const underlay::ImageParams ip = underlay::imageParams(doc_.header(), bd->name);
    ImageOut img;
    img.name = bd->name;
    img.path = bd->xrefPath;
    img.w = w;
    img.h = h;
    img.frame = ip.frame != 0;
    img.def = newHandle();
    img.reactor = newHandle();
    img.image = L().entHandle ? L().entHandle : peekHandle();  // the handle head() gives the IMAGE
    const double c = std::cos(d.rotation), sn = std::sin(d.rotation);
    const Vec2 u{c * d.scale.x, sn * d.scale.x}, v{-sn * d.scale.y, c * d.scale.y};
    const Vec2 origin = d.position - u * bd->base.x - v * bd->base.y;
    head("IMAGE", p, owner, "AcDbRasterImage");
    Out& o = *L().o;
    o.i(90, 0);
    o.pt(10, origin);
    o.pt(11, u);
    o.pt(12, v);
    o.d(13, w);
    o.d(23, h);
    o.h(340, img.def);
    // shown, shown when not aligned, [clipped], [bitonal background clear]
    o.i(70, (ip.clipped() ? 7 : 3) | (ip.transparent ? 8 : 0));
    o.i(280, ip.clipped() ? 1 : 0);
    o.i(281, ip.brightness);
    o.i(282, ip.contrast);
    o.i(283, static_cast<long long>(std::lround(ip.fade)));
    o.h(360, img.reactor);
    // Clip corners are in pixels counted from the lower-left corner of the picture.
    auto px = [&](double u, double v) { return Vec2{u * w - 0.5, (1 - v) * h - 0.5}; };
    if (ip.clip.size() >= 3) {
        o.i(71, 2);
        o.i(91, static_cast<long long>(ip.clip.size()) + 1);
        for (const Vec2& cp : ip.clip)
            o.pt2(14, px(cp.x, cp.y));
        o.pt2(14, px(ip.clip.front().x, ip.clip.front().y));
    } else {
        o.i(71, 1);
        o.i(91, 2);
        o.pt2(14, px(ip.u0, ip.v1));
        o.pt2(14, px(ip.u1, ip.v0));
    }
    images_.push_back(std::move(img));
    count("IMAGE");
    return true;
}

// A PDF page is written as a PDFUNDERLAY that points at a PDFDEFINITION with the file and page.
bool Writer::writePdfUnderlay(const Props& p, const InsertData& d, std::uint64_t owner) {
    const BlockDef* bd = doc_.block(d.block);
    if (!bd || bd->xrefPath.empty() || !underlay::isPdfPath(bd->xrefPath))
        return false;
    if (r12_) {
        skipEntity("PDFUNDERLAY", "this DXF version has no underlays");
        return true;
    }
    const underlay::Spec spec = underlay::parseSpec(bd->xrefPath);
    // One definition per file and page, shared by every placement of it.
    const std::string page = std::to_string(spec.pdf.page);
    std::uint64_t def = 0;
    for (const PdfOut& o : pdfs_)
        if (o.file == spec.file && o.page == page)
            def = o.def;
    if (!def) {
        PdfOut out;
        out.name = bd->name;
        out.file = spec.file;
        out.page = page;
        out.def = def = newHandle();
        pdfs_.push_back(std::move(out));
    }
    head("PDFUNDERLAY", p, owner, "AcDbUnderlayReference");
    Out& o = *L().o;
    o.h(340, def);
    const double c = std::cos(d.rotation), sn = std::sin(d.rotation);
    const Vec2 u{c * d.scale.x, sn * d.scale.x}, v{-sn * d.scale.y, c * d.scale.y};
    o.pt(10, d.position - u * bd->base.x - v * bd->base.y);
    o.d(41, d.scale.x);
    o.d(42, d.scale.y);
    o.d(43, d.scale.x);
    o.d(50, d.rotation * 180.0 / std::numbers::pi);
    o.i(280, (spec.pdf.monochrome ? 4 : 0));
    o.i(281, spec.pdf.contrast);
    o.i(282, static_cast<long long>(std::lround(spec.pdf.fadePercent)));
    count("PDFUNDERLAY");
    return true;
}

// A feature control frame made by TOLERANCE (or read from a TOLERANCE) is written as one. The
// size comes from the dimension style (text height x overall scale), so the frame takes the
// current style and says so when that is not the size it was drawn at.
bool Writer::writeTolerance(const Props& p, const InsertData& d, std::uint64_t owner) {
    const BlockDef* bd = doc_.block(d.block);
    if (!bd)
        return false;
    const gdt::FrameTable& table = frames();
    auto it = table.find(bd->name);
    if (it == table.end())
        return false;
    if (r12_) {
        skipEntity("TOLERANCE", "this DXF version has no tolerance frames");
        return true;
    }
    const DimStyle& ds = dim::styleOf(doc_, doc_.header().currentDimStyle);
    const double have = ds.textHeight * (ds.overallScale > 0 ? ds.overallScale : 1.0);
    if (std::abs(have - gdt::kTextHeight * d.scale.x) > 1e-6 * std::max(1.0, have))
        downgrade("TOLERANCE", "is drawn by other programs at the size of the dimension style \"" +
                                   dimStyleNameOf(doc_.header().currentDimStyle) + "\"");
    head("TOLERANCE", p, owner, "AcDbFcf");
    Out& o = *L().o;
    o.str(3, dimStyleNameOf(doc_.header().currentDimStyle));
    o.pt(10, d.position);
    o.str(1, gdt::dxfText(it->second));
    o.pt(11, Vec2{std::cos(d.rotation), std::sin(d.rotation)});
    count("TOLERANCE");
    return true;
}

void Writer::writeInsert(const Props& p, const InsertData& d, std::uint64_t owner) {
    if (writeImage(p, d, owner) || writePdfUnderlay(p, d, owner) || writeTolerance(p, d, owner))
        return;
    std::string name = blockNameOf(d.block);
    if (name.empty()) {
        skipEntity("INSERT", "its block definition does not exist");
        return;
    }
    // An associative array has no DXF form: every item becomes an INSERT of its own.
    if (d.array) {
        const BlockDef* bd = doc_.block(d.block);
        const Vec2 base = bd ? bd->base : Vec2{};
        const Transform2 inst = blocks::instanceTransform(d, base);
        const bool uniform = std::abs(std::abs(d.scale.x) - std::abs(d.scale.y)) < 1e-9;
        const double flip = d.scale.x * d.scale.y < 0 ? -1.0 : 1.0;
        bool noted = false;
        const ArrayInfo& info = *d.array;
        const auto items = blocks::arrayItemTransforms(info);
        const int arrayId = nextArrayId_++;
        // The note with the array's parameters goes on the first item that shows the source
        // (without one the items stay plain inserts when read back).
        std::size_t anchor = items.size(), written = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!blocks::arrayItemSwap(info, static_cast<int>(i)) &&
                !blocks::arrayItemShowsSource(info, static_cast<int>(i)))
                continue;  // removed
            ++written;
            if (anchor == items.size() && blocks::arrayItemShowsSource(info, static_cast<int>(i)))
                anchor = i;
        }
        for (std::size_t index = 0; index < items.size(); ++index) {
            const Transform2& item = items[index];
            const ArrayItemSwap* swap = blocks::arrayItemSwap(info, static_cast<int>(index));
            if (!swap && !blocks::arrayItemShowsSource(info, static_cast<int>(index)))
                continue;
            InsertData one = d;
            one.array.reset();
            one.position = (inst * item).apply(base);
            if (swap) {
                // A replaced item: an insert of the replacement block, its base point on `at`.
                one = InsertData{};
                one.block = swap->block;
                one.scale = d.scale;
                one.position = (inst * item).apply(swap->at);
            }
            const double phi = std::atan2(item.m10, item.m00);
            one.rotation = d.rotation + flip * phi;
            auto text = [](int code, std::string s) {
                DataItem it;
                it.code = code;
                it.text = std::move(s);
                return it;
            };
            arrayTag_.clear();
            if (anchor < items.size())
                arrayTag_.push_back(text(1000, "a=" + std::to_string(arrayId) +
                                                   ";n=" + std::to_string(written) +
                                                   ";i=" + std::to_string(index)));
            if (index == anchor) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "dpos=%.17g,%.17g;drot=%.17g",
                              d.position.x - one.position.x, d.position.y - one.position.y,
                              d.rotation - one.rotation);
                arrayTag_.push_back(text(1000, buf));
                arrayTag_.push_back(text(1000, "info=" + blocks::arrayToText(*d.array)));
            }
            if (!uniform && std::abs(phi) > 1e-9 && !noted) {
                noted = true;
                downgrade("INSERT",
                          "an array with a stretched, rotated item is written with "
                          "approximate placement");
            }
            writeInsert(p, one, owner);
            arrayTag_.clear();
        }
        return;
    }
    // Non-constant attributes of the block become ATTRIB records after the INSERT.
    std::vector<const TextData*> defs;
    if (const BlockDef* bd = doc_.block(d.block))
        for (EntityId id : bd->entities)
            if (const Entity* e = doc_.find(id))
                if (const auto* t = entityAs<TextData>(*e); t && t->attdef && !t->attdef->constant)
                    defs.push_back(t);
    head("INSERT", p, owner, "AcDbBlockReference");
    const std::uint64_t insertHandle = L().lastHead;  // owns its ATTRIBs and SEQEND
    Out& o = *L().o;
    if (!defs.empty())
        o.i(66, 1);
    o.str(2, name);
    o.pt(10, d.position);
    o.d(41, d.scale.x);
    o.d(42, d.scale.y);
    o.d(43, 1);
    if (d.rotation != 0)
        o.d(50, d.rotation * kDeg);
    if (d.clip.size() >= 3) {
        // No DXF form for a clipping boundary: TBCAD restores it from XDATA (block coordinates).
        openTbcadXdata();
        o.str(1000, d.clipOn ? "clip=1" : "clip=0");
        if (d.clipInvert)
            o.str(1000, "clipinvert=1");
        for (const Vec2& v : d.clip)
            o.pt(1010, v);
        downgrade("INSERT",
                  "a clipping boundary is kept only in TBCAD XDATA; other programs "
                  "show the whole block");
    }
    if (L().cur)  // in the INSERT record itself, ahead of its ATTRIBs (see writeEntity)
        writeCommonFlags(*L().cur);
    if (!arrayTag_.empty()) {
        o.raw(1001, "TBCAD_ARRAY");
        for (const DataItem& it : arrayTag_)
            o.str(it.code, it.text);
    }
    if (L().cur)
        writeExtXdata(*L().cur);
    count("INSERT");
    if (defs.empty())
        return;
    const BlockDef* bd = doc_.block(d.block);
    const double c = std::cos(d.rotation), sn = std::sin(d.rotation);
    for (const TextData* def : defs) {
        // The definition's placement, carried through the insert transform. A non-uniform scale
        // cannot stretch text, so the height follows the Y scale.
        Vec2 shift{};  // the attribute's own grip offset, in block coordinates
        for (const AttribValue& v : d.attribs)
            if (v.tag == def->attdef->tag)
                shift = v.offset;
        Vec2 rel = def->position + shift - bd->base;
        rel = {rel.x * d.scale.x, rel.y * d.scale.y};
        TextData t = *def;
        t.position = d.position + Vec2{rel.x * c - rel.y * sn, rel.x * sn + rel.y * c};
        Vec2 al = def->alignPoint + shift - bd->base;
        al = {al.x * d.scale.x, al.y * d.scale.y};
        t.alignPoint = d.position + Vec2{al.x * c - al.y * sn, al.x * sn + al.y * c};
        t.height = def->height * std::abs(d.scale.y);
        t.rotation = def->rotation + d.rotation;
        std::string value;
        bool found = false;
        for (const AttribValue& v : d.attribs)
            if (v.tag == def->attdef->tag) {
                value = v.value;
                found = true;
                break;
            }
        if (!found)
            value = def->attdef->defaultValue;
        writeTextRecord(p, t, r12_ ? owner : insertHandle, "ATTRIB", value);
    }
    o.zero("SEQEND");
    if (!r12_) {
        o.h(5, newHandle());
        o.h(330, insertHandle);
        o.raw(100, "AcDbEntity");
    }
    o.str(8, p.layer);
}

void Writer::writeLeader(const Props& p, const LeaderData& d, std::uint64_t owner) {
    if (d.vertices.size() < 2) {
        skipEntity("LEADER", "fewer than two vertices");
        return;
    }
    if (r12_) {
        downgrade("LEADER", "has no R12 form; written as a polyline");
        std::vector<PolyVertex> v;
        for (Vec2 pt : d.vertices) {
            PolyVertex pv;
            pv.p = pt;
            v.push_back(pv);
        }
        writeVertexPolyline(p, v, false, 0, owner);
    } else {
        head("LEADER", p, owner, "AcDbLeader");
        Out& o = *L().o;
        o.str(3, dimStyleNameOf(d.style));
        o.i(71, 1);
        o.i(72, 0);
        o.i(73, 3);  // no annotation: the text is a separate entity
        o.i(74, 1);
        o.i(75, 0);
        o.d(40, 0);
        o.d(41, 0);
        o.i(76, static_cast<long long>(d.vertices.size()));
        for (Vec2 pt : d.vertices)
            o.pt(10, pt);
        o.d(210, 0);
        o.d(220, 0);
        o.d(230, 1);
        o.pt(211, {1, 0});
        o.pt(212, {0, 0});
        o.pt(213, {0, 0});
        writeDimExtras(d.breaks, d.breakWith, d.autoBreaks, nullptr, &d);
        count("LEADER");
    }
    if (!d.utf8.empty()) {
        MTextData text;
        text.position = d.textPos;
        const DimStyle* ds = doc_.dimStyle(d.style ? d.style : doc_.header().currentDimStyle);
        text.height = ds ? ds->textHeight : 2.5;
        text.attach = Attachment::MiddleLeft;
        text.utf8 = d.utf8;
        writeMText(p, text, owner);
    }
}

void Writer::writeProxy(const Props& p, const ProxyData& d, std::uint64_t owner) {
    if (d.originalType == "ACAD_PROXY_ENTITY" && !d.raw.empty() && !r12_) {
        head("ACAD_PROXY_ENTITY", p, owner, "AcDbProxyEntity");
        Out& o = *L().o;
        o.i(90, 498);
        o.i(91, 500);
        o.i(95, 0);
        o.i(70, 0);
        o.i(92, static_cast<long long>(d.raw.size()));
        static const char* digits = "0123456789ABCDEF";
        for (std::size_t pos = 0; pos < d.raw.size(); pos += 127) {
            std::string hex;
            for (std::size_t i = pos; i < std::min(pos + 127, d.raw.size()); ++i) {
                hex.push_back(digits[d.raw[i] >> 4]);
                hex.push_back(digits[d.raw[i] & 15]);
            }
            o.raw(310, hex);
        }
        o.i(93, 0);
        count("ACAD_PROXY_ENTITY");
        return;
    }
    // An entity this program cannot read, written back as it was read.
    if (const std::string_view groups = proxyDxfGroups(d);
        !groups.empty() && !r12_ && d.originalType != "ACAD_PROXY_ENTITY") {
        head(d.originalType, p, owner, "");
        for (const auto& [code, value] : unpackGroups(groups))
            L().o->str(code, value);
        count(d.originalType);
        return;
    }
    if (!d.graphics.items.empty()) {
        writeDisplayList(d.graphics, p.layer, owner);
        return;
    }
    skipEntity("PROXY", "unknown object \"" + d.originalType + "\" has no graphics to write");
}

void Writer::writeSolid(const Props& p, const std::vector<Vec2>& ring, std::uint64_t owner) {
    head("SOLID", p, owner, "AcDbTrace");
    Out& o = *L().o;
    Vec2 p1 = ring[0], p2 = ring[1], p3 = ring.size() == 4 ? ring[3] : ring[2],
         p4 = ring.size() == 4 ? ring[2] : ring[2];
    o.pt(10, p1);
    o.pt(11, p2);
    o.pt(12, p3);
    o.pt(13, p4);
    count("SOLID");
}

void Writer::writeDisplayList(const DisplayList& list, const std::string& fallbackLayer,
                              std::uint64_t owner) {
    for (const DisplayItem& item : list.items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
            if (pl->pts.size() < 2)
                continue;
            Props p = propsOfStyle(pl->style, fallbackLayer);
            std::vector<PolyVertex> v;
            v.reserve(pl->pts.size());
            for (Vec2 pt : pl->pts) {
                PolyVertex pv;
                pv.p = pt;
                v.push_back(pv);
            }
            PolylineData data;
            data.v = std::move(v);
            data.closed = pl->closed;
            writePolyline(p, data, owner);
        } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
            Props p = propsOfStyle(fill->style, fallbackLayer);
            // A ring may repeat its first point at the end; DXF closes the loop by itself.
            std::vector<std::vector<Vec2>> rings = fill->rings;
            for (auto& ring : rings) {
                if (ring.size() > 3 && ring.front() == ring.back())
                    ring.pop_back();
            }
            if (rings.size() == 1 && (rings[0].size() == 3 || rings[0].size() == 4)) {
                writeSolid(p, rings[0], owner);
            } else if (!rings.empty()) {
                if (r12_) {
                    downgrade("HATCH", "has no R12 form; the filled area is written as outlines");
                    for (const auto& ring : rings) {
                        std::vector<PolyVertex> v;
                        for (Vec2 pt : ring) {
                            PolyVertex pv;
                            pv.p = pt;
                            v.push_back(pv);
                        }
                        if (v.size() >= 2)
                            writeVertexPolyline(p, v, true, 0, owner);
                    }
                } else {
                    writeSolidHatch(p, rings, owner);
                }
            }
        } else if (const auto* pts = std::get_if<DisplayPoints>(&item)) {
            Props p = propsOfStyle(pts->style, fallbackLayer);
            for (Vec2 pt : pts->pts)
                writePoint(p, PointData{pt}, owner);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Dimensions

std::size_t Writer::writeDimension(const std::vector<EntityId>& ids, std::size_t index,
                                   std::uint64_t owner) {
    const Entity* e = doc_.find(ids[index]);
    const DimensionData& d = *entityAs<DimensionData>(*e);
    if (d.kind == DimKind::ArcLength || d.kind == DimKind::Jogged) {
        // DXF's arc dimension is a separate entity we do not write; keep what it draws.
        downgrade("DIMENSION",
                  "arc length and jogged dimensions are written as plain lines, arcs and text");
        if (auto parts = dim::explodeDimension(doc_, *e))
            for (const Entity& part : parts.value())
                writeEntity(part, owner);
        return 1;
    }
    Props p = propsOf(e->common);

    // The anonymous *D block is generated from the dimension generator, so every dimension
    // carries drawn graphics that other programs can show without regenerating anything.
    const dim::Layout layout = dim::layoutDimension(doc_, d);
    DimBlock block;
    do {
        block.name = "*D" + std::to_string(++dimCounter_);
    } while (blockSet_.has(block.name));
    blockSet_.add(block.name);
    block.rec = newHandle();
    {
        Out bo(utf8_, r12_);
        Out* saved = L().o;
        L().o = &bo;
        auto parts = dim::explodeDimension(doc_, *e);
        if (parts) {
            for (const Entity& part : parts.value())
                writeEntity(part, block.rec);
        }
        L().o = saved;
        block.body = std::move(bo.buf);
    }
    std::string blockName = block.name;
    dimBlocks_.push_back(std::move(block));
    pickEntityHandle(ids[index]);

    int type = 0;
    switch (d.kind) {
        case DimKind::Linear:
            type = 0;
            break;
        case DimKind::Aligned:
            type = 1;
            break;
        case DimKind::Angular2Line:
            type = 2;
            break;
        case DimKind::Diameter:
            type = 3;
            break;
        case DimKind::Radius:
            type = 4;
            break;
        case DimKind::Angular3Point:
            type = 5;
            break;
        case DimKind::Jogged:
            break;
        case DimKind::Ordinate:
            type = 6 | (d.rotation < kPi / 4 ? 64 : 0);
            break;
        case DimKind::ArcLength:
            break;
    }
    if (!r12_)
        type |= 32;
    if (d.textPosUser)
        type |= 128;

    const char* subclass = "AcDbAlignedDimension";
    switch (d.kind) {
        case DimKind::Radius:
            subclass = "AcDbRadialDimension";
            break;
        case DimKind::Diameter:
            subclass = "AcDbDiametricDimension";
            break;
        case DimKind::Angular2Line:
            subclass = "AcDb2LineAngularDimension";
            break;
        case DimKind::Angular3Point:
            subclass = "AcDb3PointAngularDimension";
            break;
        case DimKind::Ordinate:
            subclass = "AcDbOrdinateDimension";
            break;
        default:
            break;
    }

    setLevelTag(e->common);
    head("DIMENSION", p, owner, "AcDbDimension");
    Out& o = *L().o;
    o.str(2, blockName);
    switch (d.kind) {
        case DimKind::Linear:
        case DimKind::Aligned:
            o.pt(10, d.defPts[2]);
            break;
        case DimKind::Radius:
        case DimKind::Diameter:
            o.pt(10, d.defPts[0]);
            break;
        case DimKind::Ordinate:
            o.pt(10, d.defPts[2]);
            break;
        default:
            o.pt(10, d.defPts[3]);
            break;
    }
    // The text point: where the text was placed (for text moved with a leader that is where the
    // text starts, as AutoCAD writes it), else the middle of the text TBCAD draws.
    const bool placed =
        d.textPosUser && d.kind != DimKind::Angular2Line;  // its textPos is on the arc
    o.pt(11, placed || !layout.text.valid() ? d.textPos : layout.text.point);
    o.i(70, type | (d.textPosUser ? 128 : 0));
    o.i(71, 5);
    if (!d.textOverride.empty())
        o.str(1, text::expandFields(doc_, d.textOverride));
    o.str(3, dimStyleNameOf(d.style));
    o.d(42, dim::measurement(d));
    if (d.kind == DimKind::Ordinate && d.oblique != 0)
        o.d(51, d.oblique * kDeg);  // the X axis the ordinate is measured along (the UCS)
    if (d.textRotated)
        o.d(53, d.textRotation * kDeg);  // text turned by hand: its absolute angle
    sub(subclass);
    switch (d.kind) {
        case DimKind::Linear:
        case DimKind::Aligned: {
            o.pt(13, d.defPts[0]);
            o.pt(14, d.defPts[1]);
            if (d.kind == DimKind::Linear)
                o.d(50, d.rotation * kDeg);
            if (d.oblique != 0) {
                // The angle of the extension lines from the X axis (DIMEDIT Oblique).
                const Vec2 along = d.defPts[1] - d.defPts[0];
                const double dimAngle =
                    d.kind == DimKind::Linear ? d.rotation : std::atan2(along.y, along.x);
                const double deg = normalizeAngle(dimAngle + kPi / 2 + d.oblique) * kDeg;
                o.d(52, std::round(deg * 1e9) / 1e9);  // 60, not 59.99999999999999
            }
            if (d.kind == DimKind::Linear)
                sub("AcDbRotatedDimension");
            break;
        }
        case DimKind::Radius:
        case DimKind::Diameter:
            o.pt(15, d.defPts[1]);
            o.d(40, 0);
            break;
        case DimKind::Angular2Line:
            o.pt(13, d.defPts[0]);
            o.pt(14, d.defPts[1]);
            o.pt(15, d.defPts[2]);
            o.pt(16, d.textPos);  // a point on the dimension arc
            break;
        case DimKind::Angular3Point:
            o.pt(13, d.defPts[0]);
            o.pt(14, d.defPts[1]);
            o.pt(15, d.defPts[2]);
            break;
        case DimKind::Ordinate:
            o.pt(13, d.defPts[0]);
            o.pt(14, d.defPts[1]);
            break;
        case DimKind::ArcLength:
        case DimKind::Jogged:
            break;
    }
    count("DIMENSION");
    return 1;
}

}  // namespace tbcad::dxf::detail
