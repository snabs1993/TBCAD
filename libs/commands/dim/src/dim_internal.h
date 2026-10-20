#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/core/core.h"

namespace tbcad::dimcmd {

std::unique_ptr<cmd::Command> makeDimStyleCommand();
std::unique_ptr<cmd::Command> makeDimSetupCommand();
std::unique_ptr<cmd::Command> makeDimOverrideCommand();
std::unique_ptr<cmd::Command> makeDimStylesPanelCommand();
std::unique_ptr<cmd::Command> makeSmartDimCommand();
std::unique_ptr<cmd::Command> makeMLeaderCommand();
std::unique_ptr<cmd::Command> makeMLeaderAlignCommand();
std::unique_ptr<cmd::Command> makeMLeaderCollectCommand();
std::unique_ptr<cmd::Command> makeMLeaderEditCommand();
std::unique_ptr<cmd::Command> makeMLeaderEditAddCommand();
std::unique_ptr<cmd::Command> makeMLeaderEditRemoveCommand();
std::unique_ptr<cmd::Command> makeDimReassocCommand();
std::unique_ptr<cmd::Command> makeDimRegenCommand();
std::unique_ptr<cmd::Command> makeDimUpdateCommand();
std::unique_ptr<cmd::Command> makeDimInspectCommand();
std::unique_ptr<cmd::Command> makeDimDisassocCommand();
std::unique_ptr<cmd::Command> makeMLeaderStyleCommand();

// The multileader style in use: the dimension style that carries its arrow, landing gap and
// text style, and the default content type ("Text", "Block" or "None").
struct MLeaderStyle {
    DimStyleId dim{};
    std::string content = "Text";
    std::string block{};      // block content: the block MLEADER offers first ("" = ask)
    bool annotative = false;  // new multileaders follow the annotation scale
    long long maxPoints = 2;  // points of a leader line, arrowhead and landing included
    bool dogleg = true;       // a horizontal landing between the leader and the note
    double doglegLength = 0;  // 0 = the arrow size
};
MLeaderStyle currentMLeaderStyle(const Document& doc);
// MLEADERSTYLE [MLS] opens the Multileader Style Manager; -MLEADERSTYLE edits on the command line.
std::unique_ptr<cmd::Command> makeMLeaderStylePanelCommand();
// The stored multileader styles (Standard is implied), the current one ("" = Standard), the
// content type of one and the name of the dimension style that carries its arrow and text.
std::vector<std::string> mleaderStyleNames(const Document& doc);
std::string currentMLeaderStyleName(const Document& doc);
std::string mleaderContent(const Document& doc, const std::string& name);
std::string mleaderCompanionName(const std::string& name);
std::unique_ptr<cmd::Command> makeContinueCommand();
std::unique_ptr<cmd::Command> makeBaselineCommand();
std::unique_ptr<cmd::Command> makeTextEditCommand();
std::unique_ptr<cmd::Command> makeSpaceCommand();
std::unique_ptr<cmd::Command> makeEditCommand();
// Works the automatic DIMBREAK gaps of a dimension out again (installed as the document's
// dimension break refresh).
void refreshAutoBreaks(const Document& doc, Entity& e);
std::unique_ptr<cmd::Command> makeBreakCommand();
std::unique_ptr<cmd::Command> makeJogLineCommand();
std::unique_ptr<cmd::Command> makeQuickDimCommand();

// Shared by the dimension commands (dim_helpers.cpp).
void previewEntity(cmd::CommandContext& ctx, Entity e, DisplayList& out);
Entity dimEntity(const Document& doc, DimensionData d);
void previewDim(cmd::CommandContext& ctx, const DimensionData& d, DisplayList& out);
bool addDim(cmd::CommandContext& ctx, DimensionData d);
// A drawing viewed from far away or with an annotation scale gets a dimension style scaled to match
// before its first dimension or leader (the same sizing new text gets). Part of the command's
// transaction, so Esc takes it back.
void scaleFirstAnnotationToView(cmd::CommandContext& ctx);
void previewLine(cmd::CommandContext& ctx, Vec2 a, Vec2 b, DisplayList& out);
bool isLine(const Entity& e);
// The straight segment of a line, or of a polyline nearest to `pick`; false for anything else.
bool straightPieceAt(const Entity& e, Vec2 pick, Vec2& a, Vec2& b);
bool isCircleOrArc(const Entity& e);
bool isMeasurable(const Entity& e);
bool measurableEnds(const Entity& e, Vec2 pick, Vec2& a, Vec2& b);
// Centre and radius of a circle, an arc, or the bulged polyline segment nearest to `pick`.
bool circleOf(const Entity& e, Vec2& c, double& r, const Vec2* pick = nullptr);
double autoRotation(Vec2 p1, Vec2 p2, Vec2 c, double ucs = 0);
LeaderData leaderOf(const Document& doc, std::vector<Vec2> pts, const std::string& plain);
LeaderData leaderOf(const Document& doc, std::vector<Vec2> pts, const std::string& plain,
                    DimStyleId style);

// Applies one DIMSTYLE property. `key` is one of propertyKeywords() (matched case-insensitively);
// `value` is the text the user typed ('_' stands for a space in prefix and suffix). Returns an
// empty string on success, else the reason.
std::string applyProperty(const Document& doc, DimStyle& style, std::string_view key,
                          std::string_view value);

const std::vector<std::string>& propertyKeywords();
// Every property as (keyword, value text), in the order of propertyKeywords() (Compare).
std::vector<std::pair<std::string, std::string>> describeStyle(const Document& doc,
                                                               const DimStyle& style);

}  // namespace tbcad::dimcmd
