//
// lb_about_dialog.cpp — see lb_about_dialog.h.
//
#include "lb_about_dialog.h"

#include "lb_background_threads.h"
#include "lb_update_ui.h"          // LbIsTrustedUpdateUrl, LbUpdateFallbackUrl
#include "ui_event_post.h"
#include "update_installer.h"
#include "widgets.h"               // ApplyDarkTitleBar

#include <wx/filefn.h>
#include <wx/gauge.h>
#include <wx/log.h>
#include <wx/hyperlink.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <cstdint>

wxDEFINE_EVENT(wxEVT_LB_ABOUT_CHECK_RESULT,   wxThreadEvent);
wxDEFINE_EVENT(wxEVT_LB_ABOUT_DL_PROGRESS,    wxThreadEvent);
wxDEFINE_EVENT(wxEVT_LB_ABOUT_DL_RESULT,      wxThreadEvent);

namespace {

// Same look as the helpers in lb_themed_dialogs.cpp.
wxButton* MakeButton(wxWindow* parent, const wxString& label,
                     const ThemeData& theme, bool accent)
{
    auto* b = new wxButton(parent, wxID_ANY, label, wxDefaultPosition,
                           wxSize(-1, 32), wxBORDER_NONE);
    wxFont f = b->GetFont();
    f.SetPointSize(10);
    f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    b->SetFont(f);
    b->SetBackgroundColour(accent ? theme.accentButton : theme.bgDialogSurface);
    b->SetForegroundColour(accent ? theme.accentButtonText : theme.textMuted);
    b->SetMinSize(wxSize(150, 32));
    return b;
}

wxStaticText* MakeText(wxWindow* parent, const wxString& s,
                       const wxColour& fg, int pt = 10, bool bold = false)
{
    auto* t = new wxStaticText(parent, wxID_ANY, s);
    wxFont f = t->GetFont();
    f.SetPointSize(pt);
    if (bold) f.SetWeight(wxFONTWEIGHT_BOLD);
    t->SetFont(f);
    t->SetForegroundColour(fg);
    return t;
}

wxPanel* MakeRule(wxWindow* parent, const ThemeData& theme)
{
    auto* line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(theme.borderSubtle);
    return line;
}

wxString Mb(std::uint64_t bytes)
{
    return wxString::Format("%.0f MB", bytes / (1024.0 * 1024.0));
}

} // namespace

LbAboutDialog::LbAboutDialog(wxWindow* parent,
                             const ThemeData& theme,
                             const std::string& currentVersion,
                             const std::string& modelDisplayName,
                             const std::string& endpoint,
                             const std::string& modelsDir,
                             InstallBlockerFn installBlocker)
    : wxDialog(parent, wxID_ANY, "About LlamaBoss",
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_theme(theme)
    , m_currentVersion(currentVersion)
    , m_installBlocker(std::move(installBlocker))
    , m_alive(std::make_shared<std::atomic<bool>>(true))
    , m_cancel(std::make_shared<std::atomic<bool>>(false))
{
    SetBackgroundColour(m_theme.bgDialogSurface);
    BuildUi(modelDisplayName, endpoint, modelsDir);

    Bind(wxEVT_LB_ABOUT_CHECK_RESULT, &LbAboutDialog::OnCheckResult, this);
    Bind(wxEVT_LB_ABOUT_DL_PROGRESS,  &LbAboutDialog::OnDownloadProgress, this);
    Bind(wxEVT_LB_ABOUT_DL_RESULT,    &LbAboutDialog::OnDownloadResult, this);

    ApplyDarkTitleBar(this, m_theme.name != "light");

    // Opening About is an explicit user action, so check right away
    // instead of making them click twice.  Delete this Bind to go back
    // to a manual "Check for Updates" button.
    Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
        e.Skip();
        if (e.IsShown() && m_state == State::Idle)
            CallAfter([this]() { StartCheck(); });
    });
}

LbAboutDialog::~LbAboutDialog()
{
    // Stop any in-flight download (the worker deletes its .part file)
    // and block late posts into this destroyed dialog.
    m_cancel->store(true, std::memory_order_release);
    LbMarkUiEventTargetDead(m_alive);
}

