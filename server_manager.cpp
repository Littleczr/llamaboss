// server_manager.cpp
#include "server_manager.h"
#include "path_safety.h"

#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/stdpaths.h>
#include <wx/fileconf.h>
#include <wx/utils.h>

#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>

#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <set>
#include <cctype>
#include <initializer_list>

// ── Event definitions ────────────────────────────────────────────
wxDEFINE_EVENT(wxEVT_SERVER_READY, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_SERVER_ERROR, wxCommandEvent);

// ── Internal: read the last ~N bytes of a file, snapped to a line ────
// Used to attach llama-server log tail to error events so the user
// sees why the server died instead of a generic timeout.
static std::string ReadLogTail(const std::string& path, size_t maxBytes = 4096)
{
    try {
        std::ifstream f(path_safety::Utf8ToWide(path), std::ios::binary | std::ios::ate);
        if (!f) return "";
        std::streampos size = f.tellg();
        if (size <= 0) return "";

        size_t total   = static_cast<size_t>(size);
        size_t readLen = (total > maxBytes) ? maxBytes : total;
        f.seekg(-static_cast<std::streamoff>(readLen), std::ios::end);

        std::string buf(readLen, '\0');
        f.read(&buf[0], readLen);
        buf.resize(static_cast<size_t>(f.gcount()));

        // If we truncated from the middle of a line, advance to the
        // next newline so the tail starts cleanly.
        if (total > maxBytes) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) buf.erase(0, nl + 1);
        }
        return buf;
    } catch (...) {
        return "";
    }
}

// ═══════════════════════════════════════════════════════════════════
//  ServerHealthThread — polls /health until 200 or timeout

