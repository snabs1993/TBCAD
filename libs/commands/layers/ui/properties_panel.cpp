#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cmath>
#include <cstdio>
#include <map>

#include "panel_util.h"
#include "panels.h"
#include "tbcad/blocks/assembly.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/properties.h"

namespace tbcad::layers {

namespace {

using namespace ui_detail;

// What the panel shows, recomputed only when the selection, the type filter or the document
// changed (not every frame).
struct Cache {
    std::uint64_t revision = ~0ull;
    std::vector<EntityId> selection{};
    int filterIndex = 0;  // 0 = all, otherwise index into kinds
    CommonProps common{};
    std::vector<EntityKind> kinds{};
    std::optional<EntityKind> only{};
    // Geometry: per field id, label, type, editable, and whether the value varies.
    struct Field {
        GeometryField field{};
        bool varies = false;
    };
    std::vector<Field> fields{};
    std::size_t pickedIndex = 0;  // entities shown in the Geometry section
    std::vector<EntityId> shown{};
};

Cache& cache() {
    static Cache c;
    return c;
}

// Text being edited; keeps the buffer between frames while an input is active.
struct EditState {
    ImGuiID active = 0;
    std::string buffer{};
    std::string original{};
};
EditState& edit() {
    static EditState e;
    return e;
}

std::string formatNumber(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// A text input that applies on Enter or when it loses focus after an edit. Returns the new text
// when the user committed one that differs from what was shown.
std::optional<std::string> editableText(const char* id, const std::string& shown,
                                        float width = -1) {
    EditState& es = edit();
    ImGui::SetNextItemWidth(width);
    ImGuiID imId = ImGui::GetID(id);
    std::string text = es.active == imId ? es.buffer : shown;
    ImGui::InputText(id, &text, ImGuiInputTextFlags_AutoSelectAll);
    std::optional<std::string> result;
    if (ImGui::IsItemActivated()) {
        es.active = imId;
        es.original = shown;
    }
    if (ImGui::IsItemActive()) {
        es.buffer = text;
    } else if (es.active == imId) {
        if (ImGui::IsItemDeactivated() && es.buffer != es.original)
            result = es.buffer;
        es.active = 0;
    }
    return result;
}

void refresh(tbcad::ui::PanelContext& ctx) {
    Cache& c = cache();
    const Document& doc = *ctx.doc;
    const std::vector<EntityId>& sel = *ctx.selection;
    if (c.revision == doc.revision() && c.selection == sel)
        return;
    c.revision = doc.revision();
    c.selection = sel;
    c.common = readCommon(doc, sel);
    c.kinds.clear();
    for (const auto& [k, n] : c.common.kinds)
        c.kinds.push_back(k);
    if (c.filterIndex > static_cast<int>(c.kinds.size()))
        c.filterIndex = 0;
    c.only = c.filterIndex == 0
                 ? std::nullopt
                 : std::optional<EntityKind>(c.kinds[static_cast<std::size_t>(c.filterIndex - 1)]);
    c.common = readCommon(doc, sel, c.only);

    // Geometry fields: shown when exactly one kind is in play.
    c.fields.clear();
    c.shown.clear();
    std::optional<EntityKind> kindInPlay = c.only;
    if (!kindInPlay && c.kinds.size() == 1)
        kindInPlay = c.kinds[0];
    if (kindInPlay) {
        for (EntityId id : sel) {
            const Entity* e = doc.find(id);
            if (!e || kind(*e) != *kindInPlay)
                continue;
            c.shown.push_back(id);
            auto fs = geometryFields(*e);
            if (c.fields.empty()) {
                for (auto& f : fs)
                    c.fields.push_back({f, false});
            } else {
                for (std::size_t i = 0; i < fs.size() && i < c.fields.size(); ++i)
                    if (std::fabs(fs[i].value - c.fields[i].field.value) > 1e-9)
                        c.fields[i].varies = true;
            }
        }
    }
}

std::string selectionLabel(const Cache& c, int index) {
    if (index == 0) {
        std::size_t total = 0;
        for (const auto& [k, n] : c.common.kinds)
            total += n;
        return "All (" + std::to_string(total) + ")";
    }
    EntityKind k = c.kinds[static_cast<std::size_t>(index - 1)];
    return std::string(kindLabel(k)) + " (" + std::to_string(c.common.kinds.at(k)) + ")";
}

void generalSection(tbcad::ui::PanelContext& ctx, Cache& c) {
    const Document& doc = *ctx.doc;
    const CommonProps& p = c.common;
    auto prop = [&](const std::string& keyword, const std::string& value) {
        // "-PROP" takes the selection from the host; an optional type narrows it first. The value
        // goes on its own line so it may contain spaces ("2 * 3").
        std::string first = "-PROP";
        if (c.only)
            first += " Only " + std::string(kindLabel(*c.only));
        exec(ctx, {first + " " + keyword, value});
    };

    if (!ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (ImGui::BeginTable("##general", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

        // Layer.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Layer");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        std::string layerText = p.layer.varies ? "*varies*" : layerPath(doc, p.layer.value);
        if (ImGui::BeginCombo("##layer", layerText.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (LayerId id : layerTreeOrder(doc)) {
                std::string label(static_cast<std::size_t>(layerDepth(doc, id)) * 2, ' ');
                label += doc.layer(id)->name;
                if (ImGui::Selectable(label.c_str(), !p.layer.varies && p.layer.value == id))
                    prop("Layer", layerPath(doc, id));
            }
            ImGui::EndCombo();
        }

        // Colour.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Colour");
        ImGui::TableSetColumnIndex(1);
        if (p.color.varies) {
            if (ImGui::Button("*varies*", ImVec2(-1, 0)))
                ImGui::OpenPopup("##colour");
        } else {
            Color shown = p.color.value;
            std::string label = colorName(shown);
            ImVec4 sw = swatch(shown.kind == ColorKind::ByLayer || shown.kind == ColorKind::ByBlock
                                   ? Color::index(7)
                                   : shown);
            if (ImGui::ColorButton("##sw", sw,
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                                   ImVec2(22, ImGui::GetFrameHeight() - 4)))
                ImGui::OpenPopup("##colour");
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
        }
        {
            ColorChoices allowed;
            allowed.byLayer = true;
            allowed.byBlock = true;
            std::string chosen;
            if (colorPopup("##colour", p.color.varies ? Color::byLayer() : p.color.value, allowed,
                           chosen))
                prop("Color", chosen);
        }

        // Linetype.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Linetype");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        std::string ltText = p.linetype.varies ? "*varies*" : linetypeName(doc, p.linetype.value);
        if (ImGui::BeginCombo("##lt", ltText.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("ByLayer",
                                  !p.linetype.varies && p.linetype.value == kLinetypeByLayer))
                prop("Linetype", "ByLayer");
            if (ImGui::Selectable("ByBlock",
                                  !p.linetype.varies && p.linetype.value == kLinetypeByBlock))
                prop("Linetype", "ByBlock");
            for (const auto& [id, lt] : doc.linetypes())
                if (ImGui::Selectable(lt.name.c_str(),
                                      !p.linetype.varies && p.linetype.value == id))
                    prop("Linetype", lt.name);
            ImGui::EndCombo();
        }

        // Linetype scale.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Linetype scale");
        ImGui::TableSetColumnIndex(1);
        if (auto t = editableText("##ltscale", p.linetypeScale.varies
                                                   ? "*varies*"
                                                   : formatNumber(p.linetypeScale.value)))
            prop("LTScale", *t);

        // Lineweight.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Lineweight");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        std::string lwText = p.lineweight.varies ? "*varies*" : lineweightName(p.lineweight.value);
        if (ImGui::BeginCombo("##lw", lwText.c_str(), ImGuiComboFlags_HeightLarge)) {
            static const char* const kWeights[] = {"ByLayer", "ByBlock", "Default", "0.05", "0.09",
                                                   "0.13",    "0.15",    "0.18",    "0.20", "0.25",
                                                   "0.30",    "0.35",    "0.40",    "0.50", "0.60",
                                                   "0.70",    "0.80",    "1.00",    "1.40", "2.00"};
            for (const char* w : kWeights)
                if (ImGui::Selectable(w, false))
                    prop("Lineweight", w);
            ImGui::EndCombo();
        }
        ImGui::EndTable();
    }
}

// With nothing selected the panel shows what new objects get: the current colour, linetype and
// lineweight (-CURRENT). Layer stays in the Layers panel.
void currentSection(tbcad::ui::PanelContext& ctx) {
    const Document& doc = *ctx.doc;
    const Header& h = doc.header();
    auto set = [&](const std::string& keyword, const std::string& value) {
        exec(ctx, {"-CURRENT " + keyword, value});
    };
    ImGui::TextDisabled("Nothing selected. New objects get:");
    if (!ImGui::BeginTable("##current", 2, ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Colour");
    ImGui::TableSetColumnIndex(1);
    {
        Color shown = h.currentColor;
        ImVec4 sw = swatch(shown.kind == ColorKind::ByLayer || shown.kind == ColorKind::ByBlock
                               ? Color::index(7)
                               : shown);
        if (ImGui::ColorButton("##cursw", sw,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                               ImVec2(22, ImGui::GetFrameHeight() - 4)))
            ImGui::OpenPopup("##curcolour");
        ImGui::SameLine();
        ImGui::TextUnformatted(colorName(shown).c_str());
        ColorChoices allowed;
        allowed.byLayer = true;
        allowed.byBlock = true;
        std::string chosen;
        if (colorPopup("##curcolour", shown, allowed, chosen))
            set("Color", chosen);
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Linetype");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##curlt", linetypeName(doc, h.currentLinetype).c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("ByLayer", h.currentLinetype == kLinetypeByLayer))
            set("Linetype", "ByLayer");
        if (ImGui::Selectable("ByBlock", h.currentLinetype == kLinetypeByBlock))
            set("Linetype", "ByBlock");
        for (const auto& [id, lt] : doc.linetypes())
            if (ImGui::Selectable(lt.name.c_str(), h.currentLinetype == id))
                set("Linetype", lt.name);
        ImGui::EndCombo();
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Lineweight");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##curlw", lineweightName(h.currentLineweight).c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        static const char* const kWeights[] = {"ByLayer", "ByBlock", "Default", "0.05", "0.09",
                                               "0.13",    "0.15",    "0.18",    "0.20", "0.25",
                                               "0.30",    "0.35",    "0.40",    "0.50", "0.60",
                                               "0.70",    "0.80",    "1.00",    "1.40", "2.00"};
        for (const char* w : kWeights)
            if (ImGui::Selectable(w, false))
                set("Lineweight", w);
        ImGui::EndCombo();
    }
    ImGui::EndTable();
}

void geometrySection(tbcad::ui::PanelContext& ctx, Cache& c) {
    if (c.fields.empty())
        return;
    if (!ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    std::optional<EntityKind> kindInPlay = c.only;
    if (!kindInPlay && c.kinds.size() == 1)
        kindInPlay = c.kinds[0];
    if (ImGui::BeginTable("##geometry", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& f : c.fields) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(f.field.label.c_str());
            if (f.field.type == FieldType::Number) {  // lengths are millimetres, areas mm²
                ImGui::SameLine(0, 4);
                ImGui::TextDisabled(
                    "%s", f.field.id.find("area") != std::string::npos ? "mm\xC2\xB2" : "mm");
            }
            ImGui::TableSetColumnIndex(1);
            std::string text;
            if (f.varies) {
                text = "*varies*";
            } else if (f.field.type == FieldType::Flag) {
                text = f.field.value != 0 ? "Yes" : "No";
            } else if (f.field.type == FieldType::Count) {
                text = std::to_string(static_cast<long long>(f.field.value));
            } else {
                text = formatNumber(f.field.value);
            }
            ImGui::PushID(f.field.id.c_str());
            if (!f.field.editable || f.field.type == FieldType::Count) {
                ImGui::TextDisabled("%s", text.c_str());
            } else if (f.field.type == FieldType::Flag) {
                bool v = f.field.value != 0;
                if (ImGui::Checkbox("##flag", &v)) {
                    std::string first = "-PROP";
                    if (kindInPlay)
                        first += " Only " + std::string(kindLabel(*kindInPlay));
                    exec(ctx, {first + " Set " + f.field.id, v ? "Yes" : "No"});
                }
            } else if (auto t = editableText("##v", text)) {
                std::string first = "-PROP";
                if (kindInPlay)
                    first += " Only " + std::string(kindLabel(*kindInPlay));
                exec(ctx, {first + " Set " + f.field.id, *t});
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Parameters of the selected parametric assembly: one row per parameter, edits go through
// -ASMSET so that one edit is one undo step. Shown when every selected insert is of the same
// assembly block.
void assemblySection(tbcad::ui::PanelContext& ctx, Cache& c) {
    const Document& doc = *ctx.doc;
    const BlockDef* def = nullptr;
    const InsertData* first = nullptr;
    for (EntityId id : c.selection) {
        const Entity* e = doc.find(id);
        const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
        const BlockDef* d = ins ? doc.block(ins->block) : nullptr;
        if (!d || !d->assembly)
            return;
        if (def && def != d)
            return;
        def = d;
        if (!first)
            first = ins;
    }
    if (!def || !first)
        return;
    const AssemblyDef& ad = *def->assembly;
    if (!ImGui::CollapsingHeader(("Assembly: " + def->name).c_str(),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const auto values = blocks::asmEffectiveValues(ad, *first);
    auto set = [&](const std::string& name, const std::string& value) {
        exec(ctx, {"-ASMSET " + name, value});
    };
    if (!ad.presets.empty()) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##preset", "Preset...")) {
            for (const AsmPreset& pr : ad.presets)
                if (ImGui::Selectable(pr.name.c_str(), false))
                    set("@preset", pr.name);
            ImGui::EndCombo();
        }
    }
    if (ImGui::BeginTable("##assembly", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
        for (std::size_t i = 0; i < ad.params.size(); ++i) {
            const AsmParam& p = ad.params[i];
            const AsmValue& v = values[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(static_cast<int>(i));
            if (p.kind == AsmParam::Kind::Choice) {
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##choice", v.text.c_str())) {
                    for (const std::string& ch : p.choices)
                        if (ImGui::Selectable(ch.c_str(), ch == v.text))
                            set(p.name, ch);
                    ImGui::EndCombo();
                }
            } else {
                const bool text = p.kind == AsmParam::Kind::Text;
                if (auto t = editableText("##v", text ? v.text : formatNumber(v.number)))
                    set(p.name, *t);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!ad.joinClass.empty()) {
        bool joined = !first->joinOff;
        if (ImGui::Checkbox(("Join with other \"" + ad.joinClass + "\"").c_str(), &joined))
            if (ctx.runCommand)
                ctx.runCommand(joined ? "JOINASM" : "UNJOINASM");
    }
}

}  // namespace

void drawPropertiesPanel(tbcad::ui::PanelContext& ctx) {
    if (!ctx.doc || !ctx.selection)
        return;
    if (ctx.selection->empty()) {
        currentSection(ctx);
        return;
    }
    refresh(ctx);
    Cache& c = cache();

    // Header: type dropdown to narrow what the edits apply to.
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Delete").x -
                            3 * ImGui::GetStyle().FramePadding.x - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##type", selectionLabel(c, c.filterIndex).c_str())) {
        for (int i = 0; i <= static_cast<int>(c.kinds.size()); ++i) {
            if (ImGui::Selectable(selectionLabel(c, i).c_str(), c.filterIndex == i)) {
                c.filterIndex = i;
                c.revision = ~0ull;  // recompute
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        std::string first = "-PROP";
        if (c.only)
            first += " Only " + std::string(kindLabel(*c.only));
        exec(ctx, {first + " Delete"});
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(c.only ? "Erase the selected objects of this type"
                                 : "Erase every selected object");
    if (c.common.count == 0) {
        ImGui::TextDisabled("No objects of this type");
        return;
    }
    generalSection(ctx, c);
    assemblySection(ctx, c);
    geometrySection(ctx, c);

    // Sections other modules registered for the kind in play (text, dimensions, hatch ...),
    // for a single selected object.
    if (ctx.selection->size() == 1 && ctx.drawPropertySections)
        ctx.drawPropertySections(ctx.selection->front());
    else if (ctx.selection->size() > 1 && ctx.drawMultiSections)
        ctx.drawMultiSections();  // sections that can edit several objects at once
}

}  // namespace tbcad::layers
