#include "tbcad/training/badges.h"

#include <algorithm>
#include <set>

#include "tbcad/geom/entity.h"

namespace tbcad::training {

namespace {

bool hasNonAscii(const std::string& s) {
    return std::any_of(s.begin(), s.end(),
                       [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
}

// Every lesson of `world` (bosses aside) has at least one star.
bool worldDone(const Progress& p, const std::vector<Lesson>& lessons, int world) {
    bool any = false;
    for (const Lesson& l : lessons) {
        if (l.world != world || l.boss)
            continue;
        any = true;
        if (p.stars(l.id) == 0)
            return false;
    }
    return any;
}

bool thaiTextDrawn(const Facts& f) {
    if (!f.doc)
        return false;
    for (EntityId id : f.doc->modelSpace()) {
        const Entity* e = f.doc->find(id);
        if (!e)
            continue;
        if (const auto* t = entityAs<TextData>(*e); t && hasNonAscii(t->utf8))
            return true;
        if (const auto* m = entityAs<MTextData>(*e); m && hasNonAscii(m->utf8))
            return true;
    }
    return false;
}

}  // namespace

const std::vector<Badge>& allBadges() {
    static const std::vector<Badge> kBadges = {
        {"first-line", {"First Line", "เส้นแรก"}, {"Finish lesson 2.1", "จบด่าน 2.1"}},
        {"three-ways",
         {"Three Ways", "สามทาง"},
         {"Start one command from a button, by typing its name and with its Thai word",
          "เริ่มคำสั่งเดียวกันครบ 3 แบบ: ปุ่ม พิมพ์ชื่อ และพิมพ์คำไทย"}},
        {"keyboard-pro",
         {"Keyboard Pro", "สายคีย์บอร์ด"},
         {"Finish 5 lessons starting every command by typing", "จบ 5 ด่านโดยเริ่มทุกคำสั่งด้วยการพิมพ์"}},
        {"snap-master",
         {"Snap Master", "นักจับจุด"},
         {"Three stars in lesson 2.6", "ได้ 3 ดาวในด่าน 2.6"}},
        {"clean-corners",
         {"Clean Corners", "มุมเนี๊ยบ"},
         {"Finish every Modify lesson", "จบทุกด่านในบท Modify"}},
        {"layer-boss",
         {"Layer Boss", "เจ้าแห่งเลเยอร์"},
         {"Finish every Layers lesson", "จบทุกด่านในบท Layers"}},
        {"thai-writer",
         {"Thai Writer", "เขียนไทยได้"},
         {"Write Thai text in lesson 5.1", "เขียนข้อความภาษาไทยในด่าน 5.1"}},
        {"measured-up",
         {"Measured Up", "วัดครบ"},
         {"Finish every Annotate lesson", "จบทุกด่านในบท Annotate"}},
        {"block-builder",
         {"Block Builder", "นักสร้างบล็อก"},
         {"Finish every Blocks lesson", "จบทุกด่านในบท Blocks"}},
        {"no-undo",
         {"No Undo", "ไม่ต้องย้อน"},
         {"Finish 3 lessons in a row without Undo", "จบ 3 ด่านติดกันโดยไม่กด Undo"}},
        {"graduate",
         {"Graduate", "จบหลักสูตร"},
         {"Beat the boss of all 7 worlds", "ผ่านบอสครบทั้ง 7 บท"}},
    };
    return kBadges;
}

const Badge* findBadge(const std::string& id) {
    for (const Badge& b : allBadges())
        if (b.id == id)
            return &b;
    return nullptr;
}

std::vector<std::string> awardBadges(Progress& p, const std::vector<Lesson>& lessons,
                                     const Lesson& lesson, const LessonRun& run,
                                     const Facts& facts) {
    // The counts kept across lessons.
    bool typedAll = true;
    int counted = 0;
    for (const Event& e : run.events()) {
        if (e.kind != Event::Kind::Started || e.transparent)
            continue;
        const int way = e.source == Source::Button                              ? 1
                        : e.source == Source::Name || e.source == Source::Alias ? 2
                        : e.source == Source::Thai                              ? 4
                                                                                : 0;
        if (way)
            p.ways[e.command] |= way;
        if (countsTowardsPar(e.command)) {
            ++counted;
            typedAll = typedAll && (way == 2 || way == 4);
        }
    }
    if (typedAll && counted > 0)
        ++p.typedLessons;
    p.noUndoStreak = run.undos() == 0 ? p.noUndoStreak + 1 : 0;

    std::vector<std::string> earned;
    auto award = [&](const char* id, bool when) {
        if (when && p.badges.insert(id).second)
            earned.emplace_back(id);
    };
    award("first-line", lesson.id == "2.1");
    award("three-ways",
          std::any_of(p.ways.begin(), p.ways.end(), [](const auto& w) { return w.second == 7; }));
    award("keyboard-pro", p.typedLessons >= 5);
    award("snap-master", p.stars("2.6") == 3);
    award("clean-corners", worldDone(p, lessons, 3));
    award("layer-boss", worldDone(p, lessons, 4));
    award("thai-writer", lesson.id == "5.1" && thaiTextDrawn(facts));
    award("measured-up", worldDone(p, lessons, 5));
    award("block-builder", worldDone(p, lessons, 6));
    award("no-undo", p.noUndoStreak >= 3);
    std::set<int> bossWorlds;
    for (const Lesson& l : lessons)
        if (l.boss && p.stars(l.id) > 0)
            bossWorlds.insert(l.world);
    award("graduate", bossWorlds.size() >= 7);
    return earned;
}

}  // namespace tbcad::training
