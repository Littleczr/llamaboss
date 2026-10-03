// activity_strip.cpp — see activity_strip.h
#include "activity_strip.h"
#include "theme.h"

#include <wx/sizer.h>
#include <wx/dcclient.h>
#if defined(__WXMSW__) && wxUSE_TASKBARBUTTON
#include <wx/taskbarbutton.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace {

std::string FormatClock(long long seconds)
{
    if (seconds < 0) seconds = 0;
    const long long h = seconds / 3600;
    const long long m = (seconds % 3600) / 60;
    const long long s = seconds % 60;
    std::ostringstream out;
    if (h > 0) {
        out << h << ':';
        if (m < 10) out << '0';
    }
    out << m << ':';
    if (s < 10) out << '0';
    out << s;
    return out.str();
}

// Finds the LAST "NN%" or "NN.N%" token in |line| and returns 0..100, or
// -1 if none.  Last rather than first because pip prints "|####| 42%"
// and ffmpeg prints multiple numeric fields before its percent.
int ParsePercent(const std::string& line)
{
    int best = -1;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '%') continue;
        size_t j = i;
        while (j > 0 && (std::isdigit(static_cast<unsigned char>(line[j - 1])) ||
                         line[j - 1] == '.')) {
            --j;
        }
        if (j == i) continue;
        const double v = std::atof(line.substr(j, i - j).c_str());
        if (v >= 0.0 && v <= 100.0) best = static_cast<int>(v + 0.5);
    }
    return best;
}

// Squeeze a line to roughly one row of the strip.  Progress lines are
// short already; this only guards against a build tool spewing a 400-char
// compiler invocation.
//
// The cut must land on a UTF-8 codepoint boundary.  Render() hands the
// result to wxString::FromUTF8, which VALIDATES and returns an empty
// string on a partial sequence — and since title, clock and live line
// share one label string, a byte-offset cut through an accented
// character (yt-dlp titles are the usual source) blanked the whole strip
// for the rest of the task.
std::string Squeeze(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    size_t start = s.size() - (maxBytes - 1);
    // Skip forward over continuation bytes (10xxxxxx) so the tail starts
    // on a lead byte.  At most 3 steps for valid UTF-8.
    while (start < s.size() &&
           (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) {
        ++start;
    }
    return "\xE2\x80\xA6" + s.substr(start);
}

} // namespace

ActivityStrip::ActivityStrip(wxFrame* frame, wxWindow* parent,
                             const ThemeData& theme)
    : m_frame(frame)
    , m_ticker(this)
{
    m_panel = new wxPanel(parent, wxID_ANY);

    auto* row = new wxBoxSizer(wxHORIZONTAL);

    m_gauge = new wxGauge(m_panel, wxID_ANY, 100,
                          wxDefaultPosition, wxSize(parent->FromDIP(110),
                                                    parent->FromDIP(10)),
                          wxGA_HORIZONTAL | wxGA_SMOOTH);
    row->Add(m_gauge, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(12));

    m_label = new wxStaticText(m_panel, wxID_ANY, "",
                               wxDefaultPosition, wxDefaultSize,
                               wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    row->Add(m_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT,
             parent->FromDIP(10));

    m_panel->SetSizer(row);
    m_panel->SetMinSize(wxSize(-1, parent->FromDIP(24)));
    ApplyTheme(theme);
    m_panel->Hide();
}

ActivityStrip::~ActivityStrip()
{
    m_ticker.Stop();
}

void ActivityStrip::ApplyTheme(const ThemeData& theme)
{
    if (!m_panel) return;
    m_panel->SetBackgroundColour(theme.bgMain);
    m_label->SetBackgroundColour(theme.bgMain);
    m_label->SetForegroundColour(theme.textMuted);

    wxFont f = m_label->GetFont();
    f.SetFamily(wxFONTFAMILY_TELETYPE);
    f.SetPointSize(std::max(8, f.GetPointSize() - 1));
    m_label->SetFont(f);
    m_panel->Refresh();
}

void ActivityStrip::Begin(const std::string& title, int limitSec,
                          bool countdown)
{
    m_active    = true;
    m_title     = title;
    m_limitSec  = limitSec;
    m_countdown = countdown;
    m_percent   = -1;
    m_liveLine.clear();
    m_startedAt = std::chrono::steady_clock::now();

    m_panel->Show();
    if (auto* parent = m_panel->GetParent()) parent->Layout();

    Render();
    m_ticker.Start(1000);
}

void ActivityStrip::SetLiveLine(const std::string& line)
{
    if (!m_active) return;
    m_liveLine = line;
    const int p = ParsePercent(line);
    if (p >= 0) m_percent = p;
    Render();
}

void ActivityStrip::End()
{
    if (!m_active) return;
    // Read before m_active flips: ElapsedSec() reports 0 once inactive.
    const double ranForSec = ElapsedSec();
    m_ticker.Stop();
    m_active = false;

    m_panel->Hide();
    if (auto* parent = m_panel->GetParent()) parent->Layout();

    // Taskbar: clear progress; nudge if the user is elsewhere.  A 15-minute
    // download finishing while the window is buried is exactly the moment
    // this matters.  Only for tasks that actually ran a while: Begin/End
    // bracket EVERY pending async tool, so without the threshold an agent
    // loop of fifteen quick PowerShell calls flashed the taskbar fifteen
    // times while the user was in another window.
#if defined(__WXMSW__) && wxUSE_TASKBARBUTTON
    if (m_frame) {
        if (auto* tb = m_frame->MSWGetTaskBarButton())
            tb->SetProgressState(wxTASKBAR_BUTTON_NO_PROGRESS);
    }
#endif
    if (m_frame && !m_frame->IsActive() && ranForSec >= kAttentionAfterSec)
        m_frame->RequestUserAttention(wxUSER_ATTENTION_INFO);
}

double ActivityStrip::ElapsedSec() const
{
    if (!m_active) return 0.0;
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_startedAt).count();
}