void LbAboutDialog::BuildUi(const std::string& modelDisplayName,
                            const std::string& endpoint,
                            const std::string& modelsDir)
{
    const int pad = 16;
    auto* top = new wxBoxSizer(wxVERTICAL);

    // ── Identity ────────────────────────────────────────────────────
    top->Add(MakeText(this, "LlamaBoss", m_theme.textPrimary, 16, true),
             0, wxLEFT | wxRIGHT | wxTOP, pad);
    top->Add(MakeText(this, wxString::FromUTF8("Beta v" + m_currentVersion),
                      m_theme.textMuted, 10),
             0, wxLEFT | wxRIGHT, pad);
    top->AddSpacer(8);
    top->Add(MakeText(this,
                      "Private local desktop AI assistant for Windows.\n"
                      "Powered by llama.cpp.",
                      m_theme.textPrimary, 10),
             0, wxLEFT | wxRIGHT, pad);

    top->Add(MakeRule(this, m_theme), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad - 4);

    // ── Current setup ───────────────────────────────────────────────
    auto* grid = new wxFlexGridSizer(2, 6, 12);
    grid->AddGrowableCol(1, 1);
    auto addRow = [&](const char* label, const std::string& value) {
        grid->Add(MakeText(this, label, m_theme.textMuted, 9), 0, wxALIGN_TOP);
        auto* v = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(value),
                                   wxDefaultPosition, wxSize(380, -1),
                                   wxST_ELLIPSIZE_MIDDLE | wxST_NO_AUTORESIZE);
        wxFont f = v->GetFont(); f.SetPointSize(9); v->SetFont(f);
        v->SetForegroundColour(m_theme.textPrimary);
        v->SetToolTip(wxString::FromUTF8(value));
        grid->Add(v, 1, wxEXPAND);
    };
    addRow("Model",    modelDisplayName.empty() ? std::string("(none)") : modelDisplayName);
    addRow("Endpoint", endpoint);
    addRow("Models",   modelsDir);
    top->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

    top->Add(MakeRule(this, m_theme), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad - 4);

    // ── Credits ─────────────────────────────────────────────────────
    auto* credits = new wxBoxSizer(wxHORIZONTAL);
    credits->Add(MakeText(this, wxString::FromUTF8("Created by Cesar Avelar  \xC2\xB7  "),
                          m_theme.textMuted, 9), 0, wxALIGN_CENTER_VERTICAL);
    auto* link = new wxHyperlinkCtrl(this, wxID_ANY, "llamaboss.com",
                                     "https://llamaboss.com");
    link->SetNormalColour(m_theme.accentButton);
    link->SetVisitedColour(m_theme.accentButton);
    link->SetHoverColour(m_theme.textPrimary);
    link->SetBackgroundColour(m_theme.bgDialogSurface);
    credits->Add(link, 0, wxALIGN_CENTER_VERTICAL);
    credits->Add(MakeText(this, wxString::FromUTF8("  \xC2\xB7  MIT License  \xC2\xB7  wxWidgets + Poco"),
                          m_theme.textMuted, 9), 0, wxALIGN_CENTER_VERTICAL);
    top->Add(credits, 0, wxLEFT | wxRIGHT, pad);
    top->AddSpacer(4);
    top->Add(MakeText(this, "Beta software: features and behavior may change before 1.0.",
                      m_theme.textMuted, 9),
             0, wxLEFT | wxRIGHT, pad);

    top->Add(MakeRule(this, m_theme), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad - 4);

    // ── Updates ─────────────────────────────────────────────────────
    m_status = MakeText(this, wxEmptyString, m_theme.textPrimary, 10);
    top->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

    m_notes = MakeText(this, wxEmptyString, m_theme.textMuted, 9);
    top->Add(m_notes, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    m_notes->Hide();

    m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 6),
                          wxGA_HORIZONTAL | wxGA_SMOOTH);
    top->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad / 2);
    m_gauge->Hide();

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);
    m_primary   = MakeButton(this, "Check for Updates", m_theme, /*accent=*/true);
    m_secondary = MakeButton(this, "Close", m_theme, /*accent=*/false);
    m_secondary->SetMinSize(wxSize(96, 32));
    buttons->Add(m_primary, 0, wxRIGHT, 12);
    buttons->Add(m_secondary, 0);
    top->Add(buttons, 0, wxEXPAND | wxALL, pad);

    m_primary->Bind(wxEVT_BUTTON, &LbAboutDialog::OnPrimary, this);
    m_secondary->Bind(wxEVT_BUTTON, &LbAboutDialog::OnSecondary, this);

    // Esc routes through OnSecondary (cancel download first, else close).
    SetEscapeId(wxID_NONE);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            wxCommandEvent dummy;
            OnSecondary(dummy);
            return;
        }
        e.Skip();
    });

    SetSizerAndFit(top);
    SetMinSize(GetSize());
    CentreOnParent();

    SetState(State::Idle, wxString::FromUTF8("Installed: v" + m_currentVersion));
}

