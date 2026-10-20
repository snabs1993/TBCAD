// Groups of a DXF file collected into records (everything from one "0" group to the next).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "tbcad/dxf/lexer.h"
#include "tbcad/dxf/report.h"

namespace tbcad::dxf::detail {

struct Record {
    std::string_view type{};  // value of the leading "0" group (raw bytes)
    int line = 0;             // line (or byte offset) of the leading group
    std::vector<Group> g{};   // the groups after the leading "0" group

    void clear() {
        type = {};
        line = 0;
        g.clear();
    }
    // First group with this code, or nullptr.
    const Group* find(int code) const {
        for (const Group& x : g) {
            if (x.code == code)
                return &x;
        }
        return nullptr;
    }
    bool has(int code) const { return find(code) != nullptr; }
    double num(int code, double fallback = 0) const {
        const Group* x = find(code);
        return x ? x->number() : fallback;
    }
    std::int64_t integer(int code, std::int64_t fallback = 0) const {
        const Group* x = find(code);
        return x ? x->integer() : fallback;
    }
    std::string_view raw(int code) const {
        const Group* x = find(code);
        return x ? x->str() : std::string_view{};
    }
};

// Reads the next record (from a "0" group up to the next one). `pending` holds the "0" group that
// ended the last record. False at the end of the input.
inline bool readRecord(Lexer& lexer, std::optional<Group>& pending, Record& out) {
    out.clear();
    if (!pending) {
        // Skip stray groups before the first "0".
        while (true) {
            std::optional<Group> g = lexer.next();
            if (!g)
                return false;
            if (g->code == 0) {
                pending = std::move(g);
                break;
            }
        }
    }
    out.type = pending->str();
    out.line = pending->line;
    pending.reset();
    while (true) {
        std::optional<Group> g = lexer.next();
        if (!g)
            break;
        if (g->code == 0) {
            pending = std::move(g);
            break;
        }
        out.g.push_back(std::move(*g));
    }
    return true;
}

// A second thread that runs the lexer ahead of the reader: it fills batches of records, each with
// the diagnostics the lexer made while reading it, and the reader adds those to the real report
// as it hands out the record. Same records, same diagnostics, same order as reading directly.
class RecordPipe {
public:
    RecordPipe(Lexer& lexer, Report& real) : real_(real), lexer_(lexer) {
        lexer.setReport(lexReport_);
        worker_ = std::thread([this] { produce(); });
    }
    ~RecordPipe() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        canPush_.notify_all();
        worker_.join();
        lexer_.setReport(real_);
    }

    bool next(Record& out) {
        if (cur_ >= batch_.n) {
            if (finished_)
                return false;
            std::unique_lock<std::mutex> lock(m_);
            canPop_.wait(lock, [&] { return !queue_.empty(); });
            if (!batch_.recs.empty())
                pool_.push_back(std::move(batch_));  // its records' storage is used again
            batch_ = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            canPush_.notify_one();
            cur_ = 0;
            ev_ = 0;
            if (batch_.last)
                finished_ = true;
            if (batch_.n == 0) {
                replay(0);
                return false;
            }
        }
        replay(cur_);
        // The caller's old record goes into the slot: its vector keeps its storage for the worker.
        std::swap(out, batch_.recs[cur_++]);
        if (cur_ == batch_.n && finished_)
            replay(cur_);  // what the lexer said looking for a record that is not there
        return true;
    }

private:
    struct Event {
        std::uint32_t rec;
        Severity severity;
        bool dropped;  // the worker only counted it: count it here too
        Diagnostic d;
    };
    struct Batch {
        std::vector<Record> recs;  // kBatch slots, the first n filled
        std::size_t n = 0;
        std::vector<Event> events;
        bool last = false;
    };
    static constexpr std::size_t kBatch = 256;
    static constexpr std::size_t kQueued = 6;

    void replay(std::size_t rec) {
        while (ev_ < batch_.events.size() && batch_.events[ev_].rec == rec) {
            Event& e = batch_.events[ev_++];
            if (e.dropped)
                real_.add(e.severity, "", "");
            else
                real_.add(e.d.severity, e.d.code, std::move(e.d.message), e.d.line, e.d.handle);
        }
    }

