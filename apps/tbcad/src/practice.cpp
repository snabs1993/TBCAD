#include "practice.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "tbcad/core/math.h"
#include "tbcad/render/overlay.h"

namespace tbcad::app {

namespace {

constexpr const char* kProgressKey = "training.progress";

struct WorldName {
    const char* en;
    const char* th;
};

WorldName worldName(int world) {
    static const WorldName kNames[] = {{"Get around", "รู้จักโปรแกรม"},
                                       {"Draw", "วาด"},
                                       {"Modify", "แก้ไข"},
                                       {"Layers", "เลเยอร์"},
                                       {"Annotate", "ข้อความและขนาด"},
                                       {"Blocks", "บล็อก"},
                                       {"Print and save", "พิมพ์และบันทึก"}};
    if (world >= 1 && world <= static_cast<int>(std::size(kNames)))
        return kNames[world - 1];
    return {"More", "เพิ่มเติม"};
}

const ImVec4 kDone{0.30f, 0.75f, 0.42f, 1.0f};
const ImVec4 kStar{0.98f, 0.76f, 0.18f, 1.0f};

// The English text, and the Thai one under it in the dimmer colour.
void twoLanguages(const training::Text& t, bool dimEnglish = false) {
    if (!t.en.empty()) {
        if (dimEnglish)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", t.en.c_str());
        if (dimEnglish)
            ImGui::PopStyleColor();
    }
    if (!t.th.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", t.th.c_str());
        ImGui::PopStyleColor();
    }
}

// Five-pointed stars, `filled` of them filled, at the cursor; moves the cursor past them.
void stars(int filled, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImU32 fill = ImGui::GetColorU32(kStar);
    const ImU32 empty = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (int s = 0; s < 3; ++s) {
        const ImVec2 c{at.x + size * (static_cast<float>(s) + 0.5f) + 2.0f * static_cast<float>(s),
                       at.y + size * 0.5f};
        ImVec2 pts[10];
        for (int i = 0; i < 10; ++i) {
            const float r = (i % 2 == 0) ? size * 0.5f : size * 0.21f;
            const float a = -1.5707963f + static_cast<float>(i) * 0.6283185f;
            pts[i] = ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a));
        }
        if (s < filled) {
            for (int i = 0; i < 10; ++i)  // a star is a fan of triangles around its centre
                dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], fill);
        } else {
            dl->AddPolyline(pts, 10, empty, ImDrawFlags_Closed, 1.2f);
        }
    }
    ImGui::Dummy(ImVec2(size * 3 + 4.0f, size));
}

std::string clock(double seconds) {
    const int s = static_cast<int>(seconds);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

}  // namespace

Practice::Practice(ui::AppCore& core, ui::UiRegistry& registry,
                   const std::filesystem::path& lessonDir)
    : core_(core) {
    lessons_ = training::loadLessons(lessonDir, &loadErrors_);
    progress_ = training::Progress::fromJson(core_.settings().getString(kProgressKey, ""));
    registry.addPanel("practice", "Practice", [this](ui::PanelContext& pc) { draw(pc); });
    registry.addOverlayDrawer(
        [this](ui::PanelContext&, render::Overlay& overlay) { drawGhosts(overlay); });
    core_.addCommandListener([this](const ui::CommandEvent& e) {
        if (!run_ || finished_ || !inPracticeDrawing())
            return;
        training::Event ev;
        ev.kind = static_cast<training::Event::Kind>(e.kind);
        ev.command = e.command;
        ev.source = training::classifySource(e.word, e.fromButton, e.command);
        ev.transparent = e.transparent;
        ev.cancelled = e.cancelled;
        run_->record(ev);
    });
}

void Practice::open() {
    core_.openPanel("practice");
}