bool LbAboutDialog::CanInstallInApp() const
{
    return !m_info.installerUrl.empty() &&
           LbIsTrustedUpdateUrl(m_info.installerUrl) &&
           UpdateInstaller::IsValidSha256Hex(m_info.sha256);
}

void LbAboutDialog::SetState(State s, const wxString& status)
{
    m_state = s;
    m_status->SetLabel(status);
    m_status->Wrap(FromDIP(460));

    const bool showNotes = (s == State::Available || s == State::Downloading) &&
                           !m_info.notes.empty();
    if (showNotes) {
        m_notes->SetLabel(wxString::FromUTF8(m_info.notes));
        m_notes->Wrap(FromDIP(460));
    }
    m_notes->Show(showNotes);
    m_gauge->Show(s == State::Downloading);

    m_primary->Enable(true);
    m_secondary->SetLabel("Close");
    switch (s) {
    case State::Idle:
    case State::UpToDate:
    case State::Error:
        m_primary->SetLabel("Check for Updates");
        break;
    case State::Checking:
        m_primary->SetLabel(wxString::FromUTF8("Checking\xE2\x80\xA6"));
        m_primary->Enable(false);
        break;
    case State::Available:
        m_primary->SetLabel(CanInstallInApp() ? "Download and Install"
                                              : "Open Download Page");
        break;
    case State::Downloading:
        m_primary->SetLabel(wxString::FromUTF8("Downloading\xE2\x80\xA6"));
        m_primary->Enable(false);
        m_secondary->SetLabel("Cancel");
        break;
    }

    Layout();
    Fit();
}

void LbAboutDialog::OnPrimary(wxCommandEvent&)
{
    switch (m_state) {
    case State::Idle:
    case State::UpToDate:
    case State::Error:
        StartCheck();
        break;
    case State::Available:
        if (CanInstallInApp()) StartDownload();
        else                   OpenDownloadPage();
        break;
    default:
        break;
    }
}

void LbAboutDialog::OnSecondary(wxCommandEvent&)
{
    if (m_state == State::Downloading) {
        m_cancel->store(true, std::memory_order_release);
        m_secondary->Enable(false);   // re-enabled when the worker reports back
        m_status->SetLabel(wxString::FromUTF8("Cancelling\xE2\x80\xA6"));
        return;
    }
    EndModal(wxID_CANCEL);
}

void LbAboutDialog::StartCheck()
{
    if (m_state == State::Checking || m_state == State::Downloading) return;
    SetState(State::Checking, "Checking llamaboss.com for updates...");

    const std::string current = m_currentVersion;
    wxEvtHandler* target = this;
    std::weak_ptr<std::atomic<bool>> alive = m_alive;
    LbBackgroundThreadKeeper::Instance().Launch([target, alive, current]() {
        UpdateChecker::UpdateInfo info = UpdateChecker::CheckBlocking(current);
        auto* ev = new wxThreadEvent(wxEVT_LB_ABOUT_CHECK_RESULT);
        ev->SetPayload(info);
        LbQueueEventIfAlive(target, alive, ev);
    });
}

