#include <doctest/doctest.h>

#include <fstream>

#include "tbcad/platform/file_lock.h"

using namespace tbcad::platform;
namespace fs = std::filesystem;

TEST_CASE("a second lock on the same drawing is refused until the first is released") {
    const fs::path dir = fs::temp_directory_path() / "tbcad_file_lock_test";
    fs::create_directories(dir);
    const fs::path drawing = dir / "plan.tbcad";
    {
        std::ofstream(drawing) << "x";
    }
    {
        FileLock first(drawing);
        CHECK(first.held());
        CHECK_FALSE(first.heldElsewhere());
        FileLock second(drawing);
        CHECK_FALSE(second.held());
        CHECK(second.heldElsewhere());
        FileLock other(dir / "other.tbcad");
        CHECK(other.held());
    }
    FileLock again(drawing);
    CHECK(again.held());
    fs::remove_all(dir);
}

TEST_CASE("a lock in a folder that cannot be written is simply not held") {
    FileLock l(fs::path("/nonexistent_dir_for_tbcad/plan.tbcad"));
    CHECK_FALSE(l.held());
    CHECK_FALSE(l.heldElsewhere());
}