void Practice::start(std::size_t index) {
    if (index >= lessons_.size())
        return;
    for (int i = 0; i < 16 && core_.runner().active(); ++i)
        core_.pressEscape();
    if (core_.runner().active())
        return;
    const training::Lesson& lesson = lessons_[index];
    run_.reset();  // the setup's own commands are not the user's
    active_ = index;
    if (inPracticeDrawing()) {
        core_.newScratchDocument("Practice " + lesson.id);
        // One practice tab at a time: the one just left goes (it never asks to be saved).
        const std::uint64_t old = docId_;
        for (std::size_t i = 0; i < core_.documentCount(); ++i)
            if (core_.documentId(i) == old && core_.documentFile(i).empty() &&
                !core_.documentModified(i)) {
                core_.closeDocument(i);
                break;
            }
    } else {
        core_.newScratchDocument("Practice " + lesson.id);
    }
    core_.runSetup(lesson.setup);
    docId_ = core_.documentId(core_.activeDocument());
    run_ = std::make_unique<training::LessonRun>(lesson);
    startedAt_ = std::chrono::steady_clock::now();
    finished_ = false;
    showHint_ = false;
    improved_ = false;
    run_->update(facts());
    open();
}

void Practice::leave() {
    run_.reset();
    active_.reset();
    finished_ = false;
    showHint_ = false;
}

bool Practice::practiceDrawingOpen() const {
    for (std::size_t i = 0; i < core_.documentCount(); ++i)
        if (core_.documentId(i) == docId_)
            return true;
    return false;
}

bool Practice::inPracticeDrawing() const {
    return docId_ != 0 && core_.documentCount() > 0 &&
           core_.documentId(core_.activeDocument()) == docId_;
}

double Practice::elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count();
}

training::Facts Practice::facts() const {
    training::Facts f;
    f.doc = &core_.document();
    f.selection = core_.selection();
    const cmd::ViewState v = core_.view();
    BBox2 box;
    box.expand({v.center.x - v.sizePx.x * v.scale / 2, v.center.y - v.sizePx.y * v.scale / 2});
    box.expand({v.center.x + v.sizePx.x * v.scale / 2, v.center.y + v.sizePx.y * v.scale / 2});
    f.view = box;
    if (const cmd::PromptInfo* p = core_.promptInfo()) {
        f.runningCommand = p->command;
        f.prompt = p->text;
    }
    return f;
}

void Practice::saveProgress() {
    core_.settings().setString(kProgressKey, progress_.toJson());
}

void Practice::tick() {
    if (!run_ || finished_)
        return;
    if (!practiceDrawingOpen()) {  // the practice tab was closed: the lesson is over
        leave();
        return;
    }
    if (!inPracticeDrawing())
        return;
    run_->update(facts());
    if (!run_->complete())
        return;
    finished_ = true;
    score_ = run_->score(elapsed());
    improved_ = progress_.record(run_->lesson().id, score_);
    saveProgress();
    core_.print("Lesson " + run_->lesson().id + " done: " + std::to_string(score_.stars) +
                (score_.stars == 1 ? " star." : " stars."));
    open();
}

void Practice::draw(ui::PanelContext&) {
    if (run_ && finished_)
        drawResult();
    else if (run_)
        drawLesson();
    else
        drawMap();
}

