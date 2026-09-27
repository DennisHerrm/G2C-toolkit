// gui/update.h - checking for and installing new versions from GitHub.
//
// Releases are built by GitHub Actions (.github/workflows): every version tag
// becomes a release, every push to main replaces the pre-release "snapshot".
// This file asks the GitHub API for the newest one, downloads g2c.exe,
// verifies it against the published SHA256SUMS.txt and swaps it in.
//
// Deliberately without Win32: HTTP comes in through two functions, so the
// whole flow - including the file swap - runs in the tests against a fake
// server. Only main_win32.cpp knows WinHTTP.
//
// Nothing is installed without the user clicking "Update now". The startup
// check only reads one small JSON document and can be switched off.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace g2::gui::update {

inline constexpr const char* kRepo = "DennisHerrm/G2C-toolkit";

// --- Version of this build ------------------------------------------------

// Set by CMake (-DG2C_VERSION=..., -DG2C_COMMIT=...). A local build without
// them is "dev": it never checks on its own, so a developer's build is not
// replaced behind their back.
struct BuildInfo {
    std::string version;   // "1.2.0", "snapshot-1a2b3c4" or "dev"
    std::string commit;    // full commit hash, may be empty
};
BuildInfo thisBuild();

enum class BuildKind { Release, Snapshot, Dev };
BuildKind kindOf(const BuildInfo& b);

struct Version {
    int         major = 0, minor = 0, patch = 0;
    std::string pre;   // "beta.1" for 1.2.0-beta.1, empty for a final version
};

// "1.2.3", "v1.2.3", "1.2.3-rc.1". Anything else: nullopt.
std::optional<Version> parseVersion(std::string_view text);

// Semantic-versioning precedence: <0, 0, >0. 1.2.0-beta < 1.2.0.
int compare(const Version& a, const Version& b);

// --- What GitHub reports ----------------------------------------------------

struct Asset {
    std::string   name;
    std::string   url;
    std::uint64_t size = 0;
};

struct Release {
    std::string        tag;
    std::string        name;
    std::string        htmlUrl;
    std::string        commit;   // target_commitish: the SHA for snapshots
    std::string        notes;
    bool               prerelease = false;
    std::vector<Asset> assets;

    const Asset* find(std::string_view assetName) const;
};

// One release object of the GitHub REST API. nullopt if it isn't one.
std::optional<Release> parseRelease(std::string_view json);

enum class Channel { Stable, Snapshot };

// Default channel if the user never chose one: the one this build came from.
Channel defaultChannel(const BuildInfo& b);

std::string apiUrl(Channel ch);

// Identifies a release for "skip this version": tag for stable, commit for
// snapshots (the snapshot tag never changes).
std::string releaseId(const Release& r, Channel ch);

// Short name for the UI: "1.2.0" or "snapshot 1a2b3c4".
std::string displayName(const Release& r, Channel ch);
std::string displayName(const BuildInfo& b);

// Is r worth offering to this build?
bool isNewer(const BuildInfo& own, const Release& r, Channel ch);

// Only files from this repository's release downloads are accepted. The API
// answer is trusted for the version number, but not to send us elsewhere.
bool trustedDownloadUrl(std::string_view url);

// --- Integrity ----------------------------------------------------------------

std::string                sha256Hex(std::string_view data);
std::optional<std::string> sha256File(const std::filesystem::path& path);

// Line for `file` in a sha256sum listing ("<hex>  <name>" or "<hex> *<name>").
std::optional<std::string> checksumFor(std::string_view sums, std::string_view file);

// --- Swapping the executable ------------------------------------------------
//
// A running exe cannot be overwritten on Windows, but it can be renamed. So:
// target -> target.old, staged -> target. If the second step fails, the first
// is undone. The .old file is removed on the next start.

// Empty on success, otherwise the reason.
std::string replaceFile(const std::filesystem::path& target, const std::filesystem::path& staged);

std::filesystem::path oldPathOf(const std::filesystem::path& exe);

// Removes leftovers of a previous update. true if there was one, i.e. this is
// the first start after an update. Retries for a moment: the old process may
// still be exiting.
bool cleanupAfterUpdate(const std::filesystem::path& exe);

// --- The flow ---------------------------------------------------------------

struct HttpResult {
    int         status = 0;   // 0 = no connection
    std::string body;
    std::string error;        // network error text if status == 0
};

struct Network {
    // GET with a size limit. Needed.
    std::function<HttpResult(const std::string& url, std::size_t maxBytes)> get;

    // Streams into a file. progress(done, total) returns false to cancel.
    // Empty string on success, otherwise the reason. Needed for installing.
    std::function<std::string(const std::string& url, const std::filesystem::path& dest,
                              const std::function<bool(std::uint64_t, std::uint64_t)>& progress)>
        download;
};

enum class Phase { Idle, Checking, UpToDate, Available, Downloading, Installed, Failed };

enum class Error {
    None,
    Network,     // no connection, timeout, HTTP error; detail says which
    NoRelease,   // 404: nothing published yet on this channel
    BadAnswer,   // not a release object
    NoAsset,     // release without g2c.exe or SHA256SUMS.txt
    Untrusted,   // download URL outside this repository
    Checksum,    // size, header or SHA-256 wrong: file discarded
    Write,       // exe could not be replaced; detail says why
    Cancelled,
};

struct Status {
    Phase         phase = Phase::Idle;
    Error         error = Error::None;
    std::string   detail;
    bool          manual = false;   // started by the user: report "up to date" too
    Release       release;
    std::uint64_t done = 0, total = 0;
};

// Runs check and install on a worker thread. The UI polls status() each frame.
class Updater {
public:
    Updater(Network net, BuildInfo build, std::filesystem::path exePath);
    ~Updater();

    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // True if a previous update left its .old file behind, i.e. this is the
    // first start of the new version. Decided once, in the constructor.
    bool justUpdated() const { return justUpdated_; }

    // Once at program start: removes the leftovers of an update on the worker
    // thread (the old process may still be exiting), then checks if asked to.
    void startup(bool autoCheck, Channel ch);

    // Ignored while something is running.
    void check(Channel ch, bool manual);

    // Only in phase Available.
    void install();

    void cancel() { cancel_.store(true); }

    // Back to Idle, e.g. after "Later" or a shown error.
    void dismiss();

    Status status() const;
    bool   busy() const;

    const BuildInfo&             build() const { return build_; }
    const std::filesystem::path& exePath() const { return exe_; }

    // For the tests: wait until the worker is done.
    void wait();

private:
    // Joins the previous worker and starts fn on a new one.
    void launch(std::function<void()> fn);
    void runCheck(Channel ch);
    void runInstall(Release r);
    // Download, then check size, "MZ" header and SHA-256. On failure the
    // status already holds the error and the partial file is gone.
    bool fetchVerified(const Asset& a, const std::string& sums, const std::filesystem::path& dest,
                       std::uint64_t base);
    void fail(Error e, std::string detail = {});
    void set(Phase p);

    Network               net_;
    BuildInfo             build_;
    std::filesystem::path exe_;
    bool                  justUpdated_ = false;

    mutable std::mutex         mutex_;
    Status                     status_;
    std::atomic<bool>          cancel_{false};
    std::atomic<std::uint64_t> done_{0}, total_{0};
    std::thread                worker_;
};

}  // namespace g2::gui::update
