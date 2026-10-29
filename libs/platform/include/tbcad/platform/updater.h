#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tbcad::platform {

// A program version such as 0.1.0 or 0.1.0-test.7 (a leading "v" is accepted).
struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre;  // text after '-', empty for a final release
    bool valid = false;
};

Version parseVersion(std::string_view text);
// <0, 0, >0. A version with a pre-release suffix is older than the same number without one;
// suffixes compare by their dot-separated parts (numbers as numbers, as in SemVer).
int compareVersions(const Version& a, const Version& b);

// One release as the update check needs it.
struct ReleaseInfo {
    std::string tag;        // v0.1.0-test.8
    std::string version;    // 0.1.0-test.8
    std::string pageUrl;    // the release page, for "what's new"
    std::string notes;      // the release text
    std::string assetName;  // TBCAD-0.1.0-test.8-setup.exe
    std::string assetUrl;
    std::string sha256;  // lower-case hex, empty when GitHub gave none
    long long assetSize = 0;
    bool prerelease = false;
};

// Only installers from this release area may be downloaded and run.
inline constexpr std::string_view kReleaseUrlPrefix =
    "https://github.com/snabs1993/TBCAD/releases/download/";
inline constexpr std::string_view kReleaseFeedUrl =
    "https://api.github.com/repos/snabs1993/TBCAD/releases?per_page=15";

// Reads the answer of GitHub's "list releases". Drafts and releases without a setup installer
// (TBCAD-<version>-setup.exe under `allowedPrefix`) are left out.
std::vector<ReleaseInfo> parseReleases(std::string_view json,
                                       std::string_view allowedPrefix = kReleaseUrlPrefix);

// channel: "stable" only final releases; "test" also test builds; "auto" is "test" when the
// running version is itself a test build, else "stable".
// Returns the newest release that is newer than `current`, or none.
std::optional<ReleaseInfo> pickUpdate(const std::vector<ReleaseInfo>& releases,
                                      std::string_view current, std::string_view channel);

std::string sha256Hex(std::string_view bytes);
// Empty on a read error.
std::string sha256FileHex(const std::filesystem::path& file);

// Command-line of the installer for an in-place update: silent, over the old install, and the
// program starts again when it is done. `perMachine` is true when the running program lives in
// Program Files (the installer then asks Windows for administrator rights, as the first install
// did).
std::vector<std::string> installerArguments(bool perMachine);

// Platform parts. HTTPS GET into `body`; false with `error` set on failure. Not available on
// platforms other than Windows (always false).
bool httpsGet(const std::string& url, std::string& body, std::string& error);
// Downloads to `file`; `progress` (0..1) and `cancel` are optional.
bool httpsDownload(const std::string& url, const std::filesystem::path& file,
                   std::atomic<float>* progress, const std::atomic<bool>* cancel,
                   std::string& error);
// The running program's file; empty when unknown.
std::filesystem::path selfExecutable();
// Starts a program detached; true when it started.
bool launchDetached(const std::filesystem::path& exe, const std::vector<std::string>& args);

// The whole flow with a worker thread: check -> (user asks) download -> verify -> ready.
class UpdateService {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };

    UpdateService() = default;
    ~UpdateService();
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    // Starts a check in the background; ignored while one runs.
    void check(std::string currentVersion, std::string channel);
    // Starts downloading the update found by check().
    void download(const std::filesystem::path& folder);
    void cancel();

    State state() const { return state_.load(); }
    float progress() const { return progress_.load(); }
    // The following are valid in the states named; copies, so safe to hold.
    std::optional<ReleaseInfo> release() const;
    std::filesystem::path installer() const;
    std::string error() const;

private:
    void join();
    std::atomic<State> state_{State::Idle};
    std::atomic<float> progress_{0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mu_;
    std::optional<ReleaseInfo> release_;
    std::filesystem::path installer_;
    std::string error_;
    std::thread worker_;
};

}  // namespace tbcad::platform