void Practice::drawMap() {
    ImGui::TextWrapped(
        "Short lessons that check your drawing as you go. Each one opens in a "
        "practice drawing of its own, so your work is never touched.");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        "บทเรียนสั้นๆ ที่ตรวจแบบให้ทันทีขณะวาด แต่ละบทเปิดในแบบฝึกแยก "
        "งานจริงของคุณจะไม่ถูกแตะต้อง");
    ImGui::PopStyleColor();
    for (const std::string& e : loadErrors_)
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "%s", e.c_str());
    if (lessons_.empty()) {
        ImGui::TextWrapped("No lessons were found (assets/training/lessons).");
        return;
    }
    // The first lesson without a star is the one to do next.
    std::optional<std::size_t> next;
    for (std::size_t i = 0; i < lessons_.size() && !next; ++i)
        if (progress_.stars(lessons_[i].id) == 0)
            next = i;
    int world = -1;
    for (std::size_t i = 0; i < lessons_.size(); ++i) {
        const training::Lesson& l = lessons_[i];
        if (l.world != world) {
            world = l.world;
            const WorldName w = worldName(world);
            ImGui::Spacing();
            ImGui::SeparatorText((std::to_string(world) + "  " + w.en + "  /  " + w.th).c_str());
        }
        ImGui::PushID(static_cast<int>(i));
        const std::string label = l.id + "  " + l.title.en + (next == i ? "   < Next" : "");
        const float starSize = ImGui::GetFontSize() * 0.9f;
        const float right = ImGui::GetContentRegionAvail().x - (starSize * 3 + 8.0f);
        if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(std::max(right, 40.0f), 0)))
            start(i);
        ImGui::SameLine();
        stars(progress_.stars(l.id), starSize);
        if (!l.title.th.empty()) {
            ImGui::Indent();
            ImGui::TextDisabled("%s", l.title.th.c_str());
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
}

void Practice::drawLesson() {
    const training::Lesson& l = run_->lesson();
    if (ImGui::SmallButton("< All lessons"))
        leave();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", clock(elapsed()).c_str());
    if (!run_)
        return;
    ImGui::Separator();
    ImGui::Text("%s  %s", l.id.c_str(), l.title.en.c_str());
    if (!l.title.th.empty())
        ImGui::TextDisabled("%s", l.title.th.c_str());
    ImGui::Spacing();
    twoLanguages(l.goal);
    ImGui::Separator();
    if (!inPracticeDrawing()) {
        ImGui::TextColored(kStar, "Go back to the %s tab to carry on.",
                           ("Practice " + l.id).c_str());
        ImGui::Spacing();
    }
    const std::size_t current = run_->currentStep();
    const float r = ImGui::GetFontSize() * 0.32f;
    for (std::size_t i = 0; i < l.steps.size(); ++i) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 c{at.x + r + 1, at.y + ImGui::GetFontSize() * 0.55f};
        if (run_->stepDone(i))
            dl->AddCircleFilled(c, r, ImGui::GetColorU32(kDone));
        else if (i == current)
            dl->AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_Text), 0, 2.0f);
        else
            dl->AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_TextDisabled), 0, 1.0f);
        ImGui::Dummy(ImVec2(r * 2 + 6, ImGui::GetFontSize()));
        ImGui::SameLine();
        ImGui::BeginGroup();
        twoLanguages(l.steps[i].text, i != current);
        ImGui::EndGroup();
        if (i == current && showHint_ && !l.steps[i].hint.en.empty()) {
            ImGui::Indent(r * 2 + 6);
            ImGui::PushStyleColor(ImGuiCol_Text, kStar);
            ImGui::TextWrapped("Hint: %s", l.steps[i].hint.en.c_str());
            ImGui::PopStyleColor();
            if (!l.steps[i].hint.th.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kStar);
                ImGui::TextWrapped("คำใบ้: %s", l.steps[i].hint.th.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Unindent(r * 2 + 6);
        }
        ImGui::Spacing();
    }
    ImGui::Separator();
    const bool hasHint = current < l.steps.size() && !l.steps[current].hint.en.empty();
    ImGui::BeginDisabled(!hasHint || showHint_);
    if (ImGui::Button("Hint")) {
        showHint_ = true;
        run_->useHint();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Restart"))
        start(*active_);
    ImGui::SameLine();
    if (ImGui::Button("Skip")) {
        if (*active_ + 1 < lessons_.size())
            start(*active_ + 1);
        else
            leave();
    }
    ImGui::TextDisabled("Hint: no second star. Par: %d command%s for the third.", l.par,
                        l.par == 1 ? "" : "s");
}