void ActivityStrip::OnTick()
{
    if (!m_active) return;
    Render();
}

void ActivityStrip::Render()
{
    if (!m_active || !m_label) return;

    const long long elapsed = static_cast<long long>(ElapsedSec());

    // ── Gauge ────────────────────────────────────────────────────
    int percent = m_percent;
    if (percent < 0 && m_countdown && m_limitSec > 0)
        percent = static_cast<int>(std::min<long long>(100, elapsed * 100 / m_limitSec));

    if (percent >= 0) {
        m_gauge->SetValue(std::max(0, std::min(100, percent)));
    } else {
        m_gauge->Pulse();
    }

    // ── Label ────────────────────────────────────────────────────
    std::ostringstream line;
    line << m_title << "  \xC2\xB7  " << FormatClock(elapsed);
    if (m_limitSec > 0) line << " / " << FormatClock(m_limitSec);

    if (!m_liveLine.empty()) {
        line << "  \xC2\xB7  " << Squeeze(m_liveLine, 140);
    } else if (!m_countdown) {
        line << "  \xC2\xB7  Stop cancels";
    }

    // Runway warning for a hard timeout: the executor will kill the
    // process tree at the limit, so say so in the last minute.
    if (!m_countdown && m_limitSec > 0 && (m_limitSec - elapsed) <= 60 &&
        (m_limitSec - elapsed) > 0) {
        line << "  \xC2\xB7  \xE2\x9A\xA0 timeout in "
             << FormatClock(m_limitSec - elapsed);
    }

    m_label->SetLabel(wxString::FromUTF8(line.str()));
    UpdateTaskbar();
}

void ActivityStrip::UpdateTaskbar()
{
#if defined(__WXMSW__) && wxUSE_TASKBARBUTTON
    if (!m_frame) return;
    auto* tb = m_frame->MSWGetTaskBarButton();
    if (!tb) return;

    int percent = m_percent;
    if (percent < 0 && m_countdown && m_limitSec > 0)
        percent = static_cast<int>(std::min<long long>(
            100, static_cast<long long>(ElapsedSec()) * 100 / m_limitSec));

    if (percent >= 0) {
        tb->SetProgressRange(100);
        tb->SetProgressValue(std::max(0, std::min(100, percent)));
        tb->SetProgressState(wxTASKBAR_BUTTON_NORMAL);
    } else {
        tb->SetProgressState(wxTASKBAR_BUTTON_INDETERMINATE);
    }
#endif
}
