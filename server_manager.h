// server_manager.h
#pragma once

#include <wx/wx.h>
#include <wx/thread.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>

#ifdef __WXMSW__
#include <windows.h>
#endif

#include <Poco/Logger.h>

// ── Server launch configuration ─────────────────────────────────
struct ServerConfig
{
    int  port       = 8384;
    int  gpuLayers  = -1;      // -1 = offload all layers to GPU
    int  ctxSize    = 8192;
    int  threads    = 0;       // 0 = auto-detect
    bool flashAttn  = true;
};

// ── Backend type ────────────────────────────────────────────────
enum class Backend { CPU, CUDA12 };

// ── Custom events ───────────────────────────────────────────────
wxDECLARE_EVENT(wxEVT_SERVER_READY, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_SERVER_ERROR, wxCommandEvent);

// ── Health-check thread ─────────────────────────────────────────
// Polls GET /health until 200 or timeout, then posts an event.
class ServerHealthThread : public wxThread
{
public:
    ServerHealthThread(wxEvtHandler* handler,
                       const std::string& baseUrl,
                       std::shared_ptr<std::atomic<bool>> cancelFlag,
                       std::weak_ptr<std::atomic<bool>> aliveToken,
                       int timeoutMs = 60000);
protected:
    ExitCode Entry() override;
private:
    wxEvtHandler* m_handler;
    std::string   m_baseUrl;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    int m_timeoutMs;
    bool SafePost(wxCommandEvent* ev);
};

// ── Server manager ──────────────────────────────────────────────
class ServerManager
{
public:
    ServerManager(wxEvtHandler* eventHandler,
                  std::weak_ptr<std::atomic<bool>> aliveToken,
                  Poco::Logger* logger = nullptr);
    ~ServerManager();

    // Lifecycle
    bool StartServer(const std::string& ggufPath,
                     const ServerConfig& config = ServerConfig());
    void StopServer();
    bool IsProcessRunning() const;

    // Accessors
    std::string GetBaseUrl() const;
    std::string GetLoadedModel() const { return m_loadedModel; }

    // Display name: "/path/to/model.gguf" -> "model"
    static std::string ModelDisplayName(const std::string& ggufPath);

    // ── Static utilities ────────────────────────────────────────
    static Backend     DetectBackend();
    static std::string FindServerBinary(Backend backend);

    // Token-match an mmproj .gguf to the loaded model; returns empty if none found.
    static std::string FindMatchingMmproj(const std::string& modelGgufPath,
                                          Poco::Logger* logger = nullptr);

    // ── Data directory helpers ───────────────────────────────────
    static std::string GetDataDir();           // %LOCALAPPDATA%\LlamaBoss
    static std::string GetModelsDir();         // Documents\LlamaBoss\models
    static std::string GetLogsDir();
    static std::string GetConfigDir();
    static std::string GetConversationsDir();
    static std::string GetCacheDir();
    static void        EnsureDataDirs();

    // Scan models\ for *.gguf files (returns full paths, sorted)
    static std::vector<std::string> ScanModels();

private:
#ifdef __WXMSW__
    HANDLE m_processHandle = INVALID_HANDLE_VALUE;
    HANDLE m_threadHandle  = INVALID_HANDLE_VALUE;
    DWORD  m_processId     = 0;
#endif

    wxEvtHandler* m_eventHandler;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    Poco::Logger* m_logger;
    std::string   m_loadedModel;
    int           m_port = 8384;
    std::shared_ptr<std::atomic<bool>> m_healthCancelFlag;

    void KillProcess();
};
