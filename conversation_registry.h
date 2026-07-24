// conversation_registry.h
#pragma once

// ── ConversationRegistry ─────────────────────────────────────────
// App-level "which window has which conversation open" map (Phase
// 3b of the multi-window plan).  Two windows saving the same
// conversation JSON is last-writer-wins message loss, so the same
// conversation must never be open in two windows at once.  The
// guard lives at the single load choke point
// (ConversationController::LoadConversationFromPath): if another
// window owns the path, that window is raised instead of loading —
// VS Code behavior.
//
// Model: one claim per frame (a window shows exactly one
// conversation at a time; an unsaved new chat claims the empty
// string, which never matches anything).  Claims are refreshed at
// every point a window's current path changes: load, first
// autosave, Save-As, New Chat, window close.
//
// Threading: main thread only, like every other UI-adjacent
// registry in the app.

#include <wx/frame.h>
#include <wx/filename.h>

#include <map>
#include <set>
#include <string>

class ConversationRegistry
{
public:
    // Record |frame|'s current conversation (empty string = a new,
    // not-yet-saved chat, i.e. no claim).
    void SetCurrent(wxFrame* frame, const std::string& path)
    {
        if (!frame) return;
        m_current[frame] = Normalize(path);
    }

    void Remove(wxFrame* frame)
    {
        m_current.erase(frame);
    }

    // ── Session-scoped approval trust ────────────────────────────
    // "Approve" (one-approval mode) used to die on every conversation
    // reload because ChatHistory's trust flag is in-memory only and
    // LoadFromFile always builds a fresh history.  These two methods
    // give trust app-session lifetime instead: granting trust records
    // the conversation's normalized path here, and the load path in
    // ConversationController re-arms the ChatHistory flag when the
    // same file is opened again in this run of LlamaBoss.
    //
    // Deliberately NOT persisted to disk: a chat reopened weeks later
    // should not silently carry pre-approved delete/script-create.
    // Restarting the app is the reset. python_install_package remains
    // per-card regardless (IsToolChatApproved hard-refuses it).
    // Empty paths (unsaved new chats) are ignored; the in-memory flag
    // covers those until AutoSaveConversation assigns a path and
    // syncs the claim here.
    void RememberSessionTrust(const std::string& path)
    {
        const std::string key = Normalize(path);
        if (!key.empty()) m_sessionTrust.insert(key);
    }

    bool HasSessionTrust(const std::string& path) const
    {
        const std::string key = Normalize(path);
        return !key.empty() && m_sessionTrust.count(key) > 0;
    }

    // The frame (other than |exclude|) that currently has |path|
    // open, or nullptr.  Empty paths never match.
    wxFrame* OwnerOf(const std::string& path, const wxFrame* exclude) const
    {
        const std::string key = Normalize(path);
        if (key.empty()) return nullptr;
        for (const auto& [frame, current] : m_current) {
            if (frame != exclude && current == key)
                return frame;
        }
        return nullptr;
    }

private:
    // Case-folded absolute path so the same file always compares
    // equal regardless of how the caller spelled it.  Conversation
    // paths all come from the same sidebar scan today, but the
    // Ctrl+O file dialog can produce a differently-cased spelling
    // of the same file on Windows.
    static std::string Normalize(const std::string& path)
    {
        if (path.empty()) return {};
        wxFileName fn(wxString::FromUTF8(path));
        fn.MakeAbsolute();
        wxString full = fn.GetFullPath();
#ifdef __WXMSW__
        full.MakeLower();
#endif
        return std::string(full.ToUTF8());
    }

    std::map<wxFrame*, std::string> m_current;

    // Normalized paths of conversations granted one-approval mode
    // during this app session.  App lifetime; never saved to disk.
    std::set<std::string> m_sessionTrust;
};
