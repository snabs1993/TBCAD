#include "common.h"

namespace tbcad::select {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(detail::makePurge());
    (void)registry.add(detail::makePurgeDialog());
    (void)registry.add(detail::makePurgeItems());
    (void)registry.add(detail::makeQSelect());
    (void)registry.add(detail::makeSelectSimilar());
    (void)registry.add(detail::makeIsolate());
    (void)registry.add(detail::makeSelectAll());
    (void)registry.add(detail::makeSelectCommand());
    (void)registry.add(detail::makeQuickProperties());
    (void)registry.add(detail::makeChProp());
    (void)registry.add(detail::makeChange());
    (void)registry.add(detail::makeFilter());
    (void)registry.add(detail::makeHide());
    (void)registry.add(detail::makeUnisolate());
}

}  // namespace tbcad::select
