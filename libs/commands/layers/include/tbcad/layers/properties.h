#pragma once

#include <map>
#include <optional>
#include <string>
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
// area), Arc (centre, radius, angles, length) and Polyline (vertex count, closed, length, area,
// global width). Empty for other kinds.
std::vector<GeometryField> geometryFields(const Entity& e);
// Sets one editable field. Fails for read-only fields, invalid values (radius <= 0 ...) or an
// entity kind without that field.
Result<void> setGeometryField(Transaction& tx, EntityId id, const std::string& fieldId,
                              double value);

// Name of an entity kind for the type breakdown ("Line", "Polyline", ...).
const char* kindLabel(EntityKind kind);
// Parses a number that may be an expression ("100/3", "2*(3+4)") and carry a unit ("2.5m", "30cm",
// "12in"); the result is in millimetres unless `plain` (no unit conversion, used for angles).
Result<double> parseNumberExpr(const std::string& text, bool plain = false);

}  // namespace tbcad::layers
