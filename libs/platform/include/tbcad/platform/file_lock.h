#pragma once

#include <filesystem>
#include <memory>

namespace tbcad::platform {

// Marks a drawing file as open in this process. The lock is held on a small side file next to the
// drawing (".<name>.lock") and the operating system drops it when the process ends, however it
// ends, so a crash never leaves a lock behind. A second process (or a second lock in the same
// process) that asks for the same drawing is told that it is taken.
class FileLock {
public:
    // Tries to take the lock for `drawing`. `held()` is false when another holder has it or when
    // the lock file cannot be created (a read-only folder): then nothing is locked and
    // `heldElsewhere()` tells the two cases apart.
    explicit FileLock(const std::filesystem::path& drawing);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    bool held() const { return held_; }
    bool heldElsewhere() const { return elsewhere_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool held_ = false;
    bool elsewhere_ = false;
};

}  // namespace tbcad::platform