static std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool ContainsAny(const std::string& haystack,
                        std::initializer_list<const char*> needles)
{
    for (const char* needle : needles) {
        if (needle && haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Phase 3 stabilization: only the specific "retry without --jinja"
// recovery path should use MaybeRetryWithoutJinja(). Earlier builds
// retried on every server-start failure, which could hide unrelated
// problems such as a bad model path, CUDA/DLL failure, or port conflict.
static bool LooksLikeJinjaOrTemplateFailure(const std::string& error)
{
    const std::string e = ToLowerAscii(error);

    if (ContainsAny(e, {"--jinja", "jinja"})) {
        return true;
    }

    if (ContainsAny(e, {
            "chat template",
            "chat_template",
            "chat-template",
            "failed to parse template",
            "failed to compile template",
            "template parsing",
            "template compile",
            "template error",
            "unsupported chat template",
            "invalid chat template"})) {
        return true;
    }

    return false;
}
// ═══════════════════════════════════════════════════════════════════

ServerHealthThread::ServerHealthThread(wxEvtHandler* handler,
                                       const std::string& baseUrl,
                                       std::shared_ptr<std::atomic<bool>> cancelFlag,
                                       std::weak_ptr<std::atomic<bool>> aliveToken,
#ifdef __WXMSW__
                                       HANDLE processHandle,
#endif
                                       const std::string& logPath,
                                       int timeoutMs)
    : wxThread(wxTHREAD_DETACHED)
    , m_handler(handler)
    , m_baseUrl(baseUrl)
    , m_cancelFlag(cancelFlag)
    , m_aliveToken(aliveToken)
#ifdef __WXMSW__
    , m_processHandle(INVALID_HANDLE_VALUE)
#endif
    , m_logPath(logPath)
    , m_timeoutMs(timeoutMs)
{
#ifdef __WXMSW__
    // Duplicate the handle so we own our own copy. Required because
    // the ServerManager may close its handle (StopServer / dtor)
    // while this detached thread is still running — closing a handle
    // someone else is waiting on is undefined. With a duplicate,
    // both sides can close independently.
    if (processHandle != INVALID_HANDLE_VALUE) {
        DuplicateHandle(GetCurrentProcess(), processHandle,
                        GetCurrentProcess(), &m_processHandle,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
    }
#endif
}

bool ServerHealthThread::SafePost(wxCommandEvent* ev)
{
    if (m_cancelFlag->load()) {
        delete ev;
        return false;
    }
    auto alive = m_aliveToken.lock();
    if (!alive || !alive->load()) {
        delete ev;
        return false;
    }
    wxQueueEvent(m_handler, ev);
    return true;
}

wxThread::ExitCode ServerHealthThread::Entry()
{
    auto start = std::chrono::steady_clock::now();

    while (!m_cancelFlag->load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed > m_timeoutMs) {
            std::string msg = "Server did not become ready within "
                              + std::to_string(m_timeoutMs / 1000)
                              + " seconds.";
            std::string tail = ReadLogTail(m_logPath);
            if (!tail.empty()) msg += "\n\nLast log output:\n" + tail;
            else if (!m_logPath.empty())
                msg += "\n\nSee log: " + m_logPath;

            auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
            ev->SetString(wxString::FromUTF8(msg));
            SafePost(ev);
#ifdef __WXMSW__
            if (m_processHandle != INVALID_HANDLE_VALUE)
                CloseHandle(m_processHandle);
#endif
            return (ExitCode)0;
        }

#ifdef __WXMSW__
        // Did the child process exit already? Non-blocking wait (timeout=0).
        // If so, llama-server crashed before answering /health — no point
        // polling for another ~120 seconds. Grab the log tail and bail.
        if (m_processHandle != INVALID_HANDLE_VALUE &&
            WaitForSingleObject(m_processHandle, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(m_processHandle, &exitCode);
            std::string msg = "llama-server exited (code "
                              + std::to_string(exitCode)
                              + ") before becoming ready.";
            std::string tail = ReadLogTail(m_logPath);
            if (!tail.empty()) msg += "\n\nLast log output:\n" + tail;

            auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
            ev->SetString(wxString::FromUTF8(msg));
            SafePost(ev);
            CloseHandle(m_processHandle);
            return (ExitCode)0;
        }
#endif

        try {
            Poco::URI uri(m_baseUrl + "/health");
            Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
            // 500ms is plenty on localhost — if the server can't answer
            // /health in half a second, it isn't "slow," the socket's hung.
            // The outer retry loop sleeps 500ms between probes anyway, so
            // worst case we waste 1s per failed attempt.
            sess.setTimeout(Poco::Timespan(0, 500 * 1000));

            Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET,
                                       uri.getPathAndQuery());
            sess.sendRequest(req);

            Poco::Net::HTTPResponse resp;
            std::istream& in = sess.receiveResponse(resp);
            std::string body;
            Poco::StreamCopier::copyToString(in, body);

            if (resp.getStatus() == Poco::Net::HTTPResponse::HTTP_OK) {
                auto* ev = new wxCommandEvent(wxEVT_SERVER_READY);
                SafePost(ev);
#ifdef __WXMSW__
                if (m_processHandle != INVALID_HANDLE_VALUE)
                    CloseHandle(m_processHandle);
#endif
                return (ExitCode)0;
            }
            // 503 = still loading, keep polling
        }
        catch (...) {
            // Connection refused / not ready yet — keep polling
        }

        // Sleep 500ms in small increments (check cancel flag frequently)
        for (int i = 0; i < 10 && !m_cancelFlag->load(); ++i)
            wxMilliSleep(50);
    }

#ifdef __WXMSW__
    if (m_processHandle != INVALID_HANDLE_VALUE)
        CloseHandle(m_processHandle);
#endif
    return (ExitCode)0;
}

// ═══════════════════════════════════════════════════════════════════
//  ServerManager
// ═══════════════════════════════════════════════════════════════════

ServerManager::ServerManager(wxEvtHandler* eventHandler,
                             std::weak_ptr<std::atomic<bool>> aliveToken,
                             Poco::Logger* logger)
    : m_eventHandler(eventHandler)
    , m_aliveToken(aliveToken)
    , m_logger(logger)
{
}

ServerManager::~ServerManager()
{
    StopServer();
}

std::string ServerManager::GetBaseUrl() const
{
    return "http://127.0.0.1:" + std::to_string(m_port);
}

std::string ServerManager::ModelDisplayName(const std::string& ggufPath)
{
    // Prefer the bundle folder name when the model is bundled — this
    // surfaces clean, user-chosen names ("gemma-3-27b-it-abliterated")
    // in the UI instead of noisy quantization-tagged filenames
    // ("gemma-3-27b-it-abliterated-q4_k_m"). For loose files (power
    // mode, or dropped into the default folder without a bundle),
    // fall back to the .gguf filename stem — matches legacy behavior.
    wxFileName fn(ggufPath);
    wxString modelFolder = fn.GetPath();
    wxString modelsRoot  = wxString::FromUTF8(GetModelsDir());

    bool isBundled = !modelFolder.IsEmpty() &&
                     (modelFolder != modelsRoot) &&
                     (wxFileName(modelFolder).GetPath() == modelsRoot);

    if (isBundled) {
        // Return the last path component (the bundle folder name).
        wxFileName folderFn = wxFileName::DirName(modelFolder);
        const wxArrayString& dirs = folderFn.GetDirs();
        if (!dirs.IsEmpty())
            return std::string(dirs.Last().ToUTF8().data());
    }

    // Loose file — "gemma-2-9b-it-Q4_K_M.gguf" → "gemma-2-9b-it-Q4_K_M"
    return fn.GetName().ToUTF8().data();
}

// ── Backend detection ────────────────────────────────────────────

Backend ServerManager::DetectBackend()
{
#ifdef __WXMSW__
    // Check if NVIDIA CUDA runtime is available
    HMODULE hCuda = LoadLibraryA("nvcuda.dll");
    if (hCuda) {
        FreeLibrary(hCuda);
        return Backend::CUDA12;
    }
#endif
    return Backend::CPU;
}

// ── Find llama-server binary ─────────────────────────────────────

std::string ServerManager::FindServerBinary(Backend backend)
{
    wxString exePath = wxStandardPaths::Get().GetExecutablePath();
    wxFileName exeFn(exePath);
    wxString exeDir = exeFn.GetPath();

    wxString subdir = (backend == Backend::CUDA12) ? "cuda12" : "cpu";
    wxString serverExe = "llama-server.exe";

    // Helper: check candidate directories for the server binary
    auto tryDir = [&](const wxString& dir) -> std::string {
        // Try with backend subfolder: bin\cuda12\llama-server.exe
        wxString p1 = dir + wxFILE_SEP_PATH + "bin" + wxFILE_SEP_PATH
                     + subdir + wxFILE_SEP_PATH + serverExe;
        if (wxFileExists(p1)) return p1.ToUTF8().data();

        // Try flat: bin\llama-server.exe (simple dev setup)
        wxString p2 = dir + wxFILE_SEP_PATH + "bin" + wxFILE_SEP_PATH + serverExe;
        if (wxFileExists(p2)) return p2.ToUTF8().data();

        return "";
    };

    // Walk up from the exe directory, checking each level.
    // Covers both installed layout (bin\ next to exe) and
    // VS dev layouts (exe in x64\Debug\, x64\Release\, etc.)
    wxFileName walkDir = wxFileName::DirName(exeDir);
    for (int level = 0; level < 4; ++level) {
        std::string found = tryDir(walkDir.GetPath());
        if (!found.empty()) return found;

        // Move one directory up
        walkDir.RemoveLastDir();
    }

    return ""; // Not found
}

// ── Data directories ─────────────────────────────────────────────

std::string ServerManager::GetDataDir()
{
    // wxStandardPaths uses the app name set via wxApp::SetAppName().
    // Falls back to %LOCALAPPDATA%\LlamaBoss.
    wxString dir = wxStandardPaths::Get().GetUserLocalDataDir();
    return dir.ToUTF8().data();
}

std::string ServerManager::GetDefaultModelsDir()
{
    // The hardcoded "home" folder for LlamaBoss-managed models. Never
    // changes across runs or installs, so the UI can always display it
    // and "Reset to default" always points here. Casual users never
    // move away from this path.
    return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "models";
}

std::string ServerManager::GetModelsDirOverride()
{
    // Read the user's custom path from wxFileConfig. An empty value
    // (or missing key) means "no override — use the default".
    wxFileConfig cfg("LlamaBoss");
    wxString path;
    if (cfg.Read("ModelsFolderOverride", &path) && !path.IsEmpty())
        return std::string(path.ToUTF8().data());
    return "";
}

void ServerManager::SetModelsDirOverride(const std::string& path)
{
    wxFileConfig cfg("LlamaBoss");
    cfg.Write("ModelsFolderOverride", wxString::FromUTF8(path));
    cfg.Flush();
}

bool ServerManager::IsCasualMode()
{
    // Casual = no override set, scanning LlamaBoss's own folder.
    // Power  = override set, user is in control.
    return GetModelsDirOverride().empty();
}

std::string ServerManager::GetModelsDir()
{
    // The active scan root: override if set, default otherwise.
    // This is what the scanner, downloader, and UI all consume.
    std::string ov = GetModelsDirOverride();
    return ov.empty() ? GetDefaultModelsDir() : ov;
}
std::string ServerManager::GetLogsDir()           { return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "logs"; }
std::string ServerManager::GetConfigDir()         { return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "config"; }
std::string ServerManager::GetConversationsDir()  { return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "conversations"; }
std::string ServerManager::GetCacheDir()          { return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "cache"; }

void ServerManager::EnsureDataDirs()
{
    wxFileName::Mkdir(GetDataDir(),          wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetModelsDir(),        wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetLogsDir(),          wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetConfigDir(),        wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetConversationsDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetCacheDir(),         wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    // Workspace lives outside %LOCALAPPDATA% (under Documents) so the
    // user can find it from File Explorer. Created here at startup
    // so the agent always has a writeable home, even on a clean
    // first run before any /cd has been issued.
    EnsureWorkspaceDir();
}

// ── Workspace ──────────────────────────────────────────────────────
// Mirrors the models-folder helpers above: a hardcoded default path,
// an optional user override stored in wxFileConfig, and a single
// "active" accessor that returns the override when set and the
// default otherwise.
//
// The default user-visible workspace intentionally lives directly
// under the user's profile folder instead of Documents. On many
// Windows machines, Documents is silently redirected to OneDrive;
// using %USERPROFILE%\LlamaBoss keeps LlamaBoss's local-first output
// from being cloud-synced by surprise while still being easy to find.

static std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    const char sep = wxFILE_SEP_PATH;
    if (a.back() == sep || a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, sep) + b;
}

static std::string GetDefaultWorkspaceRootDir()
{
#ifdef __WXMSW__
    wxString userProfile;
    if (wxGetEnv("USERPROFILE", &userProfile) && !userProfile.IsEmpty()) {
        return JoinPath(std::string(userProfile.ToUTF8().data()), "LlamaBoss");
    }
#endif

    wxString home = wxGetHomeDir();
    if (!home.IsEmpty()) {
        return JoinPath(std::string(home.ToUTF8().data()), "LlamaBoss");
    }

    // Last-resort fallback only. This should rarely be used, but keeps
    // the app functional if the profile directory cannot be resolved.
    wxString docs = wxStandardPaths::Get().GetDocumentsDir();
    return JoinPath(std::string(docs.ToUTF8().data()), "LlamaBoss");
}

std::string ServerManager::GetDefaultWorkspaceDir()
{
    // Active default working directory for tools. Future document lanes
    // are created beside it by EnsureWorkspaceDir().
    return JoinPath(GetDefaultWorkspaceRootDir(), "Workspace");
}

std::string ServerManager::GetWorkspaceDirOverride()
{
    // Read the user's custom workspace path from wxFileConfig. Empty
    // (or missing key) means "use the default".
    wxFileConfig cfg("LlamaBoss");
    wxString path;
    if (cfg.Read("WorkspaceFolderOverride", &path) && !path.IsEmpty())
        return std::string(path.ToUTF8().data());
    return "";
}

void ServerManager::SetWorkspaceDirOverride(const std::string& path)
{
    wxFileConfig cfg("LlamaBoss");
    cfg.Write("WorkspaceFolderOverride", wxString::FromUTF8(path));
    cfg.Flush();
}

std::string ServerManager::GetWorkspaceDir()
{
    // Active workspace: override if set, default otherwise. This is
    // what ResolveCurrentCwd() in MyFrame consults as the fallback
    // when no /cd override is set on the current conversation.
    std::string ov = GetWorkspaceDirOverride();
    return ov.empty() ? GetDefaultWorkspaceDir() : ov;
}

void ServerManager::EnsureWorkspaceDir()
{
    // Idempotent: wxPATH_MKDIR_FULL is "mkdir -p" semantics, so this
    // is a cheap no-op when the directory already exists.
    wxFileName::Mkdir(GetWorkspaceDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    // For the built-in default only, create the full LlamaBoss folder
    // layout. If the user later picks a custom workspace folder, respect
    // that exact override and do not create sibling folders around it.
    if (GetWorkspaceDirOverride().empty()) {
        const std::string root = GetDefaultWorkspaceRootDir();
        wxFileName::Mkdir(root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

        const char* lanes[] = {
            "Workspace",
            "Documents",
            "Spreadsheets",
            "PDFs",
            "Scripts",
            "Downloads"
        };

        for (const char* lane : lanes) {
            wxFileName::Mkdir(JoinPath(root, lane), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Model scanning
// ═══════════════════════════════════════════════════════════════════
//
// Two layouts are supported, selected by whether the user has set a
// custom models folder:
//
//   CASUAL MODE (override not set — scanning the default folder):
//     models/
//     ├── gemma-3-27b-it-abliterated/         ← bundle subfolder
//     │   ├── gemma-3-27b-it-abliterated-q4_k_m.gguf
//     │   └── mmproj-f16.gguf                 ← paired automatically
//     └── gemma-4-e4b-it/                     ← text-only bundle
//         └── gemma-4-e4b-it-f16.gguf
//
//     Each subfolder is a "bundle" — one model + optional mmproj.
//     Pairing is deterministic (same folder = paired); no filename
//     scoring, no ambiguity.
//
//   POWER MODE (override set — user picked their own folder):
//     my-models/
//     ├── gemma-3-27b-it-abliterated-q4_k_m.gguf  ← loose files
//     ├── mmproj-gemma-3.gguf
//     └── qwen2.5-7b-instruct-q4_k_m.gguf
//
//     Flat layout; mmproj pairing falls back to filename heuristics.
//     User opted in to approximate matching by choosing this layout.
//
// The two modes share this function's signature so callers don't care.

// ── Internal: is this filename an mmproj projector? ──────────────
static bool IsMmprojFilename(const wxString& filename)
{
    return filename.Lower().Contains("mmproj");
}

// ── Internal: scan a single bundle subfolder ─────────────────────
// Returns a ModelEntry if the folder contains exactly one non-mmproj
// .gguf. Returns an empty entry (ggufPath empty) otherwise — skip it.
// Bundles with multiple .gguf weights are ambiguous and unsupported.
static ServerManager::ModelEntry ScanBundle(const wxString& bundleDir,
                                            const wxString& bundleName)
{
    ServerManager::ModelEntry entry;

    wxDir dir(bundleDir);
    if (!dir.IsOpened()) return entry;

    std::vector<wxString> weights;   // non-mmproj .gguf files
    std::vector<wxString> projs;     // mmproj .gguf files

    wxString filename;
    bool found = dir.GetFirst(&filename, "*.gguf", wxDIR_FILES);
    while (found) {
        if (IsMmprojFilename(filename))
            projs.push_back(filename);
        else
            weights.push_back(filename);
        found = dir.GetNext(&filename);
    }

    // Valid bundle: exactly one weight file. Zero = empty folder, skip.
    // More than one = ambiguous; we'd have to guess and we don't.
    if (weights.size() != 1) return entry;

    entry.ggufPath    = (bundleDir + wxFILE_SEP_PATH + weights[0]).ToUTF8().data();
    entry.displayName = bundleName.ToUTF8().data();
    entry.bundleDir   = bundleDir.ToUTF8().data();
    entry.isBundle    = true;

    // Pair the first mmproj found. Bundles should only have one by
    // convention; if someone dropped multiple in, first-wins is
    // deterministic and the user can resolve by removing extras.
    if (!projs.empty()) {
        entry.mmprojPath = (bundleDir + wxFILE_SEP_PATH + projs[0]).ToUTF8().data();
    }

    return entry;
}

std::vector<ServerManager::ModelEntry> ServerManager::ScanModels()
{
    std::vector<ModelEntry> entries;
    wxString root = GetModelsDir();
    if (!wxDir::Exists(root)) return entries;

    wxDir dir(root);
    if (!dir.IsOpened()) return entries;

    const bool casualMode = IsCasualMode();

    if (casualMode) {
        // ── Casual: enumerate subfolders as bundles ──────────────
        // Loose .gguf files at the root ARE also picked up — this
        // handles the case where a user drops a file into the default
        // folder directly instead of downloading through the app. It
        // keeps things working, but such files won't have mmproj
        // pairing; users get the benefits when they organize.
        wxString subfolder;
        bool foundDir = dir.GetFirst(&subfolder, wxEmptyString, wxDIR_DIRS);
        while (foundDir) {
            wxString bundlePath = root + wxFILE_SEP_PATH + subfolder;
            ModelEntry e = ScanBundle(bundlePath, subfolder);
            if (!e.ggufPath.empty())
                entries.push_back(std::move(e));
            foundDir = dir.GetNext(&subfolder);
        }

        // Fall-through: pick up loose .gguf files at the root.
        // These show up as unbundled entries — usable but without
        // automatic mmproj pairing.
        wxString filename;
        bool foundFile = dir.GetFirst(&filename, "*.gguf", wxDIR_FILES);
        while (foundFile) {
            if (!IsMmprojFilename(filename)) {
                wxString fullPath = root + wxFILE_SEP_PATH + filename;
                ModelEntry e;
                e.ggufPath = fullPath.ToUTF8().data();
                // Use filename stem as display name (matches legacy UX
                // so users who dropped files in don't see a regression).
                wxFileName fn(fullPath);
                e.displayName = fn.GetName().ToUTF8().data();
                e.isBundle = false;
                entries.push_back(std::move(e));
            }
            foundFile = dir.GetNext(&filename);
        }
    }
    else {
        // ── Power: flat scan (original behavior) ─────────────────
        wxString filename;
        bool foundFile = dir.GetFirst(&filename, "*.gguf", wxDIR_FILES);
        while (foundFile) {
            if (!IsMmprojFilename(filename)) {
                wxString fullPath = root + wxFILE_SEP_PATH + filename;
                ModelEntry e;
                e.ggufPath = fullPath.ToUTF8().data();
                wxFileName fn(fullPath);
                e.displayName = fn.GetName().ToUTF8().data();
                e.isBundle = false;
                entries.push_back(std::move(e));
            }
            foundFile = dir.GetNext(&filename);
        }
        // Power-mode mmproj pairing is resolved at server-start time
        // via FindMatchingMmproj (filename scoring). We don't populate
        // mmprojPath here because the pairing depends on which model
        // the user actually selects.
    }

    std::sort(entries.begin(), entries.end(),
        [](const ModelEntry& a, const ModelEntry& b) {
            return a.displayName < b.displayName;
        });

    return entries;
}

std::vector<std::string> ServerManager::ScanModelPaths()
{
    // Legacy path-only view — thin wrapper over ScanModels for callers
    // that just want .gguf paths. New code should use ScanModels().
    std::vector<std::string> paths;
    auto entries = ScanModels();
    paths.reserve(entries.size());
    for (const auto& e : entries)
        paths.push_back(e.ggufPath);
    return paths;
}

// ── Token-based mmproj matcher ──────────────────────────────────
//
// Tokenises a GGUF filename (split on - _ .), lowercases everything,
// strips quant/format noise tokens (q4, k_m, f16, bf16, gguf, mmproj),
// then scores each *mmproj*.gguf candidate by shared-token count.
// Returns the full path of the best match, or empty if nothing fits.

// Noise tokens that appear in filenames but carry no model-identity info.
// Split on '- _ .' so only the actual tokens produced by the tokenizer
// need to appear here (e.g. "k_m" never appears because it splits into
// "k" and "m").
static const std::set<std::string> kNoiseTokens = {
    "gguf", "mmproj",
    // Quant tier
    "q2", "q3", "q4", "q5", "q6", "q8",
    // K-quant suffix letters (from Q3_K_M, Q4_K_S, Q5_K_L etc.)
    "k", "m", "s", "l",
    // Imatrix quant tiers & suffixes
    "iq1", "iq2", "iq3", "iq4",
    "xxs", "xs", "nl",
    // Unsloth dynamic quants
    "ud", "xl",
    // Precision tags
    "f16", "f32", "bf16", "fp16", "fp32",
    // Finetune-type suffixes (shared mmproj across finetunes of same arch)
    "it", "instruct", "chat", "base",
};

static std::set<std::string> TokeniseFilename(const wxString& filename)
{
    // Work on the stem (no extension)
    wxFileName fn(filename);
    std::string stem = std::string(fn.GetName().Lower().ToUTF8().data());

    std::set<std::string> tokens;
    std::string tok;
    for (char c : stem) {
        if (c == '-' || c == '_' || c == '.') {
            if (!tok.empty()) {
                if (kNoiseTokens.find(tok) == kNoiseTokens.end())
                    tokens.insert(tok);
                tok.clear();
            }
        } else {
            tok += c;
        }
    }
    if (!tok.empty() && kNoiseTokens.find(tok) == kNoiseTokens.end())
        tokens.insert(tok);

    return tokens;
}

std::string ServerManager::FindMatchingMmproj(const std::string& modelGgufPath,
                                              Poco::Logger* logger)
{
    wxString modelsDir = GetModelsDir();
    if (!wxDir::Exists(modelsDir))
        return "";

    wxDir dir(modelsDir);
    if (!dir.IsOpened())
        return "";

    // ── Collect all *mmproj*.gguf candidates ──
    struct Candidate { wxString fullPath; int score; };
    std::vector<Candidate> candidates;

    wxString filename;
    bool found = dir.GetFirst(&filename, "*mmproj*.gguf", wxDIR_FILES);
    while (found) {
        wxString fullPath = modelsDir + wxFILE_SEP_PATH + filename;
        candidates.push_back({ fullPath, 0 });
        found = dir.GetNext(&filename);
    }

    if (candidates.empty())
        return "";

    // ── Tokenise the model name and score each candidate ──
    wxFileName modelFn(modelGgufPath);
    std::set<std::string> modelTokens = TokeniseFilename(modelFn.GetFullName());

    int bestScore = 0;
    int bestIdx   = -1;

    for (size_t i = 0; i < candidates.size(); ++i) {
        wxFileName candFn(candidates[i].fullPath);
        std::set<std::string> candTokens = TokeniseFilename(candFn.GetFullName());

        int score = 0;
        for (const auto& t : candTokens) {
            if (modelTokens.count(t))
                ++score;
        }
        candidates[i].score = score;

        if (score > bestScore) {
            bestScore = score;
            bestIdx   = static_cast<int>(i);
        }
        else if (score == bestScore && score > 0 && bestIdx >= 0) {
            // Tie — break deterministically by filename (case-insensitive,
            // lexicographic). Without this, the "winner" depends on whatever
            // order the OS returned entries, which is non-portable and can
            // flip between runs on the same machine.
            wxString a = candidates[i].fullPath.Lower();
            wxString b = candidates[bestIdx].fullPath.Lower();
            if (a < b) bestIdx = static_cast<int>(i);
        }
    }

    // Detect ambiguous ties among the top candidates so the user can see
    // *why* a particular projector was picked (or why none was).
    int tiedWithBest = 0;
    if (bestIdx >= 0) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (static_cast<int>(i) != bestIdx &&
                candidates[i].score == bestScore)
                ++tiedWithBest;
        }
    }

    // ── Single candidate → use it unconditionally ──────────────────
    // Common case: user has one mmproj file named generically
    // (e.g. "mmproj-F16.gguf") with no model-identity tokens.
    // No ambiguity to resolve, so just use it.
    if (candidates.size() == 1) {
        std::string result = candidates[0].fullPath.ToUTF8().data();
        if (logger) {
            if (bestScore > 0)
                logger->information(
                    "mmproj: matched \"" + result +
                    "\" (score " + std::to_string(bestScore) + "/" +
                    std::to_string(modelTokens.size()) + ")");
            else
                logger->information(
                    "mmproj: using sole projector \"" + result +
                    "\" (no token overlap — single candidate, no ambiguity)");
        }
        return result;
    }

    // ── Multiple candidates but no token overlap → ambiguous, skip ──
    if (bestScore == 0) {
        if (logger) {
            logger->information(
                "mmproj: " + std::to_string(candidates.size()) +
                " projector file(s) found but none matched model tokens — skipping");
        }
        return "";
    }

    std::string result = candidates[bestIdx].fullPath.ToUTF8().data();
    if (logger) {
        std::string tieNote;
        if (tiedWithBest > 0) {
            tieNote = " [AMBIGUOUS: " + std::to_string(tiedWithBest + 1) +
                      " files tied at score " + std::to_string(bestScore) +
                      ", picked alphabetically — consider renaming for clarity]";
        }
        logger->information(
            "mmproj: matched \"" + result +
            "\" (score " + std::to_string(bestScore) + "/" +
            std::to_string(modelTokens.size()) + ")" + tieNote);

        for (size_t i = 0; i < candidates.size(); ++i) {
            if (static_cast<int>(i) == bestIdx) continue;
            logger->information(
                "mmproj:   skipped \"" +
                std::string(candidates[i].fullPath.ToUTF8().data()) +
                "\" (score " + std::to_string(candidates[i].score) + ")");
        }
    }

    return result;
}

// ── Start server ─────────────────────────────────────────────────

bool ServerManager::StartServer(const std::string& ggufPath, const ServerConfig& config)
{
#ifdef __WXMSW__
    // Stop any existing server first
    StopServer();

    // Cache the launch args so MaybeRetryWithoutJinja() can re-invoke
    // us with the same (model, config) minus the --jinja flag.  When
    // m_jinjaForceOff is already true we're being called BY that
    // retry path; preserve the flag.  Otherwise this is a fresh load
    // attempt — clear any leftover retry state from a previous model
    // so the new model also tries jinja first.
    m_lastGgufPath = ggufPath;
    m_lastConfig   = config;
    if (!m_jinjaForceOff) {
        m_jinjaRetryAttempted = false;
    }

    // Capture the launch mode before any later retry-state cleanup.
    // This is the truth for the server process we are about to start.
    const bool launchJinjaEnabled = !m_jinjaForceOff;

    m_port = config.port;

    // Detect backend and find binary
    Backend backend = DetectBackend();
    std::string serverBin = FindServerBinary(backend);

    if (serverBin.empty()) {
        // Try the other backend as fallback
        Backend fallback = (backend == Backend::CUDA12) ? Backend::CPU : Backend::CUDA12;
        serverBin = FindServerBinary(fallback);
        if (!serverBin.empty()) {
            if (m_logger)
                m_logger->warning("Preferred backend not found, falling back to " +
                    std::string(fallback == Backend::CUDA12 ? "CUDA12" : "CPU"));
            backend = fallback;
        }
    }

    if (serverBin.empty()) {
        if (m_logger)
            m_logger->error("llama-server binary not found in any search path");

        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        ev->SetString("llama-server.exe not found.\n\n"
                      "Download llama.cpp release binaries from:\n"
                      "https://github.com/ggml-org/llama.cpp/releases\n\n"
                      "Place them in a 'bin\\cpu\\' or 'bin\\cuda12\\' folder "
                      "next to LlamaBoss.exe.\n\n"
                      "Models go in:\n" + GetModelsDir());
        wxQueueEvent(m_eventHandler, ev);
        return false;
    }

    // Verify the GGUF file exists
    if (!wxFileExists(ggufPath)) {
        if (m_logger)
            m_logger->error("GGUF model not found: " + ggufPath);

        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        ev->SetString("Model file not found:\n" + ggufPath);
        wxQueueEvent(m_eventHandler, ev);
        return false;
    }

    if (m_logger) {
        m_logger->information("Starting llama-server: backend=" +
            std::string(backend == Backend::CUDA12 ? "CUDA12" : "CPU") +
            " model=" + ggufPath +
            " port=" + std::to_string(config.port) +
            " jinja=" + std::string(launchJinjaEnabled ? "on" : "off"));
    }

    // Build command line
    std::ostringstream cmd;
    cmd << "\"" << serverBin << "\""
        << " -m \"" << ggufPath << "\""
        << " --port " << config.port
        << " -c " << config.ctxSize;

    // ── --jinja (Phase 3a) ───────────────────────────────────────
    // Required by llama-server for native /v1/chat/completions tool
    // calling on tool-aware chat templates (Hermes 2 Pro, Qwen 2.5,
    // Llama 3.x, etc.).  Cost on non-tool models is one Jinja render
    // per chat completion — negligible on a modern GPU.  If the
    // server fails to start with this flag (rare; some custom chat
    // templates don't compile under Jinja), MaybeRetryWithoutJinja
    // re-launches without it and we fall back to the existing XML
    // tool-call protocol for that model.
    if (launchJinjaEnabled) {
        cmd << " --jinja";
    }

    if (config.gpuLayers != 0)
        cmd << " -ngl " << config.gpuLayers;

    if (config.flashAttn)
        cmd << " --flash-attn on";

    if (config.threads > 0)
        cmd << " -t " << config.threads;

    // ── Auto-detect multimodal projector (mmproj) ────────────────
    // Bundle mode (casual): the model's own folder either contains
    // an mmproj or it doesn't. Zero ambiguity — no scoring needed.
    // This is the casual user's guarantee: pairing is determined by
    // filesystem structure, not filename heuristics.
    //
    // Flat mode (power): fall back to filename-token scoring against
    // *mmproj*.gguf files in the models folder. User opted in to
    // approximate matching by choosing a custom folder.
    {
        std::string mmproj;

        wxFileName modelFn(wxString::FromUTF8(ggufPath));
        wxString modelFolder = modelFn.GetPath();
        wxString modelsRoot  = wxString::FromUTF8(GetModelsDir());

        // A model path is "bundled" when its containing folder is a
        // direct subfolder of the scan root (not the root itself and
        // not nested deeper). This catches both real bundles and
        // honors the user's intent if they manually organized their
        // custom folder into subfolders.
        bool isBundled = (modelFolder != modelsRoot) &&
                         (wxFileName(modelFolder).GetPath() == modelsRoot);

        if (isBundled) {
            // Look for *mmproj*.gguf in the same folder as the model.
            // If exactly one exists, pair it. If none, model is
            // text-only (or the user hasn't downloaded the projector).
            wxDir bundleDir(modelFolder);
            if (bundleDir.IsOpened()) {
                wxString filename;
                bool found = bundleDir.GetFirst(&filename, "*mmproj*.gguf",
                                                wxDIR_FILES);
                if (found) {
                    mmproj = (modelFolder + wxFILE_SEP_PATH + filename)
                                .ToUTF8().data();
                    if (m_logger)
                        m_logger->information(
                            "mmproj: bundled pair \"" + mmproj + "\"");
                }
                else if (m_logger) {
                    m_logger->information(
                        "mmproj: no projector in bundle \"" +
                        std::string(modelFolder.ToUTF8().data()) +
                        "\" — launching text-only");
                }
            }
        }
        else {
            // Flat layout — use filename scoring (power-mode fallback).
            mmproj = FindMatchingMmproj(ggufPath, m_logger);
        }

        if (!mmproj.empty())
            cmd << " --mmproj \"" << mmproj << "\"";

        // Cache for Phase 3b's tool-protocol detection: the cache key
        // hashes (model, mmproj) so a newly-paired projector triggers
        // a fresh probe even if the model itself is unchanged.
        m_loadedMmproj = mmproj;
    }

    std::string cmdLine = cmd.str();

    if (m_logger)
        m_logger->information("Command: " + cmdLine);

    // Set up log file for server stdout/stderr
    EnsureDataDirs();
    std::string logPath = GetLogsDir() + std::string(1, wxFILE_SEP_PATH) + "server.log";

    // ── UTF-8 → UTF-16 conversion ────────────────────────────────
    // Every path and command-line string we built above came out of
    // wxString::ToUTF8(), so the bytes are UTF-8.  The ANSI Win32
    // APIs (CreateFileA / CreateProcessA) interpret their char* args
    // using the system code page — NOT UTF-8 — so a UTF-8 byte
    // sequence like 0xC3 0xBC for "ü" gets read as "Ã¼" under
    // CP1252 and the filesystem call silently fails to resolve.
    // Using the W APIs with explicit UTF-8 → UTF-16 conversion
    // avoids the whole class of "works on my ASCII machine, breaks
    // on a non-ASCII username / model filename / install path" bug.
    // Mirrors the pattern already used in cmd_executor.cpp.
    auto Utf8ToWide = [](const std::string& in) -> std::wstring {
        if (in.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                                    nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                            out.data(), n);
        return out;
    };

    std::wstring wLogPath = Utf8ToWide(logPath);
    std::wstring wCmdLine = Utf8ToWide(cmdLine);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hLogFile = CreateFileW(
        wLogPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,    // Allow reading while server runs
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    // Set up process creation
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    if (hLogFile != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hLogFile;
        si.hStdError  = hLogFile;
        si.hStdInput  = NULL;
    }

    PROCESS_INFORMATION pi = {};

    // CreateProcessW needs a MUTABLE wide command line buffer.
    // Windows filenames cannot contain '"', so the simple double-
    // quote wrapping around serverBin / ggufPath / mmproj in the
    // command-line build above is safe — no embedded-quote escape
    // pass needed.
    std::vector<wchar_t> cmdBuf(wCmdLine.begin(), wCmdLine.end());
    cmdBuf.push_back(L'\0');

    // Working directory = folder containing the server binary
    // (so DLLs like ggml-cuda.dll are found)
    wxFileName binFn(serverBin);
    std::string workDir = binFn.GetPath().ToUTF8().data();
    std::wstring wWorkDir = Utf8ToWide(workDir);

    BOOL ok = CreateProcessW(
        NULL,               // lpApplicationName (embedded in cmdLine)
        cmdBuf.data(),      // lpCommandLine
        NULL, NULL,         // process/thread security
        TRUE,               // bInheritHandles (for log file)
        CREATE_NO_WINDOW,   // dwCreationFlags
        NULL,               // lpEnvironment
        wWorkDir.c_str(),   // lpCurrentDirectory
        &si,
        &pi
    );

    // Close the log file handle in this process — the child has its own copy
    if (hLogFile != INVALID_HANDLE_VALUE)
        CloseHandle(hLogFile);

    if (!ok) {
        DWORD err = GetLastError();
        if (m_logger)
            m_logger->error("CreateProcess failed, error=" + std::to_string(err));

        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        ev->SetString("Failed to start llama-server (Windows error " +
                      std::to_string(err) + ")");
        wxQueueEvent(m_eventHandler, ev);
        return false;
    }

    m_processHandle       = pi.hProcess;
    m_threadHandle        = pi.hThread;
    m_processId           = pi.dwProcessId;
    m_loadedModel         = ggufPath;
    m_currentJinjaEnabled = launchJinjaEnabled;

    if (m_logger)
        m_logger->information("llama-server started, PID=" +
                              std::to_string(m_processId));

    // Start health-check thread (polls /health until 200 or timeout)
    m_healthCancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto* healthThread = new ServerHealthThread(
        m_eventHandler, GetBaseUrl(), m_healthCancelFlag, m_aliveToken,
        m_processHandle, logPath, 120000); // 2min timeout

    if (healthThread->Run() != wxTHREAD_NO_ERROR) {
        delete healthThread;
        if (m_logger)
            m_logger->error("Failed to start health-check thread");
    }

    return true;
#else
    // Non-Windows: not implemented
    (void)ggufPath; (void)config;
    return false;
#endif
}

// ── Stop server ──────────────────────────────────────────────────

void ServerManager::StopServer()
{
    // Cancel health check thread first
    if (m_healthCancelFlag) {
        m_healthCancelFlag->store(true);
        m_healthCancelFlag.reset();
    }

    KillProcess();
    m_loadedModel.clear();
    m_loadedMmproj.clear();
    m_currentJinjaEnabled = false;
}

// ── --jinja retry (Phase 3a) ────────────────────────────────────
//
// MyFrame::OnServerError calls this BEFORE delegating to the
// ModelSwitcher.  When llama-server fails to start with --jinja
// (most commonly because the active model's chat_template doesn't
// compile cleanly under our Jinja runtime), we fall back to a
// jinja-off relaunch with the same model and config.  The user
// sees a brief log line and the model loads normally; only if the
// no-jinja attempt ALSO fails does the original error surface to
// the UI.
//
// This is per-load-attempt only.  Once a load succeeds (or both
// attempts fail), the retry state resets and the next StartServer
// call tries jinja first again.  Persistent "this model never
// supports jinja" caching is Phase 3b's job.
//
// Returns true iff a retry was kicked off.  The caller suppresses
// the user-visible error in that case.
bool ServerManager::MaybeRetryWithoutJinja(const std::string& error)
{
    // Already retried this load attempt — give up and surface the
    // error.  (Belt-and-braces: also covers the case where the
    // no-jinja retry itself fails.)
    if (m_jinjaRetryAttempted) {
        ResetJinjaRetryState();
        return false;
    }

    // Jinja was off this attempt — nothing to fall back from.  This
    // can happen if the user is on a build that has m_jinjaForceOff
    // wired by some future setting, or if MaybeRetryWithoutJinja was
    // called when it shouldn't have been.
    if (m_jinjaForceOff) {
        ResetJinjaRetryState();
        return false;
    }

    // No model cached — we were never asked to start anything, or
    // StopServer cleared things out.  Surface the error.
    if (m_lastGgufPath.empty()) {
        return false;
    }

    // Only retry without --jinja when the failure actually looks like
    // a Jinja/chat-template problem. Otherwise we would hide unrelated
    // failures by launching a second server attempt that can produce a
    // different, more confusing error.
    if (!LooksLikeJinjaOrTemplateFailure(error)) {
        if (m_logger) {
            m_logger->warning(
                "Server failed while --jinja was enabled, but the error "
                "does not look Jinja/template-related; not retrying without "
                "--jinja. Error: " + error);
        }
        ResetJinjaRetryState();
        return false;
    }

    if (m_logger) {
        m_logger->warning(
            "Server failed to start with --jinja/template error; retrying "
            "without --jinja.  Original error: " + error);
    }

    // Set the retry flags BEFORE re-invoking StartServer, since
    // StartServer reads m_jinjaForceOff and respects it (and the
    // !m_jinjaForceOff branch in StartServer that resets
    // m_jinjaRetryAttempted is bypassed when the flag is set).
    m_jinjaForceOff       = true;
    m_jinjaRetryAttempted = true;

    bool ok = StartServer(m_lastGgufPath, m_lastConfig);
    if (!ok) {
        // Synchronous failure on the retry — clean up the retry
        // state so the next user-driven load tries jinja again.
        // The error from StartServer's own wxEVT_SERVER_ERROR post
        // will reach OnServerError, which will see
        // m_jinjaRetryAttempted == true and return false here,
        // allowing the error to propagate to the user normally.
        if (m_logger) {
            m_logger->error("Retry without --jinja also failed.");
        }
        // Don't reset state here — let the inevitable
        // wxEVT_SERVER_ERROR for the retry pass through OnServerError
        // and trigger the m_jinjaRetryAttempted == true branch above,
        // which calls ResetJinjaRetryState().
    }

    return true;   // We took ownership of the error; suppress UI.
}

void ServerManager::ResetJinjaRetryState()
{
    m_jinjaForceOff       = false;
    m_jinjaRetryAttempted = false;
}

void ServerManager::NotifyServerReady()
{
    // Successful start (whether on the first --jinja attempt or on
    // the retry without it) clears the per-load retry state so the
    // next user-initiated load attempt starts fresh and tries --jinja.
    // Do NOT clear m_currentJinjaEnabled here; it describes the actual
    // process currently running and is used by protocol detection.
    ResetJinjaRetryState();
}

bool ServerManager::IsProcessRunning() const
{
#ifdef __WXMSW__
    if (m_processHandle == INVALID_HANDLE_VALUE)
        return false;

    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_processHandle, &exitCode))
        return exitCode == STILL_ACTIVE;
#endif
    return false;
}

void ServerManager::KillProcess()
{
#ifdef __WXMSW__
    if (m_processHandle != INVALID_HANDLE_VALUE) {
        if (m_logger)
            m_logger->information("Stopping llama-server PID=" +
                                  std::to_string(m_processId));

        TerminateProcess(m_processHandle, 0);
        WaitForSingleObject(m_processHandle, 5000); // Wait up to 5s
        CloseHandle(m_processHandle);
        m_processHandle = INVALID_HANDLE_VALUE;
    }
    if (m_threadHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_threadHandle);
        m_threadHandle = INVALID_HANDLE_VALUE;
    }
    m_processId = 0;
#endif
}
