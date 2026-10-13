#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/expr/expr.h"

namespace tbcad::blocks {

// Parametric assemblies: a block with named parameters (BlockDef::assembly). Every Insert holds a
// value per parameter (InsertData::asmValues); bindings turn those values, through formulas, into
// dynamic parameter values, attribute texts, and settings of inserts nested in the block.

// Index of the parameter called `name` (ASCII case-insensitive), or -1.
int asmFind(const AssemblyDef& def, std::string_view name);

// Value of every parameter for an insert, in parameter order: the insert's own value, or the
// default; numbers clamped to the limits (integers rounded), text for a choice that is not
// offered replaced by the first choice.
std::vector<AsmValue> asmEffectiveValues(const AssemblyDef& def, const InsertData& insert);

// A setting a binding makes on an insert nested in the assembly's block.
struct NestedSet {
    AsmBinding::Kind kind = AsmBinding::Kind::Nested;
    EntityId entity{};
    std::string target{};
    expr::Value value{};
};

struct AsmResolved {
    // The insert with the bound dynamic values and attribute texts filled in.
    InsertData insert{};
    std::vector<NestedSet> nested{};
    // One message per binding that could not be evaluated (the binding is skipped).
    std::vector<std::string> errors{};
};
AsmResolved resolveAssembly(const Document& doc, const BlockDef& def, const InsertData& insert);

// Applies the nested settings to the copies in `items` (matched by entity id).
void applyNested(const Document& doc, std::vector<Entity>& items,
                 const std::vector<NestedSet>& nested);

// The insert with one parameter changed. A number given for a Choice or text for a Number is
// converted when it can be. Errors for a block without an assembly, an unknown parameter or a
// value outside what the parameter allows (choice not offered, number outside the limits).
Result<Entity> asmSet(const Document& doc, const Entity& insert, std::string_view name,
                      const expr::Value& value);
// The insert with every value of the preset applied.
Result<Entity> asmApplyPreset(const Document& doc, const Entity& insert, std::string_view preset);

// Checks a definition: formulas parse, referenced names exist, targets exist. One message per
// problem; empty when the definition is sound.
std::vector<std::string> asmValidate(const Document& doc, const BlockDef& def);

}  // namespace tbcad::blocks
