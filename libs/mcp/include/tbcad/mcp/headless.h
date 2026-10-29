#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "tbcad/cmd/script_host.h"
#include "tbcad/index/document_indexer.h"
#include "tbcad/mcp/session.h"

namespace tbcad::mcp {

// A drawing session without a window: a Document, its undo stack and a command runner fed by
// scripted input. Used by `tbcad-mcp --headless` and by tests.
class HeadlessSession final : public DrawingSession {
public:
    // `registry` must already hold the commands and outlive the session.
    explicit HeadlessSession(cmd::Registry& registry);
    ~HeadlessSession() override;

    // Edit permission: Allow by default; readOnly(true) makes every edit refused. Tests install
    // their own policy with setPolicy.
    void setReadOnly(bool readOnly) { readOnly_ = readOnly; }
    void setPolicy(std::function<EditDecision(const ClientInfo&)> policy) {
        policy_ = std::move(policy);
    }
    std::function<void(const ToolCallLog&)> onLog{};

    // What new_drawing starts from (default: an empty document). The tbcad-mcp program passes the
    // built-in template, so new drawings have the same linetypes and layers as File > New.
    void setNewDocumentFactory(std::function<std::unique_ptr<Document>()> factory) {
        newDocumentFactory_ = std::move(factory);
    }

    // Loads a .tbcad file as the current drawing (also what open_drawing does).
    Result<void> load(const std::filesystem::path& path);

    // DrawingSession
    void runOnOwnerThread(std::function<void()> fn) override;
    Document& document() override { return *doc_; }
    cmd::UndoStack& undoStack() override { return host_->undoStack(); }
    cmd::Registry& registry() override { return registry_; }
    cmd::CommandRunner& runner() override { return host_->runner(); }
    const index::SpatialIndex* spatialIndex() override { return &index_; }
    std::vector<EntityId> selection() override { return host_->selection(); }
    void setSelection(std::vector<EntityId> ids) override { host_->setSelection(std::move(ids)); }
    FileInfo file() override;
    Result<void> save(std::optional<std::filesystem::path> as) override;
    Result<void> open(const std::filesystem::path& path) override { return load(path); }
    void newDocument() override;
    std::size_t messageCount() override { return host_->log().size(); }
    std::vector<std::string> messagesSince(std::size_t mark) override;
    EditDecision requestEdit(const ClientInfo& client) override;
    void logToolCall(const ToolCallLog& log) override {
        if (onLog)
            onLog(log);
    }

private:
    void install(std::unique_ptr<Document> doc, std::filesystem::path file);

    cmd::Registry& registry_;
    std::function<std::unique_ptr<Document>()> newDocumentFactory_{};
    std::recursive_mutex mutex_{};
    std::unique_ptr<Document> doc_{};
    index::SpatialIndex index_{};
    std::unique_ptr<index::DocumentIndexer> indexer_{};
    std::unique_ptr<cmd::ScriptHost> host_{};
    std::filesystem::path file_{};
    bool hasFile_ = false;
    std::uint64_t savedRevision_ = 0;
    bool readOnly_ = false;
    std::function<EditDecision(const ClientInfo&)> policy_{};
};

}  // namespace tbcad::mcp
