#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tbcad::app {

// A window's client area in screen points.
struct WindowRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

struct OtherWindow {
    long pid = 0;
    WindowRect rect{};
};

// How several TBCAD windows (one process each) know about each other, so a drawing tab dragged
// out of one window opens in a new window and a tab dropped on another window moves there.
// Everything goes through small files in one folder: <pid>.win holds the window's rectangle (and
// its age says the window is alive), <pid>.open.* are drawings another window sent to <pid>.
class WindowLink {
public:
    explicit WindowLink(std::filesystem::path dir);
    WindowLink(std::filesystem::path dir, long pid);  // tests
    ~WindowLink();
    WindowLink(const WindowLink&) = delete;
    WindowLink& operator=(const WindowLink&) = delete;

    long pid() const { return pid_; }
    static long currentProcessId();

    // Tells the other windows where this one is. Writes at most every 0.4 s unless `force`.
    void publish(const WindowRect& rect, bool force = false);
    // The other windows that are alive, newest rectangles.
    std::vector<OtherWindow> others();
    // Hands a drawing file to another window; it opens it the next time it looks.
    bool send(long pid, const std::filesystem::path& file);
    // Drawings other windows sent here (UTF-8 paths); each is returned once.
    std::vector<std::string> takeIncoming();

    // Starts another TBCAD process that opens `file` in a window placed at (x, y), `w` x `h`
    // points big. False when the program cannot be started.
    static bool spawn(const std::string& file, int x, int y, int w, int h);

private:
    std::filesystem::path winFile(long pid) const;
    std::filesystem::path dir_;
    long pid_ = 0;
    double lastPublish_ = -1e9;
    unsigned counter_ = 0;
};

}  // namespace tbcad::app
