#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::cmd {

// Outcome of one prompt.
//   Ok       a value was entered or picked
//   Cancel   the user pressed Esc; the command should wrap up and return
//   Keyword  the user chose one of the prompt's keywords (see the result's `keyword`)
//   None     the user pressed Enter without a value (and there was no default)
enum class Status { Ok, Cancel, Keyword, None };

struct PointResult {
    Status status = Status::Cancel;
    Vec2 point{};
    std::string keyword{};
    bool ok() const { return status == Status::Ok; }
};
struct RealResult {  // getDistance, getAngle (radians) and getReal
    Status status = Status::Cancel;
    double value = 0;
    std::string keyword{};
    bool ok() const { return status == Status::Ok; }
};
struct IntResult {
    Status status = Status::Cancel;
    long long value = 0;
    std::string keyword{};
    bool ok() const { return status == Status::Ok; }
};
struct StringResult {
    Status status = Status::Cancel;
    std::string text{};
    bool ok() const { return status == Status::Ok; }
};
struct KeywordResult {  // status is Keyword when a keyword was chosen
    Status status = Status::Cancel;
    std::string keyword{};
    bool ok() const { return status == Status::Keyword; }
};
struct EntityResult {
    Status status = Status::Cancel;
    EntityId id{};
    Vec2 pickPoint{};
    std::string keyword{};
    bool shift = false;  // Shift was held for the pick (TRIM <-> EXTEND)
    bool ok() const { return status == Status::Ok; }
};
struct SelectionResult {
    Status status = Status::Cancel;
    std::vector<EntityId> ids{};
    std::string keyword{};
    bool ok() const { return status == Status::Ok; }
};

// Live preview while the cursor moves: draw into `out` what the command would create if the
// user clicked at `cursor` (already passed through the point filters).
using PreviewFn = std::function<void(Vec2 cursor, DisplayList& out)>;
// Preview of what a text prompt would create with the text typed so far (drawn while typing).
using TextPreviewFn = std::function<void(const std::string& text, DisplayList& out)>;
using EntityFilterFn = std::function<bool(const Entity&)>;

struct PointOptions {
    std::optional<Vec2> base{};  // rubber-band origin; also the base for typed @, polar input
    std::vector<std::string> keywords{};
    PreviewFn preview{};
    bool allowNone = true;  // Enter returns Status::None
    // Text that is neither a point nor a keyword comes back as Status::Keyword with the typed
    // text in `keyword`, instead of being rejected.
    bool freeText = false;
    // false: a click is taken where it is, object snaps and the other point filters leave it
    // alone (HATCH pick points: a snap would land on the boundary itself, as AutoCAD knows).
    bool snap = true;
};
struct DistanceOptions {  // a number, or a point that is measured from `base`
    std::optional<Vec2> base{};
    std::vector<std::string> keywords{};
    PreviewFn preview{};
    std::optional<double> defaultValue{};  // returned (Ok) on Enter
    bool allowZero = true;
    bool allowNegative = false;
};
struct AngleOptions {  // a typed angle (current angle unit, degrees by default) or a point
    std::optional<Vec2> base{};
    std::vector<std::string> keywords{};
    PreviewFn preview{};
    std::optional<double> defaultValue{};  // radians
    // The typed number (and the default) is an angle in the current UCS, from its X axis; the
    // answer is the same direction as a world angle. A point answer is a world angle already.
    // For prompts that ask for a direction, not for an amount to turn by.
    bool inUcs = false;
};
struct RealOptions {
    std::vector<std::string> keywords{};
    std::optional<double> defaultValue{};
    std::optional<double> min{};
    std::optional<double> max{};
};
struct IntOptions {
    std::vector<std::string> keywords{};
    std::optional<long long> defaultValue{};
    std::optional<long long> min{};
    std::optional<long long> max{};
    TextPreviewFn textPreview{};  // draws what the typed number would do
};
// Asks the application to edit a text prompt in the drawing, in a box at `anchor` (top-left
// corner, world mm), instead of on the command line. `widthMm`/`heightMm` size the box.
struct TextEditorHint {
    Vec2 anchor{};
    double widthMm = 0;
    double heightMm = 0;
    bool multiline = true;  // false: Enter accepts (a single line of text)
};
struct StringOptions {
    std::optional<std::string> defaultValue{};
    std::optional<TextEditorHint> editor{};
    bool allowEmpty = false;  // Enter returns Ok with "" instead of None
    TextPreviewFn textPreview{};
};
struct KeywordOptions {
    std::vector<std::string> keywords{};
    std::optional<std::string> defaultKeyword{};  // returned on Enter
    // Short forms that would be ambiguous as prefixes, e.g. {"N", "New"}; not shown in the prompt.
    std::vector<std::pair<std::string, std::string>> aliases{};
};
struct SelectionOptions {
    std::vector<std::string> keywords{};
    EntityFilterFn filter{};        // entities for which it returns false are ignored
    bool allowPreselection = true;  // use the host's current selection without prompting
    bool single = false;            // finish after the first pick
};
struct EntityOptions {
    std::vector<std::string> keywords{};
    EntityFilterFn filter{};
};

// What the host (or ScriptHost) feeds to the CommandRunner.
struct RawInput {
    Vec2 worldCursor{};
    Vec2 screenCursor{};
    double pixelSize = 1;  // world units (mm) per screen pixel, for apertures
    bool shift = false;
    bool ctrl = false;
};

struct Input {
    enum class Kind { Text, Enter, Cancel, Point, Entity, Selection };
    Kind kind = Kind::Enter;
    std::string text{};
    RawInput raw{};
    EntityId entity{};
    std::vector<EntityId> ids{};

