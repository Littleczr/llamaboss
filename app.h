// app.h
#pragma once

// ── MyApp ────────────────────────────────────────────────────────
// Application object, extracted from the bottom of LlamaBoss.cpp.
//
// Ownership (Chunk C complete): MyApp owns the app-level singletons
// — AppState (settings, theme, logger, secrets/endpoint stores) and
// ModelService (ServerManager, i.e. llama-server's lifetime).  Both
// exist before any window does and outlive every window; frames
// borrow them via wxGetApp().
//
// Member declaration order is load-bearing: m_appState is declared
// before m_modelService, so C++ reverse-destruction order tears the
// service (and llama-server) down BEFORE AppState.  Do not reorder.

#include <wx/wx.h>
#include <wx/snglinst.h>

#include <memory>

#include "conversation_registry.h"

class AppState;
class ModelService;

class MyApp : public wxApp
{
public:
    bool OnInit() override;
    int  OnExit() override;

    // Valid from the moment OnInit succeeds until process exit — by
    // construction, every frame's lifetime sits inside that window,
    // so frame code may call these unconditionally.
    AppState&     GetAppState()     { return *m_appState; }
    ModelService& GetModelService() { return *m_modelService; }

    // Which window has which conversation open (Phase 3b guard
    // against the same conversation being edited in two windows).
    ConversationRegistry& GetConversationRegistry()
    {
        return m_conversationRegistry;
    }

    // ── Single-instance handoff (Phase 3d) ───────────────────────
    // A second desktop-shortcut launch detects this process via
    // wxSingleInstanceChecker, signals the named new-window event,
    // and exits.  The listener thread marshals that signal here on
    // the main thread; the result is byte-for-byte the Ctrl+Shift+N
    // behavior — a full MyFrame borrowing the app-owned singletons,
    // joining whatever server/target is already active.
    void OpenNewWindowFromSecondLaunch();

private:
    // Declaration order matters — see header comment.
    std::unique_ptr<AppState>     m_appState;
    std::unique_ptr<ModelService> m_modelService;
    ConversationRegistry          m_conversationRegistry;

    // Held for the whole process lifetime; its existence is what a
    // second launch detects.  Declared last so it releases first —
    // harmless either way, but keeps "the lock outlives everything
    // the lock protects" trivially true.
    std::unique_ptr<wxSingleInstanceChecker> m_instanceChecker;
};

wxDECLARE_APP(MyApp);

// Factory implemented in LlamaBoss.cpp — MyFrame's definition is
// file-local there, so the app TU creates the main window through
// this seam instead of naming the type.
wxFrame* CreateMainFrame();