    void produce() {
        std::optional<Group> pending;
        std::size_t seen[3] = {lexReport_.count(Severity::Info),
                               lexReport_.count(Severity::Warning),
                               lexReport_.count(Severity::Error)};
        for (;;) {
            Batch b = takeBatch();
            for (;;) {
                if (readRecord(lexer_, pending, b.recs[b.n])) {
                    ++b.n;
                    collect(b, static_cast<std::uint32_t>(b.n - 1), seen);
                } else {
                    b.last = true;
                    collect(b, static_cast<std::uint32_t>(b.n), seen);
                    break;
                }
                if (b.n >= kBatch)
                    break;
            }
            std::unique_lock<std::mutex> lock(m_);
            canPush_.wait(lock, [&] { return stop_ || queue_.size() < kQueued; });
            if (stop_)
                return;
            const bool last = b.last;
            queue_.push_back(std::move(b));
            lock.unlock();
            canPop_.notify_one();
            if (last)
                return;
        }
    }

    Batch takeBatch() {
        Batch b;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (!pool_.empty()) {
                b = std::move(pool_.back());
                pool_.pop_back();
            }
        }
        if (b.recs.empty())
            b.recs.resize(kBatch);
        b.n = 0;
        b.events.clear();
        b.last = false;
        return b;
    }

    // Moves what the lexer reported since the last call into the batch, tagged with `rec`.
    void collect(Batch& b, std::uint32_t rec, std::size_t (&seen)[3]) {
        std::vector<Diagnostic> items = lexReport_.takeItems();
        std::size_t kept[3] = {0, 0, 0};
        for (Diagnostic& d : items) {
            if (d.code == code::kTooManyDiagnostics)
                continue;  // the real report makes its own
            ++kept[static_cast<int>(d.severity)];
            const Severity sev = d.severity;
            b.events.push_back({rec, sev, false, std::move(d)});
        }
        const Severity sevs[3] = {Severity::Info, Severity::Warning, Severity::Error};
        for (int i = 0; i < 3; ++i) {
            const std::size_t now = lexReport_.count(sevs[i]);
            for (std::size_t k = seen[i] + kept[i]; k < now; ++k)
                b.events.push_back({rec, sevs[i], true, {}});
            seen[i] = now;
        }
    }

    Report& real_;
    Report lexReport_{};
    Lexer& lexer_;
    std::thread worker_{};
    std::mutex m_{};
    std::condition_variable canPush_{}, canPop_{};
    std::deque<Batch> queue_{};
    std::vector<Batch> pool_{};
    bool stop_ = false;
    // Used by the reading side only.
    Batch batch_{};
    std::size_t cur_ = 0, ev_ = 0;
    bool finished_ = false;
};

// Reads records from a lexer; supports pushing one record back.
class RecordReader {
public:
    explicit RecordReader(Lexer& lexer) : lexer_(lexer) {}
    RecordReader(const RecordReader&) = delete;
    RecordReader& operator=(const RecordReader&) = delete;

    // From now on a second thread runs the lexer ahead of the caller (see RecordPipe). Call
    // before the first next(); nobody else may use the lexer until this reader is gone.
    void readAhead(Report& report) {
        if (!pipe_ && !held_ && !pending_)
            pipe_ = std::make_unique<RecordPipe>(lexer_, report);
    }

    // False at the end of the input. The record's type is "EOF" for the final "0 EOF".
    bool next(Record& out) {
        if (held_) {
            out = std::move(*held_);
            held_.reset();
            return true;
        }
        if (pipe_)
            return pipe_->next(out);
        return readRecord(lexer_, pending_, out);
    }

    // The next call to next() returns `r` again.
    void pushBack(Record&& r) { held_ = std::move(r); }

private:
    Lexer& lexer_;
    std::optional<Group> pending_{};
    std::optional<Record> held_{};
    std::unique_ptr<RecordPipe> pipe_{};
};

}  // namespace tbcad::dxf::detail