    static Input typed(std::string s) {
        Input i;
        i.kind = Kind::Text;
        i.text = std::move(s);
        return i;
    }
    static Input enter() { return Input{}; }
    static Input cancel() {
        Input i;
        i.kind = Kind::Cancel;
        return i;
    }
    static Input click(RawInput r) {
        Input i;
        i.kind = Kind::Point;
        i.raw = r;
        return i;
    }
    static Input click(Vec2 p) { return click(RawInput{p, p, 1}); }
    // The host picked an entity at `at` (for getEntity, and for getSelection).
    static Input pick(EntityId id, Vec2 at = {}, bool shift = false) {
        Input i;
        i.kind = Kind::Entity;
        i.entity = id;
        i.raw.worldCursor = at;
        i.raw.shift = shift;
        return i;
    }
    static Input select(std::vector<EntityId> ids) {
        Input i;
        i.kind = Kind::Selection;
        i.ids = std::move(ids);
        return i;
    }
};

// ---- Point filters (osnap, polar/ortho, grid snap ...) ---------------------------------------

struct PointQuery {
    std::optional<Vec2> base{};  // current rubber-band origin, if any
    bool click = false;          // true when the point is being committed, false while hovering
    bool shift = false;          // Shift is held: temporary ortho (flips the ORTHO setting)
    double pixelSize = 1;
};

// What a filter wants the UI to draw (snap marker, tracking line, tooltip).
struct FilterFeedback {
    bool active = false;
    std::string marker{};  // e.g. "endpoint"; empty = none
    Vec2 markerPos{};
    std::string tooltip{};
    std::optional<std::pair<Vec2, Vec2>> guide{};  // tracking line
    // Set by the runner for every point prompt: the base point and the point the filters settled
    // on (what the preview and a click would use). `hasPoint` is false before the first move.
    bool hasPoint = false;
    std::optional<Vec2> base{};
    Vec2 point{};
};

// One editable field next to the cursor (dynamic input): "Distance 12.5", "Angle 45".
struct CursorField {
    std::string label{};
    std::string text{};
    bool locked = false;  // the user typed a value; it no longer follows the cursor
};

// The data behind the cursor fields. The application draws them and routes typing; the module
// that implements this decides what they mean.
class CursorFieldProvider {
public:
    virtual ~CursorFieldProvider() = default;
    // Fields for the point prompt that `feedback` describes; empty when there are none.
    virtual std::vector<CursorField> fields(const FilterFeedback& feedback) const = 0;
    // Field that has the keyboard (-1 = none).
    virtual int focused() const = 0;
    virtual void focus(int index) = 0;
    // Gives the keyboard to the next field; false when there are no fields to go through.
    virtual bool focusNext() = 0;
    // Text typed into a field: a value locks it, empty text unlocks it. False when unreadable.
    virtual bool edit(std::size_t index, std::string_view text) = 0;
    // The point the locked values fix completely (all fields locked); nullopt otherwise.
    virtual std::optional<Vec2> lockedPoint(const FilterFeedback& feedback) const = 0;
    // The prompt ended: forget locks and focus.
    virtual void reset() = 0;
};

class PointFilter {
public:
    virtual ~PointFilter() = default;
    // Returns the adjusted point, or nullopt when this filter does not apply at `raw`.
    virtual std::optional<Vec2> filter(const PointQuery& query, Vec2 raw,
                                       FilterFeedback& feedback) = 0;
};

// Ordered filters; the first one (lowest `order`) that returns a point wins, so osnap beats
// polar/ortho, which beat grid snap. Filters are not owned.
class PointFilterChain {
public:
    void add(PointFilter* f, int order);
    void remove(PointFilter* f);
    void clear() { entries_.clear(); }
    std::size_t size() const { return entries_.size(); }
    Vec2 apply(const PointQuery& query, Vec2 raw, FilterFeedback& feedback) const;

private:
    struct Entry {
        int order;
        PointFilter* f;
    };
    std::vector<Entry> entries_{};
};

// ---- Typed text -> point ----------------------------------------------------------------------

struct PointParseContext {
    std::optional<Vec2> base{};  // base of the active prompt
    Vec2 lastPoint{};            // last point entered in this session ("@" is relative to it)
    Vec2 cursor{};               // last known cursor position, for direct distance entry
    LengthUnit unit = LengthUnit::Millimeter;
    AngleUnit angleUnit = AngleUnit::Degrees;
};

// Drafting aids install one of these in the runner to extend typed input (direct distance,
// tracking ...). It is asked first; returning nullopt falls through to the built-in grammar.
class CommandLineParser {
public:
    virtual ~CommandLineParser() = default;
    virtual std::optional<Vec2> parsePoint(std::string_view text, const PointParseContext& ctx) = 0;
    // Rotation of the current UCS (radians), for angle prompts that are asked in the UCS.
    virtual double ucsAngle() const { return 0; }
};

// Built-in grammar: `x,y` absolute, `@dx,dy` relative to lastPoint, `@d<a` and `d<a` polar
// (relative to lastPoint when prefixed with @, otherwise to the origin), `#x,y` absolute.
std::optional<Vec2> parsePointText(std::string_view text, const PointParseContext& ctx);

// Case-insensitive keyword match: exact match, else a unique prefix.
std::optional<std::string> matchKeyword(const std::vector<std::string>& keywords,
                                        std::string_view input);

std::string trim(std::string_view s);
std::string toUpperAscii(std::string_view s);

}  // namespace tbcad::cmd
