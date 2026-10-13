// Named page setups (Q82): the plot options of a sheet saved in the drawing. A setup holds paper,
// orientation, plot area, scale, offsets, lineweights, colour mode and the plot style table name.
// Setups live in Header::custom["tbcad.pagesetups"], so they are saved with the drawing and
// changes are undoable. A layout can have one assigned (Layout::pageSetup), and so can model space.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/export/plot.h"
#include "tbcad/layout/layout.h"

namespace tbcad::layout {

inline constexpr const char* kPageSetupKey = "tbcad.pagesetups";

struct PageSetup {
    std::string name{};
    // The plot style table is stored by name (`styleName`, "" or "none" = plot as drawn); the
    // table pointer of `settings` is left null here and resolved by whoever plots.
    exportlib::PlotSettings settings{};
    std::string styleName{};
};

// All setups in creation order.
std::vector<PageSetup> pageSetups(const Document& doc);
std::optional<PageSetup> findPageSetup(const Document& doc, std::string_view name);
// 1-64 characters, no control characters or leading / trailing space; unique ignoring case.
Result<void> validatePageSetupName(const Document& doc, std::string_view name,
                                   std::string_view ignore = {});

// Creates the setup, or replaces the one with the same name (ignoring case).
Result<void> savePageSetup(Transaction& tx, const PageSetup& setup);
// Layouts that used it fall back to no setup.
Result<void> deletePageSetup(Transaction& tx, const std::string& name);
// Layouts and model space follow the new name.
Result<void> renamePageSetup(Transaction& tx, const std::string& name, const std::string& newName);

// Assigns the setup to a layout (empty `setup` clears it). Assigning also gives the layout the
// setup's paper size and orientation. The setup must exist.
Result<void> assignPageSetup(Transaction& tx, const std::string& layoutName,
                             const std::string& setup);
// The same for model space; there is no paper to change.
Result<void> assignModelPageSetup(Transaction& tx, const std::string& setup);
// Name of the setup assigned to model space, or empty.
std::string modelPageSetup(const Document& doc);

// The setup a layout uses (nullopt when none is assigned or it no longer exists).
std::optional<PageSetup> pageSetupOfLayout(const Document& doc, const std::string& layoutName);
std::optional<PageSetup> pageSetupOfModel(const Document& doc);

// Finds a plot style table by name (the file system lives outside this library).
using StyleResolver =
    std::function<std::shared_ptr<const plotstyle::PlotStyleTable>(const std::string& name)>;
// The settings to plot with when a page setup applies: the setup's own, with the style table
// resolved through `resolver` ("" and "none" plot as drawn). Without a resolver the table of `base`
// stays. A Display area keeps `base.window`, the view the caller supplies.
exportlib::PlotSettings settingsFor(const exportlib::PlotSettings& base, const PageSetup& setup,
                                    const StyleResolver& resolver = {});
// The reverse: a setup holding the plot settings as they are, with the style name given.
PageSetup makePageSetup(std::string name, const exportlib::PlotSettings& settings,
                        std::string styleName);

}  // namespace tbcad::layout
