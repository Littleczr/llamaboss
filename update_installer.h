#pragma once
//
// update_installer.h — in-app "Download and Install" for LlamaBoss updates.
//
// Flow (driven by the About dialog + MyFrame):
//   1. DownloadAndVerify()  — worker thread.  Streams the installer from
//      the manifest's installer_url into %TEMP%\LlamaBoss_Update\ while
//      hashing it (SHA-256, Windows CNG).  The file is only renamed from
//      .part to .exe after the hash matches; on mismatch it is deleted.
//   2. LaunchInstaller()    — UI thread, after every window closed cleanly.
//      Runs the Inno Setup installer silently with the flags below; the
//      installer's [Run] entry relaunches LlamaBoss when /LBUPDATE=1.
//   3. CleanupStaleDownloads() — app startup.  Removes the previous
//      update's installer so it doesn't sit in %TEMP%.
//
// Files written by WinHTTP carry no Mark-of-the-Web, so SmartScreen does
// not prompt; the SHA-256 from the HTTPS manifest is the integrity check.
// The installer is per-user (PrivilegesRequired=lowest), so no UAC prompt.
//
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace UpdateInstaller {

struct DownloadResult {
    bool         ok        = false;
    bool         cancelled = false;
    std::wstring path;      // verified installer path when ok
    std::string  error;     // human-readable reason when !ok && !cancelled
};

// done/total in bytes; total is 0 when the server sent no Content-Length.
using ProgressFn = std::function<void(std::uint64_t done, std::uint64_t total)>;

// True iff s is exactly 64 hex characters.
bool IsValidSha256Hex(const std::string& s);

// BLOCKING — run on a worker thread.  'cancel' is polled between reads.
DownloadResult DownloadAndVerify(const std::string& url,
                                 const std::string& expectedSha256Hex,
                                 const std::string& version,
                                 const ProgressFn& progress,
                                 const std::atomic<bool>& cancel);

// Starts the installer detached.  Returns false with 'error' filled on
// failure.  The caller is expected to exit the app right after success.
bool LaunchInstaller(const std::wstring& installerPath, std::string& error);

// Best effort; silently ignores files still locked by a running installer.
void CleanupStaleDownloads();

} // namespace UpdateInstaller
