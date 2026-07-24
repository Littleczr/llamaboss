// app.cpp

#include "app.h"

#include "app_state.h"
#include "model_service.h"
#include "server_manager.h"   // EnsureDataDirs

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#include <wx/clipbrd.h>
#include <thread>
#endif

// ── Single-instance handoff machinery (Phase 3d) ─────────────────
// A second LlamaBoss.exe launch must not boot a second llama-server
// (port collision, double VRAM) — it should behave like Ctrl+Shift+N
// in the running process.  Handshake: the primary owns a named
// auto-reset Win32 event and a listener thread parked on it; a
// secondary launch (detected via wxSingleInstanceChecker) signals
// the event and exits.  The listener marshals each signal to the
// main thread with CallAfter, where OpenNewWindowFromSecondLaunch
// creates the frame.
//
// Kept as file statics rather than MyApp members so app.h stays free
// of <windows.h> and <thread>.  Lifetime discipline: the thread is
// started only after the first frame is successfully shown (so an
// OnInit failure path — where wx never calls OnExit — can't leave a
// joinable std::thread to terminate() the process at static
// destruction), and OnExit stops and joins it before any teardown.
namespace {

#ifdef __WXMSW__
// "Local\" scopes the name to this login session — two different
// users on the same machine each get their own instance, matching
// wxSingleInstanceChecker's per-user default.
constexpr wchar_t kNewWindowEventName[] = L"Local\\LlamaBoss.OpenNewWindow";

HANDLE      g_newWindowEvent = nullptr;   // named, auto-reset
HANDLE      g_listenerStop   = nullptr;   // unnamed, manual-reset
std::thread g_listenerThread;

void StartNewWindowListener()
{
    g_newWindowEvent = ::CreateEventW(nullptr, FALSE /*auto-reset*/,
                                      FALSE, kNewWindowEventName);
    g_listenerStop   = ::CreateEventW(nullptr, TRUE /*manual-reset*/,
                                      FALSE, nullptr);
    if (!g_newWindowEvent || !g_listenerStop) {
        // Degrade to today's behavior: a second launch will show the
        // port-collision message from ServerManager instead.
        if (g_newWindowEvent) { ::CloseHandle(g_newWindowEvent); g_newWindowEvent = nullptr; }
        if (g_listenerStop)   { ::CloseHandle(g_listenerStop);   g_listenerStop   = nullptr; }
        return;
    }

    g_listenerThread = std::thread([]() {
        HANDLE handles[2] = { g_listenerStop, g_newWindowEvent };
        for (;;) {
            const DWORD r = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0 + 1)
                return;   // stop event, or an error — either way, done

            // CallAfter is thread-safe (it queues an event); the
            // lambda runs on the main thread inside the app's normal
            // event loop.  OnExit joins this thread before teardown,
            // so wxTheApp is always valid here.
            wxTheApp->CallAfter([]() {
                static_cast<MyApp*>(wxTheApp)->OpenNewWindowFromSecondLaunch();
            });
        }
    });
}

void StopNewWindowListener()
{
    if (g_listenerStop) ::SetEvent(g_listenerStop);
    if (g_listenerThread.joinable()) g_listenerThread.join();
    if (g_newWindowEvent) { ::CloseHandle(g_newWindowEvent); g_newWindowEvent = nullptr; }
    if (g_listenerStop)   { ::CloseHandle(g_listenerStop);   g_listenerStop   = nullptr; }
}
#else
void StartNewWindowListener() {}
void StopNewWindowListener()  {}
#endif

// Runs in the SECONDARY process.  Ask the primary to open a window,
// then the caller exits by returning false from OnInit.
void SignalPrimaryInstanceToOpenWindow()
{
#ifdef __WXMSW__
    // Windows foreground rules: this process holds the foreground
    // right (the user just double-clicked us); the primary doesn't.
    // Grant it away before signaling so the primary's Raise() on the
    // new frame actually comes to the front instead of just flashing
    // the taskbar.
    ::AllowSetForegroundWindow(ASFW_ANY);

    // The primary may still be mid-startup (checker created, event
    // not yet).  Retry briefly before giving up.
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE ev = ::OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                 kNewWindowEventName);
        if (ev) {
            ::SetEvent(ev);
            ::CloseHandle(ev);
            return;
        }
        ::Sleep(100);
    }
#endif
    wxMessageBox(
        "LlamaBoss is already running, but it did not respond to the "
        "new-window request.\n\nUse Ctrl+Shift+N in the existing window "
        "to open another one.",
        "LlamaBoss", wxOK | wxICON_INFORMATION);
}

} // namespace

