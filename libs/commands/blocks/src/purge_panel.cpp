// The Purge dialog (PURGE): the unused layers, blocks, text styles, linetypes and dimension
// styles, with a picture of each block, plus zero-length objects, empty text and orphaned data.
// What is checked is removed by the hidden command _PURGEITEMS, so it is one undo step.
#include <map>
#include <string>
#include <vector>

#include "commands.h"
#include "tbcad/block_cmds/palette.h"
#include "tbcad/select/select.h"

#ifdef TBCAD_BLOCK_PANEL_IMGUI
#include <imgui.h>
#endif

namespace tbcad::block_cmds {

#ifdef TBCAD_BLOCK_PANEL_IMGUI

namespace {

using select::PurgeItem;
using select::PurgeKind;

constexpr const char* kNestedSetting = "purge.nested";

struct PurgeState {
    std::uint64_t revision = ~0ull;
    const Document* doc = nullptr;
    bool nested = true;
    std::vector<PurgeItem> items{};
    std::map<std::string, bool> checked{};  // by key(item); missing = checked
    std::vector<std::string> inUse[5]{};    // names that cannot go, per kind
    std::size_t zero = 0, empty = 0, orphaned = 0;
    bool doZero = false, doEmpty = false, doOrphaned = false;
    bool showInUse = false;
    int previewBlock = -1;  // id of the block whose picture is shown
    Thumbnail preview{};
    int previewOf = -1;
};

PurgeState& purgeState() {
    static PurgeState s;
    return s;
}

const char* kindTitle(PurgeKind k) {
    switch (k) {
        case PurgeKind::Layers:
            return "Layers";
        case PurgeKind::Blocks:
            return "Blocks";
        case PurgeKind::TextStyles:
            return "Text styles";
        case PurgeKind::Linetypes:
            return "Linetypes";
        case PurgeKind::DimStyles:
            return "Dimension styles";
        default:
            return "";
    }
}

char kindLetter(PurgeKind k) {
    switch (k) {
        case PurgeKind::Layers:
            return 'L';
        case PurgeKind::Blocks:
            return 'B';
        case PurgeKind::TextStyles:
            return 'T';
        case PurgeKind::Linetypes:
            return 'Y';
        default:
            return 'D';
    }
}

int kindIndex(PurgeKind k) {
    switch (k) {
        case PurgeKind::Layers:
            return 0;
        case PurgeKind::Blocks:
            return 1;
        case PurgeKind::TextStyles:
            return 2;
        case PurgeKind::Linetypes:
            return 3;
        default:
            return 4;
    }
}

constexpr PurgeKind kKinds[] = {PurgeKind::Layers, PurgeKind::Blocks, PurgeKind::TextStyles,
                                PurgeKind::Linetypes, PurgeKind::DimStyles};

std::string keyOf(const PurgeItem& it) {
    return kindLetter(it.kind) + std::to_string(it.id);
}

void refresh(PurgeState& st, Document& doc) {
    st.items = select::purgeableItems(doc, st.nested);
    st.zero = select::zeroLengthObjects(doc).size();
    st.empty = select::emptyTextObjects(doc).size();
    st.orphaned = select::orphanedDataKeys(doc).size();
    for (auto& v : st.inUse)
        v.clear();
    auto gone = [&](PurgeKind k, std::uint32_t id) {
        for (const auto& it : st.items)
            if (it.kind == k && it.id == id)
                return true;
        return false;
    };
    for (const auto& [id, row] : doc.layers())
        if (!gone(PurgeKind::Layers, id.value))
            st.inUse[0].push_back(row.name);
    for (const auto& [id, row] : doc.blocks())
        if (!row.anonymous && !gone(PurgeKind::Blocks, id.value))
            st.inUse[1].push_back(row.name);
    for (const auto& [id, row] : doc.textStyles())
        if (!gone(PurgeKind::TextStyles, id.value))
            st.inUse[2].push_back(row.name);
    for (const auto& [id, row] : doc.linetypes())
        if (!gone(PurgeKind::Linetypes, id.value))
            st.inUse[3].push_back(row.name);
    for (const auto& [id, row] : doc.dimStyles())
        if (!gone(PurgeKind::DimStyles, id.value))
            st.inUse[4].push_back(row.name);
    st.revision = doc.revision();
    st.doc = &doc;
}

void drawPicture(const Thumbnail& t, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(at, ImVec2(at.x + size, at.y + size), ImGui::GetColorU32(ImGuiCol_FrameBg));
    if (!t.empty()) {
        const float inner = size - 8;
        const float ox = at.x + 4 + (inner - static_cast<float>(t.width) * inner) / 2;
        const float oy = at.y + 4 + (inner + static_cast<float>(t.height) * inner) / 2;
        std::vector<ImVec2> pts;
        for (const auto& stroke : t.strokes) {
            pts.clear();
            for (Vec2 p : stroke)
                pts.emplace_back(ox + static_cast<float>(p.x) * inner,
                                 oy - static_cast<float>(p.y) * inner);
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                            ImGui::GetColorU32(ImGuiCol_Text), ImDrawFlags_None, 1.0f);
        }
    }
    ImGui::Dummy(ImVec2(size, size));
}

void run(ui::PanelContext& ctx, const std::string& spec) {
    if (!ctx.runCommand || spec.empty())
        return;
    if (ctx.cancelCommand)
        ctx.cancelCommand();
    ctx.host->setSetting("tmp.purge.items", spec);
    ctx.runCommand("_PURGEITEMS");
}

void drawPurgePanel(ui::PanelContext& ctx) {
    if (!ctx.doc || !ctx.host)
        return;
    PurgeState& st = purgeState();
    const bool nested = ctx.host->setting(kNestedSetting).value_or("1") != "0";
    // Worked out in a transaction that is thrown away: never while a command has one open.
    if (!ctx.commandActive &&
        (st.doc != ctx.doc || st.revision != ctx.doc->revision() || st.nested != nested)) {
        st.nested = nested;
        refresh(st, *ctx.doc);
    }

    if (ImGui::RadioButton("Items you can purge", !st.showInUse))
        st.showInUse = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Items in use", st.showInUse))
        st.showInUse = true;

    const float pictureSize = 96;
    const float listH = std::max(140.0f, ImGui::GetContentRegionAvail().y - 150.0f);
    ImGui::BeginChild("##purgetree",
                      ImVec2(ImGui::GetContentRegionAvail().x - pictureSize - 12, listH), true);
    for (PurgeKind k : kKinds) {
        const int ki = kindIndex(k);
        if (st.showInUse) {
            const auto& names = st.inUse[ki];
            const std::string head =
                std::string(kindTitle(k)) + " (" + std::to_string(names.size()) + ")";
            if (ImGui::TreeNode(head.c_str())) {
                for (const auto& n : names)
                    ImGui::BulletText("%s", n.c_str());
                ImGui::TreePop();
            }
            continue;
        }
        std::vector<const PurgeItem*> rows;
        for (const auto& it : st.items)
            if (it.kind == k)
                rows.push_back(&it);
        const std::string head = std::string(kindTitle(k)) + " (" + std::to_string(rows.size()) +
                                 ")##" + std::to_string(ki);
        ImGuiTreeNodeFlags flags = rows.empty() ? ImGuiTreeNodeFlags_Leaf : 0;
        if (!rows.empty())
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (ImGui::TreeNodeEx(head.c_str(), flags)) {
            for (const PurgeItem* it : rows) {
                const std::string key = keyOf(*it);
                auto found = st.checked.find(key);
                bool on = found == st.checked.end() || found->second;
                ImGui::PushID(key.c_str());
                if (ImGui::Checkbox(it->name.c_str(), &on))
                    st.checked[key] = on;
                if (k == PurgeKind::Blocks && (ImGui::IsItemHovered() || ImGui::IsItemClicked()))
                    st.previewBlock = static_cast<int>(it->id);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("Preview");
    if (st.previewBlock >= 0 &&
        ctx.doc->block(BlockId{static_cast<std::uint32_t>(st.previewBlock)})) {
        if (st.previewOf != st.previewBlock) {
            st.preview =
                thumbnailOfBlock(*ctx.doc, BlockId{static_cast<std::uint32_t>(st.previewBlock)});
            st.previewOf = st.previewBlock;
        }
        drawPicture(st.preview, pictureSize);
    } else {
        drawPicture({}, pictureSize);
    }
    ImGui::EndGroup();

    bool nestedBox = nested;
    if (ImGui::Checkbox("Purge nested items", &nestedBox))
        ctx.host->setSetting(kNestedSetting, nestedBox ? "1" : "0");
    ImGui::TextDisabled("Unnamed objects");
    const std::string zeroLabel = "Zero-length geometry (" + std::to_string(st.zero) + ")";
    const std::string emptyLabel = "Empty text objects (" + std::to_string(st.empty) + ")";
    const std::string orphanLabel = "Orphaned data (" + std::to_string(st.orphaned) + ")";
    ImGui::Checkbox(zeroLabel.c_str(), &st.doZero);
    ImGui::SameLine();
    ImGui::Checkbox(emptyLabel.c_str(), &st.doEmpty);
    ImGui::SameLine();
    ImGui::Checkbox(orphanLabel.c_str(), &st.doOrphaned);

    // The unnamed groups go with either button when their box is ticked.
    auto extras = [&] {
        std::string spec;
        if (st.doZero && st.zero)
            spec += " Z";
        if (st.doEmpty && st.empty)
            spec += " E";
        if (st.doOrphaned && st.orphaned)
            spec += " O";
        return spec;
    };
    ImGui::BeginDisabled(ctx.commandActive);
    if (ImGui::Button("Purge Checked Items")) {
        std::string spec;
        for (const auto& it : st.items) {
            auto found = st.checked.find(keyOf(it));
            if (found == st.checked.end() || found->second)
                spec += " " + keyOf(it);
        }
        run(ctx, spec + extras());
    }
    ImGui::SameLine();
    if (ImGui::Button("Purge All")) {
        std::string spec;
        for (const auto& it : st.items)
            spec += " " + keyOf(it);
        run(ctx, spec + extras());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh") && !ctx.commandActive)
        refresh(st, *ctx.doc);
}

}  // namespace

#endif

void registerPurgePanel(ui::UiRegistry& ui) {
#ifdef TBCAD_BLOCK_PANEL_IMGUI
    ui.addPanel("purge", "Purge", drawPurgePanel, false);
#endif
    ui.addMenuItem("File/Drawing Utilities/Purge...", "PURGE", "file.purge");
}

}  // namespace tbcad::block_cmds
