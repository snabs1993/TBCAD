#include "tbcad/cmd/runner.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <span>
#include <string>
#include <unordered_set>

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
    ++entry_;
    auto r = startImpl(nameOrAlias, false);
    --entry_;
    if (entry_ == 0)
        pumpScript();
    return r;
}

void CommandRunner::queueScript(std::vector<std::string> lines, int repeat) {
    constexpr std::size_t kMaxLines = 2'000'000;
    // (in front of what is left: a script that runs another script finishes that one first)
    std::deque<std::string> add;
    for (int i = 0; i < repeat && add.size() < kMaxLines; ++i)
        for (const std::string& l : lines)
            add.push_back(l);
    script_.insert(script_.begin(), std::make_move_iterator(add.begin()),
                   std::make_move_iterator(add.end()));
}

void CommandRunner::pumpScript() {
    ++entry_;
    while (!script_.empty()) {
        std::string line = std::move(script_.front());
        script_.pop_front();
        // A script line "CIRCLE 0,0 5" starts the command and answers its prompts one by one.
        if (stack_.empty() && !line.empty() && line.front() != '\'' &&
            line.find_first_of(" \t") != std::string::npos) {
            const std::size_t sp = line.find_first_of(" \t");
            if (registry_.find(line.substr(0, sp))) {
                std::deque<std::string> rest;
                std::size_t i = sp;
                while (i < line.size()) {
                    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                        ++i;
                    const std::size_t j = line.find_first_of(" \t", i);
                    if (i < line.size())
                        rest.push_back(line.substr(i, j == std::string::npos ? j : j - i));
                    i = j == std::string::npos ? line.size() : j;
                }
                // a trailing space is an Enter in a script
                if (line.back() == ' ')
                    rest.emplace_back();
                script_.insert(script_.begin(), rest.begin(), rest.end());
                line = line.substr(0, sp);
            }
        }
        if (line.empty())
            (void)submitInput(Input::enter());
        else
            (void)submitInput(Input::typed(line));
    }
    --entry_;
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
    } else {
        lastSeg_.reset();
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
    // Like AutoCAD, a drawing command that starts with objects selected drops the selection
    // (the grips go away); the editing commands take it as their first answer instead.
    if (!transparent && !stack_.empty() && stack_.back().ctx->hasPending() &&
        stack_.back().ctx->pending()->kind == detail::PromptKind::Point &&
        !host_.selection().empty()) {
        static const std::unordered_set<std::string_view> kDrawing = {
            "LINE",   "PLINE", "POLYLINE", "RECTANG", "RECTANGLE", "POLYGON",
            "CIRCLE", "ARC",   "ELLIPSE",  "SPLINE",  "XLINE",     "RAY",
            "POINT",  "DONUT", "TEXT",     "MTEXT",   "MLINE",     "REVCLOUD"};
        if (kDrawing.count(c->name()))
            host_.setSelection({});
    }
    return {};
}

void CommandRunner::afterResume() {
    if (stack_.empty())
        return;
    Active& a = stack_.back();
    if (a.task.done()) {
        finishTop(false, {}, true);
    } else if (a.ctx->hasPending()) {
        multipleHadPrompt_ = true;
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
        // Like AutoCAD: a modify command that used the pickfirst selection leaves nothing
        // selected, unless it chose a selection of its own. Grip edits and the Properties panel
        // keep it.
        static const std::unordered_set<std::string_view> kConsumes = {
            "MOVE",      "COPY",       "ROTATE",    "SCALE",   "MIRROR", "STRETCH", "ARRAY",
            "ARRAYRECT", "ARRAYPOLAR", "ARRAYPATH", "EXPLODE", "ALIGN",  "ERASE"};
        if (!a.ctx->usedPreselection().empty() && kConsumes.count(a.command->name()) &&
            host_.selection() == a.ctx->usedPreselection())
            host_.setSelection({});
        feedback_ = FilterFeedback{};
        host_.feedbackChanged(feedback_);
        refreshPrompt();
    }
    // MULTIPLE: the next run of the repeated command. A run that never asked for anything would
    // repeat forever, so it ends the repeating.
    if (!multiple_.empty() && stack_.empty()) {
        if (failed || a.ctx->cancelled() || !multipleHadPrompt_) {
            multiple_.clear();
        } else {
            multipleHadPrompt_ = false;
            const std::string again = multiple_;
            (void)startImpl(again, false);
        }
    }
}

