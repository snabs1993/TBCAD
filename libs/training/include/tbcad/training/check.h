#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/training/lesson.h"

namespace tbcad::training {

// How the user started a command.
enum class Source { Other, Button, Name, Alias, Thai };

// `word` is what was typed to start `command` (empty when picked from a button).
Source classifySource(std::string_view word, bool fromButton, std::string_view command);

// Something the user did, as the application reports it.
struct Event {
    enum class Kind { Started, Ended, Undo, Redo };
    Kind kind = Kind::Started;
    std::string command{};
    Source source = Source::Other;
    bool transparent = false;
    bool cancelled = false;
};

// What the checks look at.
struct Facts {
    const Document* doc = nullptr;
    std::vector<EntityId> selection{};
    std::optional<BBox2> view{};  // the part of the drawing in view
    std::string runningCommand{};
    std::string prompt{};
    std::span<const Event> events{};  // since the lesson started
};

bool passes(const Check& check, const Facts& facts);

}  // namespace tbcad::training
