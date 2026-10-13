#include "tbcad/mcp/headless.h"

#include "tbcad/geom/geom.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/platform/paths.h"

namespace tbcad::mcp {

HeadlessSession::HeadlessSession(cmd::Registry& registry) : registry_(registry) {
    install(std::make_unique<Document>(), {});
    hasFile_ = false;
}

HeadlessSession::~HeadlessSession() {
    // Order matters: the host refers to the document, the indexer observes it.
    host_.reset();
    indexer_.reset();
    doc_.reset();
}

void HeadlessSession::install(std::unique_ptr<Document> doc, std::filesystem::path file) {
    host_.reset();
    indexer_.reset();
    doc_ = std::move(doc);
    Document* d = doc_.get();
    indexer_ = std::make_unique<index::DocumentIndexer>(
        *doc_, index_, [d](const Entity& e) { return geom::bounds(e, *d); });
    host_ = std::make_unique<cmd::ScriptHost>(*doc_, registry_);
    file_ = std::move(file);
    hasFile_ = !file_.empty();
    savedRevision_ = doc_->revision();
}

void HeadlessSession::runOnOwnerThread(std::function<void()> fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    fn();
}

Result<void> HeadlessSession::load(const std::filesystem::path& path) {
    io::IoReport report;
    auto doc = io::readTbcad(path, report);
    if (!doc)
        return doc.error();
    install(std::move(doc.value()), path);
    return {};
}

FileInfo HeadlessSession::file() {
    FileInfo f;
    f.hasFile = hasFile_;
    f.path = file_;
    f.name = hasFile_ ? platform::pathToUtf8(file_.filename()) : "Untitled";
    f.modified = doc_->revision() != savedRevision_;
    return f;
}

Result<void> HeadlessSession::save(std::optional<std::filesystem::path> as) {
    std::filesystem::path target = as ? *as : file_;
    if (target.empty())
        return makeError(ErrorCode::InvalidArgument, "no file name");
    auto r = io::writeTbcad(*doc_, target);
    if (!r)
        return r;
    file_ = target;
    hasFile_ = true;
    savedRevision_ = doc_->revision();
    return {};
}

void HeadlessSession::newDocument() {
    install(std::make_unique<Document>(), {});
}

std::vector<std::string> HeadlessSession::messagesSince(std::size_t mark) {
    const auto& log = host_->log();
    if (mark >= log.size())
        return {};
    return std::vector<std::string>(log.begin() + static_cast<std::ptrdiff_t>(mark), log.end());
}

EditDecision HeadlessSession::requestEdit(const ClientInfo& client) {
    if (policy_)
        return policy_(client);
    return readOnly_ ? EditDecision::ReadOnly : EditDecision::Allow;
}

}  // namespace tbcad::mcp
