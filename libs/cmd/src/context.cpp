#include <stdexcept>

#include "tbcad/cmd/command.h"
#include "tbcad/cmd/registry.h"

namespace tbcad::cmd {

CommandContext::CommandContext(Document& doc, IHost& host, UndoStack& undo, Registry& registry,
                               std::string commandName, CommandContext* parent)
    : doc_(doc),
      host_(host),
      undo_(undo),
      registry_(registry),
      name_(commandName),
      label_(std::move(commandName)),
      parent_(parent) {}

CommandContext::~CommandContext() = default;

Transaction& CommandContext::tx() {
    if (parent_)
        return parent_->tx();
    if (!tx_) {
        tx_.emplace(doc_.begin(label_));
        tx_->setDefaultOwner(host_.drawingOwner());
    }
    return *tx_;
}

namespace {

detail::PromptRequest make(detail::PromptKind kind, std::string text,
                           std::vector<std::string> keywords) {
    detail::PromptRequest r;
    r.kind = kind;
    r.text = std::move(text);
    r.keywords = std::move(keywords);
    return r;
}

}  // namespace

CommandContext::Awaiter<PointResult> CommandContext::getPoint(std::string prompt,
                                                              PointOptions opts) {
    auto r = make(detail::PromptKind::Point, std::move(prompt), std::move(opts.keywords));
    r.base = opts.base;
    r.preview = std::move(opts.preview);
    r.allowNone = opts.allowNone;
    r.freeText = opts.freeText;
    return {*this, std::move(r)};
}

CommandContext::Awaiter<RealResult> CommandContext::getDistance(std::string prompt,
                                                                DistanceOptions opts) {
    auto r = make(detail::PromptKind::Distance, std::move(prompt), std::move(opts.keywords));
    r.base = opts.base;
    r.preview = std::move(opts.preview);
    r.defaultReal = opts.defaultValue;
    r.allowZero = opts.allowZero;
    r.allowNegative = opts.allowNegative;
    return {*this, std::move(r)};
}

CommandContext::Awaiter<RealResult> CommandContext::getAngle(std::string prompt,
                                                             AngleOptions opts) {
    auto r = make(detail::PromptKind::Angle, std::move(prompt), std::move(opts.keywords));
    r.base = opts.base;
    r.preview = std::move(opts.preview);
    r.defaultReal = opts.defaultValue;
    return {*this, std::move(r)};
}

CommandContext::Awaiter<RealResult> CommandContext::getReal(std::string prompt, RealOptions opts) {
    auto r = make(detail::PromptKind::Real, std::move(prompt), std::move(opts.keywords));
    r.defaultReal = opts.defaultValue;
    r.min = opts.min;
    r.max = opts.max;
    return {*this, std::move(r)};
}

CommandContext::Awaiter<IntResult> CommandContext::getInteger(std::string prompt, IntOptions opts) {
    auto r = make(detail::PromptKind::Integer, std::move(prompt), std::move(opts.keywords));
    r.defaultInt = opts.defaultValue;
    if (opts.min)
        r.min = static_cast<double>(*opts.min);
    if (opts.max)
        r.max = static_cast<double>(*opts.max);
    r.textPreview = std::move(opts.textPreview);
    return {*this, std::move(r)};
}

CommandContext::Awaiter<StringResult> CommandContext::getString(std::string prompt,
                                                                StringOptions opts) {
    auto r = make(detail::PromptKind::String, std::move(prompt), {});
    r.defaultText = std::move(opts.defaultValue);
    r.allowEmptyString = opts.allowEmpty;
    r.textPreview = std::move(opts.textPreview);
    return {*this, std::move(r)};
}

CommandContext::Awaiter<KeywordResult> CommandContext::getKeyword(std::string prompt,
                                                                  KeywordOptions opts) {
    auto r = make(detail::PromptKind::Keyword, std::move(prompt), std::move(opts.keywords));
    r.defaultText = std::move(opts.defaultKeyword);
    r.keywordAliases = std::move(opts.aliases);
    return {*this, std::move(r)};
}

CommandContext::Awaiter<SelectionResult> CommandContext::getSelection(std::string prompt,
                                                                      SelectionOptions opts) {
    auto r = make(detail::PromptKind::Selection, std::move(prompt), std::move(opts.keywords));
    r.filter = std::move(opts.filter);
    r.allowPreselection = opts.allowPreselection;
    r.single = opts.single;
    return {*this, std::move(r)};
}

CommandContext::Awaiter<EntityResult> CommandContext::getEntity(std::string prompt,
                                                                EntityOptions opts) {
    auto r = make(detail::PromptKind::Entity, std::move(prompt), std::move(opts.keywords));
    r.filter = std::move(opts.filter);
    return {*this, std::move(r)};
}

bool CommandContext::tryImmediate(detail::PromptRequest& req) {
    if (cancelled_) {
        answer_ = {};
        answer_.status = Status::Cancel;
        return true;
    }
    if (req.kind == detail::PromptKind::Selection && req.allowPreselection) {
        std::vector<EntityId> ids;
        for (EntityId id : host_.selection()) {
            const Entity* e = doc_.find(id);
            if (e && (!req.filter || req.filter(*e)))
                ids.push_back(id);
        }
        if (!ids.empty()) {
            std::vector<EntityId> grouped;
            for (EntityId id : expandGroups(doc_, ids)) {
                const Entity* e = doc_.find(id);
                if (e && (!req.filter || req.filter(*e)))
                    grouped.push_back(id);
            }
            ids = std::move(grouped);
            answer_ = {};
            answer_.status = Status::Ok;
            answer_.ids = std::move(ids);
            return true;
        }
    }
    return false;
}

void CommandContext::suspendOn(detail::PromptRequest req, std::coroutine_handle<> h) {
    pending_ = std::move(req);
    waiting_ = h;
}

std::coroutine_handle<> CommandContext::deliver(detail::PromptAnswer answer) {
    answer_ = std::move(answer);
    pending_.reset();
    auto h = waiting_;
    waiting_ = {};
    return h;
}

void CommandContext::commitOpen() {
    if (!tx_)
        return;
    registry_.runCommitHooks(doc_, *tx_);
    ChangeSet cs = tx_->commit();
    tx_.reset();
    if (!cs.empty())
        segments_.push_back(std::move(cs));
}

void CommandContext::undoSegmentsTo(std::size_t count) {
    while (segments_.size() > count) {
        doc_.apply(segments_.back(), Direction::Undo);
        segments_.pop_back();
    }
}

void CommandContext::checkpoint() {
    if (parent_)
        throw std::logic_error("checkpoint() is not available in a transparent command");
    commitOpen();
    marks_.push_back(segments_.size());
}

void CommandContext::publishPending() {
    if (!parent_ && !marks_.empty())
        commitOpen();
}

bool CommandContext::rollbackToCheckpoint() {
    if (parent_)
        throw std::logic_error("rollbackToCheckpoint() is not available in a transparent command");
    if (marks_.empty())
        return false;
    if (tx_) {
        tx_->rollback();
        tx_.reset();
    }
    const std::size_t keep = marks_.back();
    marks_.pop_back();
    undoSegmentsTo(keep);
    return true;
}

bool CommandContext::finish(bool failed) {
    if (parent_)
        return false;  // everything went into the parent's transaction
    const bool rollback = failed || (cancelled_ && !commitOnCancel_);
    if (rollback) {
        if (tx_) {
            tx_->rollback();
            tx_.reset();
        }
        undoSegmentsTo(0);
        marks_.clear();
        return false;
    }
    commitOpen();
    marks_.clear();
    if (segments_.empty())
        return false;
    undo_.push(label_, std::move(segments_));
    segments_.clear();
    return true;
}

}  // namespace tbcad::cmd
