#include "common.h"

namespace tbcad::select {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(detail::makePurge());
    (void)registry.add(detail::makeQSelect());
    (void)registry.add(detail::makeSelectSimilar());
    (void)registry.add(detail::makeIsolate());
    (void)registry.add(detail::makeHide());
    (void)registry.add(detail::makeUnisolate());
}

}  // namespace tbcad::select
