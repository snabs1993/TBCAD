#include "tbcad/mcp/tools.h"

#include "tools_common.h"

namespace tbcad::mcp {

const Tool* ToolSet::find(const std::string& name) const {
    for (const auto& t : tools_) {
        if (t.name == name)
            return &t;
    }
    return nullptr;
}

std::vector<const Tool*> ToolSet::list(DrawingSession& session) const {
    std::vector<const Tool*> out;
    for (const auto& t : tools_) {
        if (!t.available || t.available(session))
            out.push_back(&t);
    }
    return out;
}

void registerDrawingTools(ToolSet& tools) {
    detail::registerReadTools(tools);
    detail::registerEditTools(tools);
    detail::registerModifyTools(tools);
    detail::registerLayoutTools(tools);
    detail::registerBlockTools(tools);
    detail::registerOpsTools(tools);
    detail::registerConstraintTools(tools);
    detail::registerLayerTools(tools);
    detail::registerAssemblyTools(tools);
    detail::registerLevelTools(tools);
    detail::registerCommandTools(tools);
    detail::registerFileTools(tools);
}

}  // namespace tbcad::mcp
