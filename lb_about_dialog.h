#pragma once
//
// lb_about_dialog.h — themed About dialog with in-place update flow.
//
// Replaces the old wxMessageDialog + chat-transcript status lines.  All
// update feedback (checking / up to date / available / downloading /
// errors) stays inside this dialog; nothing is written to the chat.
//
// The dialog checks and downloads.  It does NOT launch the installer:
// when a verified installer is ready it ends with ID_INSTALL and the
// frame (which owns window/close policy) calls InstallUpdateAndQuit().
//
#include "theme.h"
#include "update_checker.h"

#include <wx/wx.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

class wxGauge;

class LbAboutDialog final : public wxDialog
{
public:
    // Returned from ShowModal() when a verified installer is ready.
    static constexpr int ID_INSTALL = wxID_HIGHEST + 4101;

    // Returns a non-empty, user-facing reason when an install must not
    // start right now (e.g. a chat is still generating), else empty.
    using InstallBlockerFn = std::function<wxString()>;

    LbAboutDialog(wxWindow* parent,
                  const ThemeData& theme,
                  const std::string& currentVersion,
                  const std::string& modelDisplayName,
                  const std::string& endpoint,
                  const std::string& modelsDir,
                  InstallBlockerFn installBlocker);
    ~LbAboutDialog() override;

    const std::wstring& GetInstallerPath() const { return m_installerPath; }

private:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Error };

    void BuildUi(const std::string& modelDisplayName,
                 const std::string& endpoint,
                 const std::string& modelsDir);
    void SetState(State s, const wxString& status);

    void OnPrimary(wxCommandEvent&);
    void OnSecondary(wxCommandEvent&);

    void StartCheck();
    void StartDownload();
    void OpenDownloadPage();
    bool CanInstallInApp() const;

    void OnCheckResult(wxThreadEvent& ev);
    void OnDownloadProgress(wxThreadEvent& ev);
    void OnDownloadResult(wxThreadEvent& ev);

    ThemeData   m_theme;
    std::string m_currentVersion;
    InstallBlockerFn m_installBlocker;

    State m_state = State::Idle;
    UpdateChecker::UpdateInfo m_info;
    std::wstring m_installerPath;

    // Worker plumbing: alive token gates event posts; the cancel flag is
    // shared so it outlives the dialog if a download is mid-read.
    std::shared_ptr<std::atomic<bool>> m_alive;
    std::shared_ptr<std::atomic<bool>> m_cancel;

    wxStaticText* m_status    = nullptr;
    wxStaticText* m_notes     = nullptr;
    wxGauge*      m_gauge     = nullptr;
    wxButton*     m_primary   = nullptr;
    wxButton*     m_secondary = nullptr;
};
