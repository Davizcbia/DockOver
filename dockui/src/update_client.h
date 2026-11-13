#pragma once
// docky software update (Settings > About, and the Manager's automatic check).
// Modelled on Vitra Launcher's updater: a GitHub release whose tag is the version number, carrying a portable zip
// (docky-Windows-Portable-<version>.zip) with a manifest.txt of every program file. The zip is downloaded and its
// SHA-256 checked against the digest GitHub publishes for the asset; docky-updater.exe, run from a temporary copy,
// then closes docky, replaces exactly the manifest's files (keeping the old ones until the copy succeeded), and
// starts docky again. User settings live in %LOCALAPPDATA% and are never touched.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace docky::update {

struct Release {
    std::string version;    // tag without a leading "v"
    std::string assetName, assetUrl, sha256;  // sha256: lowercase hex, empty if GitHub gave no digest
    std::string pageUrl;    // the release's web page
    uint64_t size = 0;
};

enum class CheckStatus { UpToDate, Available, NoRelease, Failed };
struct CheckResult {
    CheckStatus status = CheckStatus::Failed;
    Release release;
    std::wstring error;
};

const char* CurrentVersion();
std::wstring Repository();  // "owner/name"
// <0, 0, >0 like strcmp. Numeric dot components; a pre-release suffix ("1.2.0-beta") sorts before the release.
int CompareVersions(const std::string& a, const std::string& b);

CheckResult Check();
bool Download(const Release& r, const std::wstring& zipPath, const std::function<void(uint64_t done, uint64_t total)>& progress,
              const std::atomic<bool>* cancel, std::wstring& error);
// Starts a temporary copy of docky-updater.exe (from this program's folder) on the zip. It waits for this process to
// exit (waitForCaller; Settings closes itself), closes docky if it runs, installs into this program's folder and starts
// docky again if it was running. reopenSettings: open Settings > About afterwards.
bool StartInstall(const std::wstring& zipPath, const std::string& version, bool reopenSettings, bool waitForCaller, std::wstring& error);

std::wstring WorkDir();  // %LOCALAPPDATA%\LiquidDesktop\updates (created)
std::wstring ProgramDir();

// Remembered between runs (WorkDir\state.json).
struct State {
    int64_t lastCheck = 0;   // Unix seconds of the last successful check
    std::string latest;      // newest version seen at that check ("" = none)
    std::string notified;    // version the user was last told about
};
State LoadState();
void SaveState(const State& s);
int64_t UnixNow();

}  // namespace docky::update