void LbAboutDialog::OnCheckResult(wxThreadEvent& ev)
{
    m_info = ev.GetPayload<UpdateChecker::UpdateInfo>();

    if (!m_info.ok) {
        SetState(State::Error, wxString::FromUTF8("Couldn't check for updates: " + m_info.error));
        return;
    }
    if (!m_info.available) {
        SetState(State::UpToDate,
                 wxString::FromUTF8("You're up to date (v" + m_currentVersion + ")."));
        return;
    }
    SetState(State::Available,
             wxString::FromUTF8("v" + m_info.latest + " is available  (installed: v" +
                                m_currentVersion + ")"));
}

void LbAboutDialog::StartDownload()
{
    if (m_installBlocker) {
        const wxString reason = m_installBlocker();
        if (!reason.empty()) {
            m_status->SetLabel(reason);
            m_status->Wrap(FromDIP(460));
            Layout(); Fit();
            return;
        }
    }

    m_cancel->store(false, std::memory_order_release);
    m_gauge->SetValue(0);
    SetState(State::Downloading,
             wxString::FromUTF8("Downloading v" + m_info.latest + "..."));

    const std::string url     = m_info.installerUrl;
    const std::string sha     = m_info.sha256;
    const std::string version = m_info.latest;
    wxEvtHandler* target = this;
    std::weak_ptr<std::atomic<bool>> alive = m_alive;
    std::shared_ptr<std::atomic<bool>> cancel = m_cancel;

    LbBackgroundThreadKeeper::Instance().Launch(
        [target, alive, cancel, url, sha, version]() {
            auto progress = [target, alive](std::uint64_t done, std::uint64_t total) {
                auto* p = new wxThreadEvent(wxEVT_LB_ABOUT_DL_PROGRESS);
                p->SetPayload(std::make_pair(done, total));
                LbQueueEventIfAlive(target, alive, p);
            };
            UpdateInstaller::DownloadResult r =
                UpdateInstaller::DownloadAndVerify(url, sha, version, progress, *cancel);
            auto* ev = new wxThreadEvent(wxEVT_LB_ABOUT_DL_RESULT);
            ev->SetPayload(r);
            LbQueueEventIfAlive(target, alive, ev);
        });
}

void LbAboutDialog::OnDownloadProgress(wxThreadEvent& ev)
{
    if (m_state != State::Downloading || m_cancel->load()) return;
    const auto p = ev.GetPayload<std::pair<std::uint64_t, std::uint64_t>>();
    const std::uint64_t done = p.first, total = p.second;

    wxString label = wxString::FromUTF8("Downloading v" + m_info.latest + "...  ");
    if (total > 0) {
        const int pct = static_cast<int>((done * 100) / total);
        m_gauge->SetValue(pct);
        label << pct << "%  (" << Mb(done) << " of " << Mb(total) << ")";
    } else {
        m_gauge->Pulse();
        label << Mb(done);
    }
    m_status->SetLabel(label);
}

void LbAboutDialog::OnDownloadResult(wxThreadEvent& ev)
{
    const auto r = ev.GetPayload<UpdateInstaller::DownloadResult>();
    m_secondary->Enable(true);

    // The worker's result can already be queued when Cancel is clicked,
    // so the dialog's own flag is authoritative: a verified installer that
    // arrives after Cancel is discarded, never installed.
    if (r.cancelled || m_cancel->load(std::memory_order_acquire)) {
        if (r.ok && !r.path.empty()) {
            wxLogNull quiet;
            wxRemoveFile(wxString(r.path));
        }
        SetState(State::Available, "Download cancelled.");
        return;
    }
    if (!r.ok) {
        SetState(State::Available, wxString::FromUTF8("Download failed: " + r.error));
        return;
    }

    // Verified.  Hand off to the frame, which checks nothing is running,
    // closes every window (saving chats), then starts the installer.
    if (m_cancel->load(std::memory_order_acquire)) {   // last check before handoff
        SetState(State::Available, "Download cancelled.");
        return;
    }
    m_installerPath = r.path;
    m_status->SetLabel("Installing... LlamaBoss will close and reopen.");
    EndModal(ID_INSTALL);
}

void LbAboutDialog::OpenDownloadPage()
{
    std::string target = m_info.url.empty() ? LbUpdateFallbackUrl() : m_info.url;
    if (!LbIsTrustedUpdateUrl(target))
        target = LbUpdateFallbackUrl();
    wxLaunchDefaultBrowser(wxString::FromUTF8(target));
}
