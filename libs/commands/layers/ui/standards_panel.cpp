// The Standards panel (CHECKSTANDARDS): what differs from the standards drawing, each row with a
// Fix button, plus Fix All. The fixes run through _FIXSTANDARDS, so each is one undo step.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <filesystem>
#include <memory>

#include "panels.h"
#include "tbcad/layers/standards.h"
#include "tbcad/xref/xref.h"

namespace tbcad::layers {

namespace {

struct StandardsState {
    std::string file{};
    bool fileEdited = false;
    std::unique_ptr<Document> standards{};
    std::string loadedFile{};
    std::string error{};
    const Document* doc = nullptr;
    std::uint64_t revision = ~0ull;
    std::vector<StandardsViolation> found{};
};

StandardsState& state() {
    static StandardsState s;
    return s;
}

void load(StandardsState& st) {
    st.standards.reset();
    st.error.clear();
    st.found.clear();
    st.loadedFile = st.file;
    auto src =
        xref::loadSource(std::filesystem::path(std::u8string(st.file.begin(), st.file.end())));
    if (!src) {
        st.error = "Cannot read the file: " + src.error().message;
        return;
    }
    st.standards = std::move(src.value().doc);
}

const char* kindTitle(StandardsKind k) {
    switch (k) {
        case StandardsKind::Layer:
            return "Layer";
        case StandardsKind::Linetype:
            return "Linetype";
        case StandardsKind::TextStyle:
            return "Text style";
        default:
            return "Dimension style";
    }
}

void fix(ui::PanelContext& ctx, const std::string& keys) {
    if (!ctx.runCommand)
        return;
    if (ctx.cancelCommand)
        ctx.cancelCommand();
    ctx.host->setSetting("tmp.standards.keys", keys);
    ctx.runCommand("_FIXSTANDARDS");
}

}  // namespace

void drawStandardsPanel(ui::PanelContext& ctx) {
    if (!ctx.doc || !ctx.host)
        return;
    StandardsState& st = state();
    if (!st.fileEdited)
        st.file = ctx.host->setting("standards.file").value_or(st.file);
    // A file the command just checked is read here too.
    if (!st.fileEdited && !st.file.empty() && st.loadedFile != st.file) {
        load(st);
        st.revision = ~0ull;
    }
    ImGui::TextDisabled("Standards file (.tbcad or .dxf)");
    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputText("##standardsfile", &st.file))
        st.fileEdited = true;
    ImGui::SameLine();
    if (ImGui::Button("Check") && !st.file.empty()) {
        ctx.host->setSetting("standards.file", st.file);
        st.fileEdited = false;
        load(st);
        st.revision = ~0ull;
    }
    if (!st.error.empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", st.error.c_str());
    if (!st.standards) {
        ImGui::TextDisabled("Pick a standards drawing and press Check.");
        return;
    }
    // Worked out again only when the drawing changes, and never while a command is running.
    if (!ctx.commandActive && (st.doc != ctx.doc || st.revision != ctx.doc->revision())) {
        st.found = checkStandards(*ctx.doc, *st.standards);
        st.doc = ctx.doc;
        st.revision = ctx.doc->revision();
    }
    if (st.found.empty()) {
        ImGui::TextUnformatted("No violations: the drawing follows the standard.");
        return;
    }
    ImGui::Text("%zu %s", st.found.size(), st.found.size() == 1 ? "violation" : "violations");
    ImGui::BeginDisabled(ctx.commandActive);
    if (ImGui::Button("Fix All"))
        fix(ctx, "*");
    ImGui::EndDisabled();
    ImGui::BeginChild("##standardslist", ImVec2(0, 0), true);
    std::string pending;
    for (const StandardsViolation& v : st.found) {
        ImGui::PushID(v.key().c_str());
        ImGui::BeginDisabled(ctx.commandActive);
        if (ImGui::Button("Fix"))
            pending = v.key();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s %s", kindTitle(v.kind), v.name.c_str());
        ImGui::TextDisabled("%s - %s", v.problem.c_str(), fixLabel(v).c_str());
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!pending.empty())
        fix(ctx, pending);
}

}  // namespace tbcad::layers