bool MyApp::OnInit()
{
    if (!wxApp::OnInit()) return false;
    // Set app name explicitly so wxStandardPaths::GetUserLocalDataDir()
    // always returns %LOCALAPPDATA%\LlamaBoss regardless of exe filename.
    // Must precede AppState::Initialize(): the logger, config, and
    // secrets paths all resolve through wxStandardPaths.
    SetAppName("LlamaBoss");
    SetAppDisplayName("LlamaBoss");

    // ── Single-instance check (Phase 3d) ─────────────────────────
    // Must come right after SetAppName (nothing else needed) and
    // before any real initialization: a secondary launch should do
    // zero work — no data dirs, no settings load, no logger — just
    // hand off and exit.  The checker object is held for the whole
    // process lifetime; releasing it is what lets the NEXT launch
    // become primary.
    m_instanceChecker = std::make_unique<wxSingleInstanceChecker>(
        "LlamaBoss-" + wxGetUserId());
    if (m_instanceChecker->IsAnotherRunning()) {
        SignalPrimaryInstanceToOpenWindow();
        return false;   // this process exits; the primary opens the window
    }

    wxInitAllImageHandlers();

    // ── App-level singletons, in dependency order ────────────────
    // Data dirs first (cheap no-op when they exist), then AppState
    // (settings + logger), then ModelService (needs the logger).
    // All of this used to happen inside the first frame's ctor; it
    // lives here now so N frames can borrow one instance of each.
    ServerManager::EnsureDataDirs();

    m_appState = std::make_unique<AppState>();
    if (!m_appState->Initialize()) {
        // Initialize() returning false means settings/config are
        // unusable (the logger alone degrading is non-fatal and does
        // NOT land here — see AppState::Initialize).  Mirror the
        // pre-refactor failure behavior: explain and refuse to start.
        wxMessageBox("LlamaBoss could not start.\n\n"
                     "Failed to initialize application state "
                     "(settings/configuration unavailable).",
                     "Startup Error", wxOK | wxICON_ERROR);
        return false;
    }

    m_modelService = std::make_unique<ModelService>(*m_appState);

    try {
        wxFrame* frame = CreateMainFrame();
        frame->Show();

        // Start listening for second-launch handoffs only once the
        // app is fully up.  Deliberately after Show(): if anything
        // above threw, OnInit returns false, wx never calls OnExit,
        // and a running listener thread would have nobody to join it.
        StartNewWindowListener();
        return true;
    }
    catch (const std::exception& ex) {
        const wxString msg = wxString::FromUTF8("LlamaBoss could not start.\n\n")
            + wxString::FromUTF8(ex.what());
        wxMessageBox(msg, "Startup Error", wxOK | wxICON_ERROR);
        return false;
    }
}

void MyApp::OpenNewWindowFromSecondLaunch()
{
    // A second desktop-shortcut launch handed off to us and exited.
    // Mirror MyFrame::OpenNewWindow (Ctrl+Shift+N): a full MyFrame
    // borrowing the app-owned AppState and ModelService; the ctor's
    // ConsumeInitialBootstrap gate means it joins the already-running
    // server or remote target instead of booting another one.
    wxFrame* frame = nullptr;
    try {
        frame = CreateMainFrame();
    }
    catch (const std::exception& ex) {
        if (m_appState) {
            if (auto* logger = m_appState->GetLogger())
                logger->error(
                    std::string("Second-launch new window failed: ") +
                    ex.what());
        }
        return;
    }

    // Cascade from an existing window so the new one doesn't land
    // exactly on top and look like nothing happened.  RestoreWindowState
    // in the ctor may have re-applied a saved maximized state, and
    // SetPosition on a maximized frame is ignored — un-maximize first
    // so the offset actually takes.
    if (frame->IsMaximized())
        frame->Maximize(false);
    if (wxWindow* top = GetTopWindow()) {
        const wxPoint pos = top->GetPosition();
        frame->SetPosition(wxPoint(pos.x + 48, pos.y + 48));
    }

    // The secondary called AllowSetForegroundWindow before signaling,
    // so this Raise comes to the front instead of taskbar-flashing.
    frame->Show();
    frame->Raise();
}

int MyApp::OnExit()
{
    // Keep whatever LlamaBoss put on the clipboard alive after we exit.
    // wxWidgets on MSW places clipboard data in application-owned /
    // delayed-render mode: the OS clipboard holds a promise and the
    // actual bytes live in this process, and wx's cleanup EMPTIES the
    // clipboard on exit unless Flush() renders it out first
    // (OleFlushClipboard underneath).  Without this, copying a chat
    // response and then closing the app made the paste target come up
    // empty.  Covers Ctrl+C from the transcript, the input box, and
    // the code-block [Copy] button alike.  Runs first, while the app
    // is fully alive; harmless no-op when we don't own the clipboard.
    if (wxTheClipboard)
        wxTheClipboard->Flush();

    // Stop and join the second-launch listener before anything else
    // tears down — its CallAfter target is this app object.
    StopNewWindowListener();

    // Backstop teardown for paths that never route through
    // MyFrame::OnClose (direct Destroy(), app-shutdown top-level
    // window cleanup).  StopServer() is idempotent, so doubling up
    // with the frame's explicit shutdown is harmless — the point is
    // that llama-server (and its VRAM) never outlives the app no
    // matter which exit path fired.  Member destruction order then
    // finishes the job: service before AppState.
    if (m_modelService)
        m_modelService->Shutdown();

    return wxApp::OnExit();
}

wxIMPLEMENT_APP(MyApp);
