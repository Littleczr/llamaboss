// server_manager.cpp
#include "server_manager.h"

#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/stdpaths.h>

#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>

#include <sstream>
#include <algorithm>
#include <chrono>
#include <set>

// ── Event definitions ────────────────────────────────────────────
wxDEFINE_EVENT(wxEVT_SERVER_READY, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_SERVER_ERROR, wxCommandEvent);

// ═══════════════════════════════════════════════════════════════════
//  ServerHealthThread — polls /health until 200 or timeout
// ═══════════════════════════════════════════════════════════════════

ServerHealthThread::ServerHealthThread(wxEvtHandler* handler,
                                       const std::string& baseUrl,
                                       std::shared_ptr<std::atomic<bool>> cancelFlag,
                                       std::weak_ptr<std::atomic<bool>> aliveToken,
                                       int timeoutMs)
    : wxThread(wxTHREAD_DETACHED)
    , m_handler(handler)
    , m_baseUrl(baseUrl)
    , m_cancelFlag(cancelFlag)
    , m_aliveToken(aliveToken)
    , m_timeoutMs(timeoutMs)
{
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
            auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
            ev->SetString("Server did not become ready within "
                          + std::to_string(m_timeoutMs / 1000) + " seconds");
            SafePost(ev);
            return (ExitCode)0;
        }

        try {
            Poco::URI uri(m_baseUrl + "/health");
            Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
            sess.setTimeout(Poco::Timespan(2, 0));

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
    wxFileName fn(ggufPath);
    // e.g. "gemma-2-9b-it-Q4_K_M.gguf" -> "gemma-2-9b-it-Q4_K_M"
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

std::string ServerManager::GetModelsDir()
{
    // Models live in Documents\LlamaBoss\models — a location users can
    // actually find and browse. All other app data stays in %LOCALAPPDATA%.
    wxString docs = wxStandardPaths::Get().GetDocumentsDir();
    return std::string(docs.ToUTF8().data())
         + std::string(1, wxFILE_SEP_PATH) + "LlamaBoss"
         + std::string(1, wxFILE_SEP_PATH) + "models";
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
}

std::vector<std::string> ServerManager::ScanModels()
{
    std::vector<std::string> models;
    wxString modelsDir = GetModelsDir();

    if (!wxDir::Exists(modelsDir))
        return models;

    wxDir dir(modelsDir);
    if (!dir.IsOpened())
        return models;

    wxString filename;
    bool found = dir.GetFirst(&filename, "*.gguf", wxDIR_FILES);
    while (found) {
        // Skip multimodal projector files — they're not chat models
        if (filename.Lower().Contains("mmproj")) {
            found = dir.GetNext(&filename);
            continue;
        }
        wxString fullPath = modelsDir + wxFILE_SEP_PATH + filename;
        models.push_back(fullPath.ToUTF8().data());
        found = dir.GetNext(&filename);
    }

    std::sort(models.begin(), models.end());
    return models;
}

// ── Token-based mmproj matcher ──────────────────────────────────
//
// Tokenises a GGUF filename (split on - _ .), lowercases everything,
// strips quant/format noise tokens (q4, k_m, f16, bf16, gguf, mmproj),
// then scores each *mmproj*.gguf candidate by shared-token count.
// Returns the full path of the best match, or empty if nothing fits.

// Noise tokens that appear in filenames but carry no model-identity info.
static const std::set<std::string> kNoiseTokens = {
    "gguf", "mmproj",
    // Quant tags
    "q2", "q3", "q4", "q5", "q6", "q8",
    "k",  "km", "ks", "kl",
    "k_m", "k_s", "k_l",
    "iq1", "iq2", "iq3", "iq4",
    // Precision tags
    "f16", "f32", "bf16", "fp16", "fp32",
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
        logger->information(
            "mmproj: matched \"" + result +
            "\" (score " + std::to_string(bestScore) + "/" +
            std::to_string(modelTokens.size()) + ")");

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
                      "Models go in:\n"
                      "Documents\\LlamaBoss\\models\\");
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
            " port=" + std::to_string(config.port));
    }

    // Build command line
    std::ostringstream cmd;
    cmd << "\"" << serverBin << "\""
        << " -m \"" << ggufPath << "\""
        << " --port " << config.port
        << " -c " << config.ctxSize;

    if (config.gpuLayers != 0)
        cmd << " -ngl " << config.gpuLayers;

    if (config.flashAttn)
        cmd << " --flash-attn on";

    if (config.threads > 0)
        cmd << " -t " << config.threads;

    // Auto-detect multimodal projector (mmproj) for vision models.
    // Token-matches the model filename against *mmproj*.gguf candidates.
    {
        std::string mmproj = FindMatchingMmproj(ggufPath, m_logger);
        if (!mmproj.empty())
            cmd << " --mmproj \"" << mmproj << "\"";
    }

    std::string cmdLine = cmd.str();

    if (m_logger)
        m_logger->information("Command: " + cmdLine);

    // Set up log file for server stdout/stderr
    EnsureDataDirs();
    std::string logPath = GetLogsDir() + std::string(1, wxFILE_SEP_PATH) + "server.log";

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hLogFile = CreateFileA(
        logPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,    // Allow reading while server runs
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    // Set up process creation
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    if (hLogFile != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hLogFile;
        si.hStdError  = hLogFile;
        si.hStdInput  = NULL;
    }

    PROCESS_INFORMATION pi = {};

    // CreateProcessA needs a mutable command line buffer
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    // Working directory = folder containing the server binary
    // (so DLLs like ggml-cuda.dll are found)
    wxFileName binFn(serverBin);
    std::string workDir = binFn.GetPath().ToUTF8().data();

    BOOL ok = CreateProcessA(
        NULL,               // lpApplicationName (embedded in cmdLine)
        cmdBuf.data(),      // lpCommandLine
        NULL, NULL,         // process/thread security
        TRUE,               // bInheritHandles (for log file)
        CREATE_NO_WINDOW,   // dwCreationFlags
        NULL,               // lpEnvironment
        workDir.c_str(),    // lpCurrentDirectory
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

    m_processHandle = pi.hProcess;
    m_threadHandle  = pi.hThread;
    m_processId     = pi.dwProcessId;
    m_loadedModel   = ggufPath;

    if (m_logger)
        m_logger->information("llama-server started, PID=" +
                              std::to_string(m_processId));

    // Start health-check thread (polls /health until 200 or timeout)
    m_healthCancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto* healthThread = new ServerHealthThread(
        m_eventHandler, GetBaseUrl(), m_healthCancelFlag, m_aliveToken, 120000); // 2min timeout

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
