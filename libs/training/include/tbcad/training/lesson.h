#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/math.h"
#include "tbcad/core/result.h"

namespace tbcad::training {

// Text shown to the user, in English and Thai (an empty Thai text falls back to English).
struct Text {
    std::string en{};
    std::string th{};
};

// One condition a step waits for. Which fields count depends on the type; the rest stay empty.
// The lesson file format is described in docs/training.md.
struct Check {
    enum class Type {
        Line,       // a line (or straight polyline segment): from/to, or from + length/angle
        Circle,     // a circle: center, radius
        Polyline,   // a polyline: closed, vertices, arcs, box, area
        Path,       // every segment of `path` is covered by drawn lines or polyline segments
        Count,      // the number of objects that match the filters
        Layer,      // a layer `name`: color, on, frozen, locked
        Command,    // the user started `command` (from `source`) at least `min` times
        Undo,       // the user undid at least `min` steps
        Redo,       // the user redid at least `min` steps
        Selection,  // exactly `equals` objects are selected and all match the filters
        View,       // `box` is in view and fills at least `minFill` of its width or height
        Prompt,     // `command` is running and its prompt contains `contains`
        Arc,        // an arc: center, radius
        Text,       // a text, multiline text or leader note that `contains` a string: height,
                    // box (where it starts), tip (where a leader points)
        Dimension,  // a dimension: dim (its kind), value (its measurement; degrees for angles)
        Object,     // an object that passes the filters, with box (its extents)
        Block,      // a block definition `name`, holding `objects` objects
        Insert,     // a placed block `name`: center (where it is), angle (its rotation)
    };
    Type type = Type::Line;

    // Geometry (millimetres, degrees).
    std::optional<Vec2> from{}, to{}, center{};
    std::optional<double> length{}, angle{}, radius{}, area{};
    std::optional<BBox2> box{};
    std::vector<Vec2> path{};
    std::optional<bool> closed{};
    std::optional<int> vertices{}, arcs{}, objects{};
    double tol = 0.5;         // distance tolerance
    double angleTol = 0.1;    // degrees
    double areaTolPct = 0.5;  // percent of the expected area

    // Object filters (Count, Selection and the geometry checks).
    std::string kind{};  // "line", "circle", "polyline"...; empty = any
    std::string layer{};
    std::optional<int> color{};  // colour index the object shows (ByLayer resolved)

    std::optional<int> equals{}, min{}, max{};

    // Layer / Block / Insert.
    std::string name{};
    std::optional<bool> on{}, frozen{}, locked{};

    // Text / Dimension.
    std::optional<double> height{}, value{};
    std::optional<BBox2> tip{};
    std::string dim{};  // "linear", "aligned", "radius", "diameter", "angular"

    // Command / Prompt.
    std::string command{};
    std::string source{};  // "button", "name", "alias", "thai"; empty = any
    std::string contains{};

    // View.
    double minFill = 0;
};

struct Step {
    Text text{};
    Text hint{};
    std::vector<Check> checks{};  // all must pass; empty = done when a later step is done
};

// A target drawn faintly on the canvas for the user to draw over.
struct Ghost {
    enum class Kind { Line, Circle, Polyline, Dot };
    Kind kind = Kind::Line;
    std::vector<Vec2> points{};    // Line: 2; Polyline: the vertices; Circle, Dot: the centre
    std::vector<double> bulges{};  // Polyline: one per vertex (empty = all straight)
    double radius = 0;
    bool closed = false;
};

struct Lesson {
    std::string id{};  // "2.1"; a boss is "B2"
    int world = 0;
    bool boss = false;  // the world's challenge: no steps to follow, the time counts
    Text title{};
    Text goal{};
    Text tip{};                        // shown when the lesson is done
    int par = 0;                       // commands for the third star
    double parSeconds = 0;             // > 0: the third star also needs this time (challenges)
    std::vector<std::string> setup{};  // script lines that draw the starting drawing
    std::vector<Ghost> ghosts{};
    std::vector<Step> steps{};
    std::vector<std::string> solution{};            // script lines that pass the lesson (for tests)
    std::vector<std::vector<std::string>> wrong{};  // attempts that must not pass (for tests)
};

Result<Lesson> parseLesson(std::string_view json);
Result<Lesson> loadLesson(const std::filesystem::path& file);
// Every *.json lesson in `folder`, by world and then by id ("2.10" after "2.9"), the world's boss
// last. Files that do not
// load are skipped and described in `errors` when given.
std::vector<Lesson> loadLessons(const std::filesystem::path& folder,
                                std::vector<std::string>* errors = nullptr);

}  // namespace tbcad::training
