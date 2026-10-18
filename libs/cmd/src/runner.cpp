#include "tbcad/cmd/runner.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <span>
#include <string>

namespace tbcad::cmd {

namespace {

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return s;
}

PromptInfo::Kind infoKind(detail::PromptKind k) {
    switch (k) {
        case detail::PromptKind::Point:
            return PromptInfo::Kind::Point;
        case detail::PromptKind::Distance:
            return PromptInfo::Kind::Distance;
        case detail::PromptKind::Angle:
            return PromptInfo::Kind::Angle;
        case detail::PromptKind::Real:
            return PromptInfo::Kind::Real;
        case detail::PromptKind::Integer:
            return PromptInfo::Kind::Integer;
        case detail::PromptKind::String:
            return PromptInfo::Kind::String;
        case detail::PromptKind::Keyword:
            return PromptInfo::Kind::Keyword;
        case detail::PromptKind::Selection:
            return PromptInfo::Kind::Selection;
        case detail::PromptKind::Entity:
            return PromptInfo::Kind::Entity;
    }
    return PromptInfo::Kind::Point;
}

bool parseReal(const std::string& s, double& out) {
    if (s.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

bool parseInteger(const std::string& s, long long& out) {
    if (s.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size() || errno == ERANGE)
        return false;
    out = v;
    return true;
}

}  // namespace

CommandRunner::CommandRunner(Document& doc, UndoStack& undo, IHost& host, Registry& registry)
    : doc_(doc), undo_(undo), host_(host), registry_(registry) {}

CommandRunner::~CommandRunner() {
    while (!stack_.empty())
        finishTop(true, {}, false);
}

Result<void> CommandRunner::start(std::string_view nameOrAlias) {
    return startImpl(nameOrAlias, false);
}

Result<void> CommandRunner::startTransparent(std::string_view nameOrAlias) {
    return startImpl(nameOrAlias, true);
}

Result<void> CommandRunner::startImpl(std::string_view nameOrAlias, bool transparent) {
    Command* c = registry_.find(nameOrAlias);
    if (!c)
        return makeError(ErrorCode::NotFound,
                         "Unknown command \"" + std::string(nameOrAlias) + "\"");
    if (transparent) {
        if (stack_.empty())
            return makeError(ErrorCode::InvalidArgument, "No command is running");
        if (!c->isTransparent())
            return makeError(ErrorCode::InvalidArgument,
                             std::string(c->name()) + " cannot be used transparently");
    } else if (!stack_.empty()) {
        return makeError(ErrorCode::Conflict, "A command is already running");
    }
    Active a;
    a.command = c;
    a.ctx = std::make_unique<CommandContext>(doc_, host_, undo_, registry_, std::string(c->name()),
                                             transparent ? stack_.back().ctx.get() : nullptr);
    a.task = c->run(*a.ctx);
    stack_.push_back(std::move(a));
    if (!transparent)
        lastCommand_ = std::string(c->name());
    stack_.back().task.resume();
    afterResume();
    return {};
}

void CommandRunner::afterResume() {
    if (stack_.empty())
        return;
    Active& a = stack_.back();
    if (a.task.done()) {
        finishTop(false, {}, true);
    } else if (a.ctx->hasPending()) {
        a.ctx->publishPending();
        refreshPrompt();
    } else {
        finishTop(true, "the command suspended without asking for input", true);
    }
}

void CommandRunner::finishTop(bool failed, std::string_view error, bool notify) {
    Active a = std::move(stack_.back());
    stack_.pop_back();
    std::string message(error);
    if (a.task) {
        try {
            a.task.rethrowIfFailed();
        } catch (const std::exception& e) {
            failed = true;
            message = e.what();
        } catch (...) {
            failed = true;
            message = "unknown error";
        }
    }
    // The coroutine frame goes first: its locals may still refer to the context.
    a.task = Task{};
    try {
        a.ctx->finish(failed);
    } catch (const std::exception& e) {
        failed = true;
        message = e.what();
    }
    if (notify) {
        if (failed && !message.empty())
            host_.print("Command " + std::string(a.command->name()) + " failed: " + message);
        host_.setPreview(nullptr);
        feedback_ = FilterFeedback{};
        host_.feedbackChanged(feedback_);
        refreshPrompt();
    }
}

void CommandRunner::refreshPrompt() {
    collected_.clear();
    previewList_.items.clear();
    if (stack_.empty() || !stack_.back().ctx->hasPending()) {
        prompt_.reset();
        host_.promptChanged(nullptr);
        return;
    }
    const detail::PromptRequest& req = *stack_.back().ctx->pending();
    PromptInfo info;
    info.kind = infoKind(req.kind);
    info.text = req.text;
    info.keywords = req.keywords;
    info.command = stack_.back().ctx->commandName();
    switch (req.kind) {
        case detail::PromptKind::Distance:
            if (req.defaultReal)
                info.defaultText = formatDouble(*req.defaultReal);
            break;
        case detail::PromptKind::Angle:
            if (req.defaultReal)
                info.defaultText = formatAngle(*req.defaultReal, 6, AngleUnit::Degrees);
            break;
        case detail::PromptKind::Real:
            if (req.defaultReal)
                info.defaultText = formatDouble(*req.defaultReal);
            break;
        case detail::PromptKind::Integer:
            if (req.defaultInt)
                info.defaultText = std::to_string(*req.defaultInt);
            break;
        case detail::PromptKind::String:
        case detail::PromptKind::Keyword:
            info.defaultText = req.defaultText;
            break;
        default:
            break;
    }
    prompt_ = std::move(info);
    host_.promptChanged(&*prompt_);
}

Vec2 CommandRunner::filtered(const RawInput& raw, bool click, const std::optional<Vec2>& base) {
    if (!chain_) {
        feedback_ = FilterFeedback{};
        feedback_.hasPoint = true;
        feedback_.base = base;
        feedback_.point = raw.worldCursor;
        return raw.worldCursor;
    }
    PointQuery q;
    q.base = base;
    q.click = click;
    q.shift = raw.shift;
    q.pixelSize = raw.pixelSize;
    const Vec2 p = chain_->apply(q, raw.worldCursor, feedback_);
    feedback_.hasPoint = true;
    feedback_.base = base;
    feedback_.point = p;
    return p;
}

PointParseContext CommandRunner::parseContext(const detail::PromptRequest& req) const {
    PointParseContext c;
    c.base = req.base;
    c.lastPoint = lastPoint_;
    c.cursor = cursor_;
    c.unit = doc_.header().units;
    return c;
}

void CommandRunner::pointerMoved(const RawInput& raw) {
    cursor_ = raw.worldCursor;
    if (stack_.empty() || !stack_.back().ctx->hasPending())
        return;
    const detail::PromptRequest& req = *stack_.back().ctx->pending();
    const bool pointish = req.kind == detail::PromptKind::Point ||
                          req.kind == detail::PromptKind::Distance ||
                          req.kind == detail::PromptKind::Angle;
    if (!pointish)
        return;
    Vec2 p = filtered(raw, false, req.base);
    host_.feedbackChanged(feedback_);
    if (req.preview) {
        previewList_.items.clear();
        req.preview(p, previewList_);
        host_.setPreview(&previewList_);
    }
}

void CommandRunner::textEdited(const std::string& text) {
    if (stack_.empty() || !stack_.back().ctx->hasPending())
        return;
    const detail::PromptRequest& req = *stack_.back().ctx->pending();
    if ((req.kind != detail::PromptKind::String && req.kind != detail::PromptKind::Integer) ||
        !req.textPreview)
        return;
    previewList_.items.clear();
    if (!text.empty())
        req.textPreview(text, previewList_);
    host_.setPreview(&previewList_);
}

bool CommandRunner::undo() {
    if (active()) {
        host_.print("Finish the current command first.");
        return false;
    }
    const std::string label = undo_.undoLabel();
    if (!undo_.undo(doc_))
        return false;
    host_.print("Undo " + label);
    return true;
}

bool CommandRunner::redo() {
    if (active()) {
        host_.print("Finish the current command first.");
        return false;
    }
    const std::string label = undo_.redoLabel();
    if (!undo_.redo(doc_))
        return false;
    host_.print("Redo " + label);
    return true;
}

SubmitResult CommandRunner::submit(const Input& input) {
    if (stack_.empty()) {
        std::string name;
        if (input.kind == Input::Kind::Enter) {
            name = lastCommand_;
        } else if (input.kind == Input::Kind::Text) {
            name = trim(input.text);
            if (name.empty())
                name = lastCommand_;
        } else {
            return SubmitResult::Ignored;
        }
        if (name.empty())
            return SubmitResult::Ignored;
        auto r = start(name);
        if (!r) {
            host_.print(r.error().message);
            return SubmitResult::UnknownCommand;
        }
        return stack_.empty() ? SubmitResult::CommandFinished : SubmitResult::CommandStarted;
    }
    return answerTop(input);
}

SubmitResult CommandRunner::answerTop(const Input& in) {
    Active& top = stack_.back();
    CommandContext& ctx = *top.ctx;
    if (!ctx.hasPending())
        return SubmitResult::Ignored;
    const detail::PromptRequest req = *ctx.pending();
    detail::PromptAnswer ans;
    using K = detail::PromptKind;

    auto reject = [&](const std::string& why) {
        host_.print(why);
        return SubmitResult::Rejected;
    };
    auto complete = [&](detail::PromptAnswer a) {
        const std::size_t depth = stack_.size();
        if (a.status == Status::Cancel)
            ctx.markCancelled();
        ctx.setLastPoint(lastPoint_);
        auto h = ctx.deliver(std::move(a));
        h.resume();
        afterResume();
        return stack_.size() < depth ? SubmitResult::CommandFinished : SubmitResult::Accepted;
    };
    auto accepts = [&](EntityId id) {
        const Entity* e = doc_.find(id);
        return e && (!req.filter || req.filter(*e));
    };
    auto addCollected = [&](EntityId id) {
        for (EntityId x : collected_) {
            if (x == id)
                return;
        }
        collected_.push_back(id);
    };
    // "3 found, 5 total", as AutoCAD says it after every pick or window.
    auto reportFound = [&](std::size_t n) {
        host_.print(std::to_string(n) + " found" +
                    (collected_.size() > n ? ", " + std::to_string(collected_.size()) + " total"
                                           : std::string()));
    };
    // A picked member brings its whole group along (GROUP).
    auto addWithGroup = [&](EntityId id) {
        for (EntityId m : expandGroups(doc_, std::span<const EntityId>(&id, 1))) {
            if (m == id || accepts(m))
                addCollected(m);
        }
    };

    switch (in.kind) {
        case Input::Kind::Cancel:
            ans.status = Status::Cancel;
            return complete(std::move(ans));

        case Input::Kind::Enter: {
            switch (req.kind) {
                case K::Selection:
                    if (collected_.empty()) {
                        ans.status = Status::None;
                    } else {
                        ans.status = Status::Ok;
                        ans.ids = collected_;
                        previousSelection_ = collected_;
                    }
                    return complete(std::move(ans));
                case K::Point:
                    if (!req.allowNone)
                        return SubmitResult::Rejected;
                    ans.status = Status::None;
                    return complete(std::move(ans));
                case K::Distance:
                case K::Angle:
                case K::Real:
                    if (req.defaultReal) {
                        ans.status = Status::Ok;
                        ans.real = *req.defaultReal;
                    } else {
                        ans.status = Status::None;
                    }
                    return complete(std::move(ans));
                case K::Integer:
                    if (req.defaultInt) {
                        ans.status = Status::Ok;
                        ans.integer = *req.defaultInt;
                    } else {
                        ans.status = Status::None;
                    }
                    return complete(std::move(ans));
                case K::String:
                    if (req.defaultText) {
                        ans.status = Status::Ok;
                        ans.text = *req.defaultText;
                    } else if (req.allowEmptyString) {
                        ans.status = Status::Ok;
                    } else {
                        ans.status = Status::None;
                    }
                    return complete(std::move(ans));
                case K::Keyword:
                    if (req.defaultText) {
                        ans.status = Status::Keyword;
                        ans.keyword = *req.defaultText;
                    } else {
                        ans.status = Status::None;
                    }
                    return complete(std::move(ans));
                case K::Entity:
                    ans.status = Status::None;
                    return complete(std::move(ans));
            }
            return SubmitResult::Ignored;
        }

        case Input::Kind::Text: {
            if (req.kind == K::String) {
                if (trim(in.text).empty())
                    return answerTop(Input::enter());
                ans.status = Status::Ok;
                ans.text = in.text;
                return complete(std::move(ans));
            }
            const std::string text = trim(in.text);
            if (text.empty())
                return answerTop(Input::enter());
            if (text[0] == '\'') {
                auto r = startTransparent(std::string_view(text).substr(1));
                if (!r)
                    return reject(r.error().message);
                return SubmitResult::Accepted;
            }
            for (const auto& [typed, keyword] : req.keywordAliases) {
                if (matchKeyword({typed}, text)) {
                    ans.status = Status::Keyword;
                    ans.keyword = keyword;
                    return complete(std::move(ans));
                }
            }
            if (auto kw = matchKeyword(req.keywords, text)) {
                ans.status = Status::Keyword;
                ans.keyword = *kw;
                return complete(std::move(ans));
            }
            if (req.kind == K::Selection) {
                // AutoCAD habits at "Select objects:": All, Last, Previous.
                const std::string up = upper(text);
                std::vector<EntityId> found;
                if (up == "ALL" || up == "A") {
                    doc_.forEachEntity([&](const Entity& e) {
                        if (e.common.visible &&
                            doc_.ownerBlock(e.common.id) == host_.drawingOwner() &&
                            accepts(e.common.id))
                            found.push_back(e.common.id);
                    });
                } else if (up == "LAST" || up == "L") {
                    const Entity* last = nullptr;
                    doc_.forEachEntity([&](const Entity& e) {
                        if (e.common.visible &&
                            doc_.ownerBlock(e.common.id) == host_.drawingOwner() &&
                            accepts(e.common.id) && (!last || e.common.id > last->common.id))
                            last = &e;
                    });
                    if (last)
                        found.push_back(last->common.id);
                } else if (up == "PREVIOUS" || up == "P") {
                    for (EntityId id : previousSelection_)
                        if (accepts(id))
                            found.push_back(id);
                } else {
                    return reject("Invalid option \"" + text +
                                  "\". Pick objects, or type All, Last or Previous.");
                }
                std::size_t added = 0;
                for (EntityId id : found) {
                    addWithGroup(id);
                    ++added;
                }
                if (added == 0)
                    return reject("No objects found.");
                host_.print(std::to_string(added) + " found, " + std::to_string(collected_.size()) +
                            " total");
                if (req.single) {
                    ans.status = Status::Ok;
                    ans.ids = collected_;
                    return complete(std::move(ans));
                }
                return SubmitResult::Accepted;
            }
            switch (req.kind) {
                case K::Point: {
                    const PointParseContext pc = parseContext(req);
                    std::optional<Vec2> p;
                    if (parser_)
                        p = parser_->parsePoint(text, pc);
                    if (!p)
                        p = parsePointText(text, pc);
                    if (!p && req.freeText) {
                        ans.status = Status::Keyword;
                        ans.keyword = text;
                        return complete(std::move(ans));
                    }
                    if (!p)
                        return reject("Invalid point \"" + text + "\".");
                    lastPoint_ = *p;
                    ans.status = Status::Ok;
                    ans.point = *p;
                    return complete(std::move(ans));
                }
                case K::Distance: {
                    double v = 0;
                    auto len = parseLength(text, doc_.header().units);
                    if (len) {
                        v = len.value();
                    } else if (req.base) {
                        const PointParseContext pc = parseContext(req);
                        std::optional<Vec2> p;
                        if (parser_)
                            p = parser_->parsePoint(text, pc);
                        if (!p)
                            p = parsePointText(text, pc);
                        if (!p)
                            return reject("Invalid distance \"" + text + "\".");
                        lastPoint_ = *p;
                        v = distance(*req.base, *p);
                    } else {
                        return reject("Invalid distance \"" + text + "\".");
                    }
                    if (!req.allowNegative && v < 0)
                        return reject("The distance must not be negative.");
                    if (!req.allowZero && v == 0)
                        return reject("The distance must not be zero.");
                    ans.status = Status::Ok;
                    ans.real = v;
                    return complete(std::move(ans));
                }
                case K::Angle: {
                    double a = 0;
                    auto ang = parseAngle(text);
                    if (ang) {
                        a = ang.value();
                    } else if (req.base) {
                        auto p = parsePointText(text, parseContext(req));
                        if (!p)
                            return reject("Invalid angle \"" + text + "\".");
                        lastPoint_ = *p;
                        a = normalizeAngle(std::atan2(p->y - req.base->y, p->x - req.base->x));
                    } else {
                        return reject("Invalid angle \"" + text + "\".");
                    }
                    ans.status = Status::Ok;
                    ans.real = a;
                    return complete(std::move(ans));
                }
                case K::Real: {
                    double v = 0;
                    if (!parseReal(text, v))
                        return reject("Invalid number \"" + text + "\".");
                    if ((req.min && v < *req.min) || (req.max && v > *req.max))
                        return reject("The value is out of range.");
                    ans.status = Status::Ok;
                    ans.real = v;
                    return complete(std::move(ans));
                }
                case K::Integer: {
                    long long v = 0;
                    if (!parseInteger(text, v))
                        return reject("Invalid integer \"" + text + "\".");
                    if ((req.min && static_cast<double>(v) < *req.min) ||
                        (req.max && static_cast<double>(v) > *req.max))
                        return reject("The value is out of range.");
                    ans.status = Status::Ok;
                    ans.integer = v;
                    return complete(std::move(ans));
                }
                default: {
                    std::string why = "Invalid option \"" + text + "\".";
                    if (!req.keywords.empty()) {
                        why += " Options:";
                        for (std::size_t i = 0; i < req.keywords.size(); ++i)
                            why += (i ? ", " : " ") + req.keywords[i];
                        why += ".";
                    }
                    return reject(why);
                }
            }
        }

        case Input::Kind::Point: {
            if (req.kind != K::Point && req.kind != K::Distance && req.kind != K::Angle)
                return SubmitResult::Ignored;
            const Vec2 p = filtered(in.raw, true, req.base);
            lastPoint_ = p;
            ans.status = Status::Ok;
            if (req.kind == K::Point) {
                ans.point = p;
            } else if (!req.base) {
                return reject("Type a value; there is no base point to measure from.");
            } else if (req.kind == K::Distance) {
                ans.real = distance(*req.base, p);
            } else {
                ans.real = normalizeAngle(std::atan2(p.y - req.base->y, p.x - req.base->x));
            }
            return complete(std::move(ans));
        }

        case Input::Kind::Entity: {
            if (req.kind == K::Entity) {
                if (!accepts(in.entity))
                    return reject("That object cannot be used here.");
                ans.status = Status::Ok;
                ans.entity = in.entity;
                ans.point = in.raw.worldCursor;
                ans.shift = in.raw.shift;
                return complete(std::move(ans));
            }
            if (req.kind == K::Selection) {
                if (!accepts(in.entity))
                    return reject("That object cannot be selected here.");
                const std::size_t before = collected_.size();
                addWithGroup(in.entity);
                if (req.single) {
                    ans.status = Status::Ok;
                    ans.ids = collected_;
                    return complete(std::move(ans));
                }
                reportFound(collected_.size() - before);
                return SubmitResult::Accepted;
            }
            return SubmitResult::Ignored;
        }

        case Input::Kind::Selection: {
            if (req.kind != K::Selection)
                return SubmitResult::Ignored;
            std::size_t added = 0;
            const std::size_t before = collected_.size();
            for (EntityId id : in.ids) {
                if (accepts(id)) {
                    addWithGroup(id);
                    ++added;
                }
            }
            if (added == 0)
                return reject("No valid objects selected.");
            if (req.single) {
                ans.status = Status::Ok;
                ans.ids = collected_;
                return complete(std::move(ans));
            }
            reportFound(collected_.size() - before);
            return SubmitResult::Accepted;
        }
    }
    return SubmitResult::Ignored;
}

}  // namespace tbcad::cmd