void CommandRunner::refreshPrompt() {
    collected_.clear();
    collectedSet_.clear();
    shape_ = SelShape::None;
    shapePts_.clear();
    removeMode_ = false;
    selUndo_.clear();
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
            info.editor = req.editor;
            break;
        default:
            break;
    }
    prompt_ = std::move(info);
    host_.promptChanged(&*prompt_);
}

Vec2 CommandRunner::filtered(const RawInput& raw, bool click, const std::optional<Vec2>& base,
                             bool snap) {
    if (!chain_ || !snap) {
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
    if (base && lastSeg_ && distance(*base, lastSeg_->second) < 1e-9 &&
        distance(lastSeg_->first, lastSeg_->second) > 1e-9)
        q.lastSegment = lastSeg_->second - lastSeg_->first;
    const Vec2 p = chain_->apply(q, raw.worldCursor, feedback_);
    feedback_.hasPoint = true;
    feedback_.base = base;
    feedback_.point = p;
    return p;
}

double CommandRunner::ucsTurn(const detail::PromptRequest& req) const {
    return req.angleInUcs && parser_ ? parser_->ucsAngle() : 0;
}

PointParseContext CommandRunner::parseContext(const detail::PromptRequest& req) const {
    PointParseContext c;
    c.base = req.base;
    // FROM's offset and M2P's second point are measured from the point just given.
    if (ptMod_ == PtMod::FromOffset || ptMod_ == PtMod::Mid2)
        c.base = ptModA_;
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
    Vec2 p = filtered(raw, false, req.base, req.snap);
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
    if (input.kind == Input::Kind::Cancel)
        script_.clear();
    ++entry_;
    const SubmitResult r = submitInput(input);
    --entry_;
    if (entry_ == 0)
        pumpScript();
    return r;
}

SubmitResult CommandRunner::submitInput(const Input& input) {
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
    // All and Last take what the user could pick by hand: shown, in this space, and not on a
    // locked, frozen or switched-off layer.
    auto pickable = [&](const Entity& e) {
        if (!e.common.visible || doc_.ownerBlock(e.common.id) != host_.drawingOwner())
            return false;
        const EffectiveLayer l = doc_.effective(e.common.layer);
        return l.on && !l.frozen && !l.locked;
    };
    auto addCollected = [&](EntityId id) {
        if (collectedSet_.insert(id).second)
            collected_.push_back(id);
    };
    // "3 found, 5 total", as AutoCAD says it after every pick or window.
    auto reportFound = [&](std::size_t n) {
        host_.print(std::to_string(n) + " found" +
                    (collected_.size() > n ? ", " + std::to_string(collected_.size()) + " total"
                                           : std::string()));
    };
    // Adds (or, in Remove mode / with Shift, removes) a batch of objects at "Select objects:".
    auto applyBatch = [&](const std::vector<EntityId>& ids, bool shift,
                          const std::string& none) -> SubmitResult {
        const bool remove = !req.single && (removeMode_ || shift);
        std::vector<EntityId> hit;
        if (remove) {
            hit.reserve(ids.size());
            for (EntityId id : ids)
                if (collectedSet_.count(id))
                    hit.push_back(id);
        } else {
            hit.reserve(ids.size());
            for (EntityId id : ids)
                if (accepts(id))
                    hit.push_back(id);
        }
        if (hit.empty())
            return reject(none);
        selUndo_.push_back(collected_);
        if (selUndo_.size() > 20)
            selUndo_.erase(selUndo_.begin());
        const std::vector<EntityId> all = expandGroups(doc_, hit);
        if (remove) {
            std::unordered_set<EntityId> drop;
            drop.reserve(all.size());
            for (EntityId id : all)
                if (collectedSet_.count(id))
                    drop.insert(id);
            std::erase_if(collected_, [&](EntityId id) { return drop.count(id) != 0; });
            for (EntityId id : drop)
                collectedSet_.erase(id);
            host_.print(std::to_string(drop.size()) + " removed, " +
                        std::to_string(collected_.size()) + " total");
            return SubmitResult::Accepted;
        }
        const std::size_t before = collected_.size();
        const bool plain = all.size() == hit.size();
        for (EntityId id : all)
            if (plain || accepts(id))
                addCollected(id);
        if (req.single) {
            ans.status = Status::Ok;
            ans.ids = collected_;
            return complete(std::move(ans));
        }
        reportFound(collected_.size() - before);
        return SubmitResult::Accepted;
    };
    // The last point of a typed Window / Crossing / polygon / fence arrived: select.
    auto finishShape = [&]() -> SubmitResult {
        const SelShape sh = shape_;
        std::vector<Vec2> pts = std::move(shapePts_);
        shape_ = SelShape::None;
        shapePts_.clear();
        IHost::SelectShape kind = IHost::SelectShape::Window;
        switch (sh) {
            case SelShape::Crossing:
                kind = IHost::SelectShape::Crossing;
                break;
            case SelShape::WPolygon:
                kind = IHost::SelectShape::WPolygon;
                break;
            case SelShape::CPolygon:
                kind = IHost::SelectShape::CPolygon;
                break;
            case SelShape::Fence:
                kind = IHost::SelectShape::Fence;
                break;
            default:
                break;
        }
        auto found = host_.selectShape(kind, pts);
        if (!found)
            return reject("This application cannot select by window, polygon or fence.");
        return applyBatch(*found, false, "No objects found.");
    };
    auto addShapePoint = [&](Vec2 p) -> SubmitResult {
        lastPoint_ = p;
        shapePts_.push_back(p);
        if ((shape_ == SelShape::Window || shape_ == SelShape::Crossing) && shapePts_.size() == 2)
            return finishShape();
        host_.print(shape_ == SelShape::Window || shape_ == SelShape::Crossing
                        ? "Specify opposite corner:"
                    : shape_ == SelShape::Fence ? "Specify next fence point or [Undo]:"
                                                : "Specify endpoint of polygon or [Undo]:");
        return SubmitResult::Accepted;
    };

    // A point for a point, distance or angle prompt; FROM, M2P and TT take theirs first.
    auto deliverPoint = [&](Vec2 p) -> SubmitResult {
        lastPoint_ = p;
        switch (ptMod_) {
            case PtMod::FromBase:
                ptModA_ = p;
                ptMod_ = PtMod::FromOffset;
                host_.print("<Offset>:");
                return SubmitResult::Accepted;
            case PtMod::Mid1:
                ptModA_ = p;
                ptMod_ = PtMod::Mid2;
                host_.print("Second point of mid:");
                return SubmitResult::Accepted;
            case PtMod::Mid2:
                p = (ptModA_ + p) * 0.5;
                lastPoint_ = p;
                break;
            case PtMod::Track:
                ptMod_ = PtMod::None;
                if (parser_)
                    parser_->trackPoint(p);
                host_.print(req.text + ":");
                return SubmitResult::Accepted;
            default:
                break;
        }
        ptMod_ = PtMod::None;
        ans.status = Status::Ok;
        if (req.kind == K::Point) {
            ans.point = p;
            if (req.base)
                lastSeg_ = std::make_pair(*req.base, p);
        } else if (!req.base) {
            return reject("Type a value; there is no base point to measure from.");
        } else if (req.kind == K::Distance) {
            ans.real = distance(*req.base, p);
        } else {
            ans.real = normalizeAngle(std::atan2(p.y - req.base->y, p.x - req.base->x));
        }
        return complete(std::move(ans));
    };
    const bool pointPrompt =
        req.kind == K::Point || req.kind == K::Distance || req.kind == K::Angle;

    switch (in.kind) {
        case Input::Kind::Cancel:
            ptMod_ = PtMod::None;
            ans.status = Status::Cancel;
            return complete(std::move(ans));

        case Input::Kind::Enter: {
            switch (req.kind) {
                case K::Selection:
                    if (shape_ != SelShape::None) {
                        const std::size_t need =
                            shape_ == SelShape::Fence                                        ? 2
                            : (shape_ == SelShape::WPolygon || shape_ == SelShape::CPolygon) ? 3
                                                                                             : 99;
                        if (shapePts_.size() >= need) {
                            return finishShape();
                        }
                        shape_ = SelShape::None;
                        shapePts_.clear();
                        host_.print("Select objects:");
                        return SubmitResult::Accepted;
                    }
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
                        ans.real = *req.defaultReal + ucsTurn(req);
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
                const std::string up = upper(text);
                // A prefix of `full` that is at least `minLen` letters long ("W" = Window).
                auto opt = [&](std::string_view full, std::size_t minLen) {
                    return up.size() >= minLen && up.size() <= full.size() &&
                           full.substr(0, up.size()) == up;
                };
                if (shape_ != SelShape::None) {
                    if (opt("UNDO", 1)) {
                        if (!shapePts_.empty())
                            shapePts_.pop_back();
                        else
                            shape_ = SelShape::None;
                        return SubmitResult::Accepted;
                    }
                    const PointParseContext pc = parseContext(req);
                    std::optional<Vec2> p;
                    if (parser_)
                        p = parser_->parsePoint(text, pc);
                    if (!p)
                        p = parsePointText(text, pc);
                    if (!p)
                        return reject("Invalid point \"" + text + "\".");
                    return addShapePoint(*p);
                }
                auto startShape = [&](SelShape sh, const char* prompt) {
                    shape_ = sh;
                    shapePts_.clear();
                    host_.print(prompt);
                    return SubmitResult::Accepted;
                };
                if (opt("WINDOW", 1))
                    return startShape(SelShape::Window, "Specify first corner:");
                if (opt("CROSSING", 1))
                    return startShape(SelShape::Crossing, "Specify first corner:");
                if (opt("WPOLYGON", 2))
                    return startShape(SelShape::WPolygon, "Specify first polygon point:");
                if (opt("CPOLYGON", 2))
                    return startShape(SelShape::CPolygon, "Specify first polygon point:");
                if (opt("FENCE", 1))
                    return startShape(SelShape::Fence, "Specify first fence point:");
                if (opt("REMOVE", 1)) {
                    removeMode_ = true;
                    host_.print("Remove objects:");
                    return SubmitResult::Accepted;
                }
                if (opt("ADD", 1)) {
                    removeMode_ = false;
                    host_.print("Select objects:");
                    return SubmitResult::Accepted;
                }
                if (opt("UNDO", 1)) {
                    if (selUndo_.empty()) {
                        host_.print("Nothing to undo.");
                        return SubmitResult::Accepted;
                    }
                    collected_ = std::move(selUndo_.back());
                    selUndo_.pop_back();
                    collectedSet_.clear();
                    collectedSet_.insert(collected_.begin(), collected_.end());
                    host_.print(std::to_string(collected_.size()) + " total");
                    return SubmitResult::Accepted;
                }
                std::vector<EntityId> found;
                if (opt("ALL", 2)) {
                    doc_.forEachEntity([&](const Entity& e) {
                        if (pickable(e) && accepts(e.common.id))
                            found.push_back(e.common.id);
                    });
                } else if (opt("LAST", 1)) {
                    const Entity* last = nullptr;
                    doc_.forEachEntity([&](const Entity& e) {
                        if (pickable(e) && accepts(e.common.id) &&
                            (!last || e.common.id > last->common.id))
                            last = &e;
                    });
                    if (last)
                        found.push_back(last->common.id);
                } else if (opt("PREVIOUS", 1)) {
                    for (EntityId id : previousSelection_)
                        if (accepts(id))
                            found.push_back(id);
                } else {
                    return reject("Invalid option \"" + text +
                                  "\". Pick objects, or type Window, Crossing, WPolygon, CPolygon, "
                                  "Fence, All, Last, Previous, Add, Remove or Undo.");
                }
                return applyBatch(found, false, "No objects found.");
            }
            if (pointPrompt) {
                const std::string up = upper(text);
                if (up == "FROM" || up == "FRO") {
                    ptMod_ = PtMod::FromBase;
                    host_.print("Base point:");
                    return SubmitResult::Accepted;
                }
                if (up == "M2P" || up == "MTP") {
                    ptMod_ = PtMod::Mid1;
                    host_.print("First point of mid:");
                    return SubmitResult::Accepted;
                }
                if (up == "TT") {
                    ptMod_ = PtMod::Track;
                    host_.print("Specify temporary OTRACK point:");
                    return SubmitResult::Accepted;
                }
                if (parser_)
                    if (auto mode = parser_->snapOverride(text)) {
                        host_.setSetting(kOsnapOnceSetting, *mode);
                        host_.print(*mode == "none" ? "Object snaps off for the next point."
                                                    : "Snap to " + *mode + " for the next point.");
                        return SubmitResult::Accepted;
                    }
                if (ptMod_ != PtMod::None) {
                    const PointParseContext pc = parseContext(req);
                    std::optional<Vec2> p;
                    if (parser_)
                        p = parser_->parsePoint(text, pc);
                    if (!p)
                        p = parsePointText(text, pc);
                    if (!p)
                        return reject("Invalid point \"" + text + "\".");
                    return deliverPoint(*p);
                }
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
                    if (req.base)
                        lastSeg_ = std::make_pair(*req.base, *p);
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
                        a = ang.value() + ucsTurn(req);
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
            if (req.kind == K::Selection && shape_ != SelShape::None) {
                const Vec2 p = filtered(in.raw, true,
                                        shapePts_.empty() ? std::optional<Vec2>{}
                                                          : std::optional<Vec2>{shapePts_.back()});
                return addShapePoint(p);
            }
            if (req.kind != K::Point && req.kind != K::Distance && req.kind != K::Angle)
                return SubmitResult::Ignored;
            // While FROM or M2P collects its points the rubber band starts at the last of them.
            const std::optional<Vec2> base = ptMod_ == PtMod::FromOffset || ptMod_ == PtMod::Mid2
                                                 ? std::optional<Vec2>(ptModA_)
                                             : ptMod_ == PtMod::None ? req.base
                                                                     : std::nullopt;
            return deliverPoint(filtered(in.raw, true, base, req.snap));
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
                if (shape_ != SelShape::None)
                    return SubmitResult::Ignored;
                const bool remove = in.raw.shift || removeMode_;
                if (!remove && !accepts(in.entity))
                    return reject("That object cannot be selected here.");
                return applyBatch({in.entity}, in.raw.shift, "That object is not selected.");
            }
            return SubmitResult::Ignored;
        }

        case Input::Kind::Selection: {
            if (req.kind != K::Selection || shape_ != SelShape::None)
                return SubmitResult::Ignored;
            return applyBatch(in.ids, in.raw.shift,
                              removeMode_ || in.raw.shift ? "No selected objects found."
                                                          : "No valid objects selected.");
        }
    }
    return SubmitResult::Ignored;
}

}  // namespace tbcad::cmd
