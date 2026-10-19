#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::layers {

// ---- Common properties
// ---------------------------------------------------------------------------

// A value shared by the whole selection, or "varies".
template <class T>
struct Shared {
    bool varies = false;
    T value{};
};

struct CommonProps {
    std::size_t count = 0;
    std::map<EntityKind, std::size_t> kinds{};  // type breakdown
    Shared<LayerId> layer{};
    Shared<Color> color{};
    Shared<LinetypeId> linetype{};
    Shared<double> linetypeScale{};
    Shared<LineWeight> lineweight{};
    Shared<int> transparency{};
};

// Reads the common properties of `ids` (ids that no longer exist are skipped). With a `kind`
// filter only entities of that kind are included.
CommonProps readCommon(const Document& doc, const std::vector<EntityId>& ids,
                       std::optional<EntityKind> only = std::nullopt);

struct CommonEdit {
    std::optional<LayerId> layer{};
    std::optional<Color> color{};
    std::optional<LinetypeId> linetype{};
    std::optional<double> linetypeScale{};
    std::optional<LineWeight> lineweight{};
    std::optional<int> transparency{};  // percent, ByLayer or ByBlock
    bool empty() const {
        return !layer && !color && !linetype && !linetypeScale && !lineweight && !transparency;
    }
};

// Applies the edit to every entity in `ids` (optionally only one kind). Entities on locked
// layers are skipped and counted in `skippedLocked`. Returns how many were changed.
struct EditSummary {
    std::size_t changed = 0;
    std::size_t skippedLocked = 0;
};
Result<EditSummary> applyCommon(const Document& doc, Transaction& tx,
                                const std::vector<EntityId>& ids, const CommonEdit& edit,
                                std::optional<EntityKind> only = std::nullopt);

// ---- Geometry fields
// -----------------------------------------------------------------------------

enum class FieldType { Number, Angle, Count, Flag };

struct GeometryField {
    std::string id{};     // "radius"; also the name accepted by -PROP
    std::string label{};  // "Radius"
    FieldType type = FieldType::Number;
    bool editable = true;
    double value = 0;  // Angle in degrees; Flag as 0 / 1
};

// Fields for Line (start/end, length, angle), Circle (centre, radius, diameter, circumference,
// area), Arc (centre, radius, angles, length), Polyline (vertex count, the current vertex and its
// X / Y and segment widths, closed, length, area, global width), Hatch (area), Dimension
// (measurement, text position and rotation), Table (position, size, text height, margin),
// Multileader and the other kinds. `vertex` (0-based, clamped) is the polyline's current vertex.
std::vector<GeometryField> geometryFields(const Entity& e, int vertex = 0);
// Sets one editable field. Fails for read-only fields, invalid values (radius <= 0 ...) or an
// entity kind without that field. Per-vertex fields name their vertex after an '@', counted from
// 1: "vertexx@3"; without it the first vertex is meant.
Result<void> setGeometryField(Transaction& tx, EntityId id, const std::string& fieldId,
                              double value);
// "vertexx@3" -> {"vertexx", 2}; a field without '@' -> {field, 0}. The index is -1 when the
// text after '@' is not a whole number of at least 1.
std::pair<std::string, int> splitFieldVertex(const std::string& fieldId);

// ---- Text fields (contents, style, justification ...) ------------------------------------------

// A property that is a word or a sentence, not a number. `choices` (when not empty) lists the
// only accepted values, in the order a drop-down shows them.
struct StringField {
    std::string id{};     // "contents"; also the name accepted by -PROP Set
    std::string label{};  // "Contents"
    std::string value{};
    bool editable = true;
    std::vector<std::string> choices{};
};

// Fields for Text (contents, style, justify), MText (contents, style, attachment), Insert (block
// name, read-only, and one "att:TAG" field per attribute), Dimension and Leader (text override,
// dimension style), Multileader (contents, style) and Table (text style). Empty for other kinds.
std::vector<StringField> stringFields(const Document& doc, const Entity& e);
// Sets one editable text field. Justification and attachment keep the text where it is on the
// sheet. Fails for read-only fields, unknown styles or values outside `choices`.
Result<void> setStringField(const Document& doc, Transaction& tx, EntityId id,
                            const std::string& fieldId, const std::string& value);

// Name of an entity kind for the type breakdown ("Line", "Polyline", ...).
const char* kindLabel(EntityKind kind);
// Parses a number that may be an expression ("100/3", "2*(3+4)") and carry a unit ("2.5m", "30cm",
// "12in"); the result is in millimetres unless `plain` (no unit conversion, used for angles).
Result<double> parseNumberExpr(const std::string& text, bool plain = false);

}  // namespace tbcad::layers