void Practice::drawResult() {
    const training::Lesson& l = run_->lesson();
    ImGui::Text("%s  %s", l.id.c_str(), l.title.en.c_str());
    ImGui::Spacing();
    ImGui::TextColored(kDone, "Lesson complete!");
    ImGui::TextColored(kDone, "ผ่านด่านแล้ว!");
    ImGui::Spacing();
    stars(score_.stars, ImGui::GetFontSize() * 1.8f);
    ImGui::Spacing();
    ImGui::Text("Time %s   Commands %d (par %d)%s", clock(score_.seconds).c_str(), score_.commands,
                l.par, score_.hintUsed ? "   Hint used" : "");
    if (improved_)
        ImGui::TextDisabled("New best for this lesson.");
    if (!l.tip.en.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        twoLanguages(l.tip);
    }
    ImGui::Spacing();
    const bool more = *active_ + 1 < lessons_.size();
    if (more && ImGui::Button("Next lesson"))
        start(*active_ + 1);
    else {
        if (more)
            ImGui::SameLine();
        if (ImGui::Button("Play again"))
            start(*active_);
        ImGui::SameLine();
        if (ImGui::Button("All lessons"))
            leave();
    }
}

void Practice::drawGhosts(render::Overlay& overlay) const {
    if (!run_ || finished_ || !inPracticeDrawing())
        return;
    render::OverlayStyle st;
    st.color = Rgba{60, 190, 200, 170};
    st.widthPx = 2.0;
    st.dashPx = 10;
    st.gapPx = 6;
    const Rgba dot{70, 200, 100, 255};
    for (const training::Ghost& g : run_->lesson().ghosts) {
        switch (g.kind) {
            case training::Ghost::Kind::Line:
                overlay.line(render::Space::World, g.points[0], g.points[1], st);
                break;
            case training::Ghost::Kind::Dot:
                overlay.marker(render::Space::World, g.points[0], render::MarkerKind::Circle, 12.0,
                               dot, true);
                break;
            case training::Ghost::Kind::Circle: {
                std::vector<Vec2> ring;
                for (int i = 0; i < 96; ++i) {
                    const double a = kTwoPi * i / 96;
                    ring.push_back({g.points[0].x + g.radius * std::cos(a),
                                    g.points[0].y + g.radius * std::sin(a)});
                }
                overlay.polyline(render::Space::World, ring, true, st);
                break;
            }
            case training::Ghost::Kind::Polyline: {
                // Arcs (bulges) as short straight pieces.
                std::vector<Vec2> pts;
                const std::size_t n = g.points.size();
                const std::size_t segs = g.closed ? n : n - 1;
                for (std::size_t i = 0; i < segs; ++i) {
                    const Vec2 a = g.points[i], b = g.points[(i + 1) % n];
                    const double bulge = i < g.bulges.size() ? g.bulges[i] : 0.0;
                    pts.push_back(a);
                    if (std::abs(bulge) < 1e-9)
                        continue;
                    const double sweep = 4 * std::atan(bulge);
                    const double chord = distance(a, b);
                    const double radius = chord / (2 * std::sin(std::abs(sweep) / 2));
                    const Vec2 mid{(a.x + b.x) / 2, (a.y + b.y) / 2};
                    const double h =
                        radius * std::cos(sweep / 2);  // centre's distance from the chord
                    const Vec2 dir{(b.x - a.x) / chord, (b.y - a.y) / chord};
                    const Vec2 left{-dir.y, dir.x};
                    const double side = bulge > 0 ? 1.0 : -1.0;
                    const Vec2 ctr{mid.x + left.x * h * side, mid.y + left.y * h * side};
                    const double a0 = std::atan2(a.y - ctr.y, a.x - ctr.x);
                    for (int k = 1; k < 32; ++k) {
                        const double t = a0 + sweep * k / 32;
                        pts.push_back({ctr.x + radius * std::cos(t), ctr.y + radius * std::sin(t)});
                    }
                }
                if (!g.closed)
                    pts.push_back(g.points.back());
                overlay.polyline(render::Space::World, pts, g.closed, st);
                break;
            }
        }
    }
}

}  // namespace tbcad::app
