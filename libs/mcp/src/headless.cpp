#include "tbcad/mcp/headless.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>

#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/geom/geom.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/platform/paths.h"

namespace tbcad::mcp {

namespace {

bool isDxf(const std::filesystem::path& p) {
    std::string e = platform::pathToUtf8(p.extension());
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".dxf";
}

}  // namespace

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
    if (isDxf(path)) {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return makeError(ErrorCode::Io, "cannot open " + platform::pathToUtf8(path));
        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        std::memcpy(bytes.data(), raw.data(), raw.size());
        dxf::Report dxfReport;
        auto imported = dxf::importDxf(bytes, {}, dxfReport);
        if (!imported)
            return imported.error();
        install(std::move(imported.value().document), path);
        return {};
    }
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
    if (isDxf(target)) {
        dxf::ExportOptions opt;
        const auto& custom = doc_->header().custom;
        if (auto it = custom.find("dxf.acadver"); it != custom.end() && it->second.size() == 6 &&
                                                  it->second.compare(0, 2, "AC") == 0 &&
                                                  std::atoi(it->second.c_str() + 2) >= 1021)
            opt.version = dxf::DxfVersion::R2007;
        dxf::Report dxfReport;
        auto out = dxf::exportDxf(*doc_, opt, dxfReport);
        if (!out)
            return out.error();
        const auto& bytes = out.value().bytes;
        auto saved = io::saveTbcadBytes(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                          bytes.size()),
            target, io::WriteOptions{});
        if (!saved)
            return saved.error();
    } else if (auto r = io::writeTbcad(*doc_, target); !r) {
        return r;
    }
    file_ = target;
    hasFile_ = true;
    savedRevision_ = doc_->revision();
    return {};
}

void HeadlessSession::newDocument() {
    std::unique_ptr<Document> doc = newDocumentFactory_ ? newDocumentFactory_() : nullptr;
    install(doc ? std::move(doc) : std::make_unique<Document>(), {});
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
