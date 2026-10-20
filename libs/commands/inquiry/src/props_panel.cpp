// The Drawing Properties (DWGPROPS) and Drawing Units (UNITS) dialogs. Every change runs the typed
// command (-DWGPROPS, -UNITS), so it is one undo step and shows on the command line.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "panels.h"
#include "tbcad/inquiry/inquiry.h"
#include "tbcad/layout/layout.h"
#include "tbcad/platform/paths.h"

namespace tbcad::inquiry {

namespace {

namespace fs = std::filesystem;

void runLines(ui::PanelContext& pc, const std::vector<std::string>& lines) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

std::string fileTime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec)
        return {};
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

// ---- DWGPROPS ----

constexpr const char* kSummary[] = {"Title", "Subject", "Author", "Keywords", "Comments"};

struct PropsState {
    std::uint64_t revision = ~0ull;
    const Document* doc = nullptr;
    std::string fields[5]{};
    std::string newName{}, newValue{};
};

std::string summaryKey(const char* field) {
    std::string k = "tbcad.prop.";
    for (const char* c = field; *c; ++c)
        k += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    return k;
}

void drawDwgProps(ui::PanelContext& pc) {
    if (!pc.doc || !pc.host)
        return;
    static PropsState st;
    const Header& h = pc.doc->header();
    if (st.doc != pc.doc || st.revision != pc.doc->revision()) {
        for (int i = 0; i < 5; ++i) {
            auto it = h.custom.find(summaryKey(kSummary[i]));
            st.fields[i] = it == h.custom.end() ? std::string() : it->second;
        }
        st.doc = pc.doc;
        st.revision = pc.doc->revision();
    }
    if (!ImGui::BeginTabBar("##dwgprops"))
        return;
    if (ImGui::BeginTabItem("General")) {
        const std::string file = pc.host->documentFile();
        if (file.empty()) {
            ImGui::TextUnformatted("Not saved yet.");
        } else {
            const fs::path p = platform::pathFromUtf8(file);
            std::error_code ec;
            const auto size = fs::file_size(p, ec);
            ImGui::Text("Name: %s", platform::pathToUtf8(p.filename()).c_str());
            ImGui::TextWrapped("Location: %s", platform::pathToUtf8(p.parent_path()).c_str());
            if (!ec)
                ImGui::Text("Size: %.1f KB (%llu bytes)", static_cast<double>(size) / 1024.0,
                            static_cast<unsigned long long>(size));
            ImGui::Text("Modified: %s", fileTime(p).c_str());
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Summary")) {
        for (int i = 0; i < 5; ++i) {
            ImGui::PushID(i);
            if (i == 4)
                ImGui::InputTextMultiline(kSummary[i], &st.fields[i], ImVec2(0, 70));
            else
                ImGui::InputText(kSummary[i], &st.fields[i]);
            ImGui::PopID();
        }
        ImGui::BeginDisabled(pc.commandActive);
        if (ImGui::Button("Apply")) {
            std::vector<std::string> lines = {"-DWGPROPS"};
            for (int i = 0; i < 5; ++i) {
                auto it = h.custom.find(summaryKey(kSummary[i]));
                const std::string now = it == h.custom.end() ? std::string() : it->second;
                // One line per field: Comments are kept on one line too.
                std::string value = st.fields[i];
                for (char& c : value)
                    if (c == '\n' || c == '\r')
                        c = ' ';
                if (value != now) {
                    lines.push_back(kSummary[i]);
                    lines.push_back(value);
                }
            }
            lines.push_back("");
            if (lines.size() > 2)
                runLines(pc, lines);
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Statistics")) {
        const Document& d = *pc.doc;
        ImGui::Text("Objects: %zu", d.entityCount());
        ImGui::Text("Layers: %zu", d.layers().size());
        ImGui::Text("Blocks: %zu", d.blocks().size());
        ImGui::Text("Text styles: %zu", d.textStyles().size());
        ImGui::Text("Dimension styles: %zu", d.dimStyles().size());
        ImGui::Text("Linetypes: %zu", d.linetypes().size());
        ImGui::Text("Layouts: %zu", layout::layouts(d).size());
        ImGui::Text("Changes since opened: %llu", static_cast<unsigned long long>(d.revision()));
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Custom")) {
        const auto custom = customProperties(h);
        if (ImGui::BeginTable("##custom", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();
            for (const auto& [name, value] : custom) {
                ImGui::PushID(name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(value.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Delete") && !pc.commandActive)
                    runLines(pc, {"-DWGPROPS", "Custom", name, "", ""});
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::SetNextItemWidth(140);
        ImGui::InputTextWithHint("##cname", "Name", &st.newName);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##cvalue", "Value", &st.newValue);
        ImGui::SameLine();
        ImGui::BeginDisabled(pc.commandActive || st.newName.empty() || st.newValue.empty());
        if (ImGui::Button("Add")) {
            runLines(pc, {"-DWGPROPS", "Custom", st.newName, st.newValue, ""});
            st.newName.clear();
            st.newValue.clear();
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// ---- UNITS ----

constexpr const char* kLengthTypes[] = {"Scientific", "Decimal", "Engineering", "Architectural",
                                        "Fractional"};
constexpr const char* kAngleTypes[] = {"Degrees", "DMS", "Grads", "Radians", "Surveyor"};
constexpr const char* kAngleTypeNames[] = {"Decimal Degrees", "Deg/Min/Sec", "Grads", "Radians",
                                           "Surveyor's Units"};
constexpr const char* kUnits[] = {"Millimeters", "Centimeters", "Meters", "Inches", "Feet"};

void drawUnits(ui::PanelContext& pc) {
    if (!pc.doc || !pc.host)
        return;
    const Header& h = pc.doc->header();
    const int lunits = lengthUnitsType(h);
    const int lprec = linearPrecision(h);
    const int aunits = angleUnitsType(h);
    const int aprec = anglePrecision(h);
    ImGui::BeginDisabled(pc.commandActive);

    ImGui::SeparatorText("Length");
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("Type##length", kLengthTypes[lunits - 1])) {
        for (int i = 0; i < 5; ++i)
            if (ImGui::Selectable(kLengthTypes[i], i == lunits - 1))
                runLines(pc, {"-UNITS", "Type", kLengthTypes[i]});
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(180);
    const std::string lshown = formatLengthType(0, lunits, lprec);
    if (ImGui::BeginCombo("Precision##length", lshown.c_str())) {
        for (int p = 0; p <= 8; ++p) {
            const std::string label = formatLengthType(0, lunits, p) + "##" + std::to_string(p);
            if (ImGui::Selectable(label.c_str(), p == lprec))
                runLines(pc, {"-UNITS", "Precision", std::to_string(p), std::to_string(aprec)});
        }
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("Angle");
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("Type##angle", kAngleTypeNames[aunits])) {
        for (int i = 0; i < 5; ++i)
            if (ImGui::Selectable(kAngleTypeNames[i], i == aunits))
                runLines(pc, {"-UNITS", "Angles", kAngleTypes[i]});
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(180);
    const std::string ashown = formatAngleType(0, aunits == 4 ? 0 : aunits, aprec);
    if (ImGui::BeginCombo("Precision##angle", ashown.c_str())) {
        for (int p = 0; p <= 8; ++p) {
            const std::string label =
                formatAngleType(0, aunits == 4 ? 0 : aunits, p) + "##" + std::to_string(p);
            if (ImGui::Selectable(label.c_str(), p == aprec))
                runLines(pc, {"-UNITS", "Precision", std::to_string(lprec), std::to_string(p)});
        }
        ImGui::EndCombo();
    }
    bool clockwise = anglesClockwise(h);
    if (ImGui::Checkbox("Clockwise", &clockwise))
        runLines(pc, {"-UNITS", "Direction", clockwise ? "Clockwise" : "Counterclockwise"});
    // Typed into a copy and applied when the field is left, not on every key.
    static double baseDeg = 0;
    static bool editingBase = false;
    if (!editingBase)
        baseDeg = angleBase(h) * 180.0 / kPi;
    ImGui::SetNextItemWidth(180);
    ImGui::InputDouble("Base angle (0 = east)", &baseDeg, 0, 0, "%.4g");
    editingBase = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        runLines(pc, {"-UNITS", "Base", formatDouble(baseDeg)});

    ImGui::SeparatorText("Insertion scale");
    ImGui::SetNextItemWidth(180);
    const int unit = static_cast<int>(h.units);
    if (ImGui::BeginCombo("Units to scale inserted content", kUnits[unit])) {
        for (int i = 0; i < 5; ++i)
            if (ImGui::Selectable(kUnits[i], i == unit))
                runLines(pc, {"-UNITS", kUnits[i]});
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Sample output");
    const std::string sample = formatLengthType(1.5, lunits, lprec) + "," +
                               formatLengthType(2.0039, lunits, lprec) + "," +
                               formatLengthType(0, lunits, lprec);
    ImGui::TextUnformatted(sample.c_str());
    const std::string angSample = formatLengthType(3, lunits, lprec) + "<" +
                                  formatAngleType(45.0 * kPi / 180.0, aunits, aprec);
    ImGui::TextUnformatted(angSample.c_str());
}

}  // namespace

void registerPropsPanels(ui::UiRegistry& registry) {
    registry.addPanel("dwgprops", "Drawing Properties", drawDwgProps, false);
    registry.addPanel("units", "Drawing Units", drawUnits, false);
}

}  // namespace tbcad::inquiry
