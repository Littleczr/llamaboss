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
#include <vector>

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
    // ChatHistory's trust flag is in-memory only and LoadFromFile
    // always builds a fresh history, so on its own "Approve"
    // (one-approval mode) would die on every conversation reload.
    // These two methods give trust app-session lifetime instead:
    // granting trust records the conversation's normalized path here,
    // and the load path in ConversationController re-arms the
    // ChatHistory flag when the same file is opened again in this run
    // of LlamaBoss.
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

    // Chat-scoped folder capabilities follow a saved conversation while the
    // app remains open, just like one-approval mode. The map is never
    // serialized, so restarting LlamaBoss clears every external write grant.
    void RememberSessionWriteRoot(const std::string& conversationPath,
                                  const std::string& writeRoot)
    {
        const std::string key = Normalize(conversationPath);
        if (key.empty() || writeRoot.empty()) return;

        auto& roots = m_sessionWriteRoots[key];
        const std::string normalizedRoot = Normalize(writeRoot);
        for (const std::string& existing : roots) {
            if (Normalize(existing) == normalizedRoot) return;
        }
        roots.push_back(writeRoot);
    }

    std::vector<std::string> SessionWriteRoots(
        const std::string& conversationPath) const
    {
        const std::string key = Normalize(conversationPath);
        auto it = m_sessionWriteRoots.find(key);
        return (it == m_sessionWriteRoots.end())
            ? std::vector<std::string>{}
            : it->second;
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
    std::map<std::string, std::vector<std::string>> m_sessionWriteRoots;
};
