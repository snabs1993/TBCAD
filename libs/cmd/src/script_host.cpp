#include "tbcad/cmd/script_host.h"

namespace tbcad::cmd {

ScriptHost::ScriptHost(Document& doc, Registry& registry)
    : doc_(doc), registry_(registry), runner_(doc, undo_, *this, registry) {
    undo_.setMessageSink([this](std::string m) { print(m); });
    runner_.setFilterChain(&chain_);
}

ScriptHost::~ScriptHost() = default;

ScriptResult ScriptHost::run(std::string_view command, const std::vector<ScriptItem>& items) {
    ScriptResult r;
    auto s = runner_.start(command);
    if (!s) {
        r.error = s.error().message;
        r.finished = !runner_.active();
        return r;
    }
    r.started = true;
    return pump(items, r);
}

ScriptResult ScriptHost::feed(const std::vector<ScriptItem>& items) {
    ScriptResult r;
    r.started = runner_.active();
    return pump(items, r);
}

ScriptResult ScriptHost::pump(const std::vector<ScriptItem>& items, ScriptResult r) {
    for (const ScriptItem& item : items) {
        if (!runner_.active())
            break;
        if (runner_.submit(item.input) == SubmitResult::Rejected)
            ++r.rejected;
        ++r.consumed;
    }
    r.finished = !runner_.active();
    return r;
}

bool ScriptHost::logContains(std::string_view needle) const {
    for (const std::string& l : log_) {
        if (l.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

std::optional<std::string> ScriptHost::setting(std::string_view key) const {
    for (const auto& [k, v] : settings_) {
        if (k == key)
            return v;
    }
    return std::nullopt;
}

void ScriptHost::setSetting(std::string_view key, std::string value) {
    for (auto& [k, v] : settings_) {
        if (k == key) {
            v = std::move(value);
            return;
        }
    }
    settings_.emplace_back(std::string(key), std::move(value));
}

void ScriptHost::setPreview(const DisplayList* list) {
    if (list)
        preview_ = *list;
    else
        preview_.items.clear();
}

void ScriptHost::promptChanged(const PromptInfo* prompt) {
    if (prompt)
        prompts_.push_back(prompt->display());
}

}  // namespace tbcad::cmd
