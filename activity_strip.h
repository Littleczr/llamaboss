// activity_strip.h
//
// Single-row "something is running" strip that sits between the chat
// transcript and the composer.  It replaces the old 1 Hz rewrite of a
// progress line *inside* the wxRichTextCtrl (which re-laid-out the tail
// paragraph every second for up to 30 minutes and fought the user's
// scroll position).  A wxStaticText updated once a second costs nothing
// and never touches the transcript.
//
// What it shows, left to right:
//
//   [gauge]  Running powershell  ·  4:32 / 30:00  ·  [download]  42.3% of 120MiB at 2.1MiB/s ETA 00:35
//
// - The gauge is determinate when the live line contains a "NN%" token
//   (yt-dlp, pip, curl, ffmpeg progress), indeterminate (pulsing) otherwise.
// - The middle segment is elapsed / limit.  For the `wait` tool the limit
//   is the requested wait; for PowerShell it is timeout_seconds.
// - The right segment is the most recent output line from the worker
//   (wxEVT_CMD_OUTPUT), trimmed to fit.
//
// On MSW the same percentage is mirrored to the taskbar button so a user
// who alt-tabbed away for a 15-minute download can see it from the
// taskbar.  When the task ends while the frame is not the foreground
// window, the taskbar flashes (RequestUserAttention) so the completion
// is noticed.
//
// Main-thread only.  Owned by MyFrame.
//
#pragma once

#include <wx/wx.h>
#include <wx/gauge.h>
#include <wx/timer.h>

#include <chrono>
#include <string>

struct ThemeData;

class ActivityStrip
{
public:
    ActivityStrip(wxFrame* frame, wxWindow* parent, const ThemeData& theme);
    ~ActivityStrip();

    wxPanel* GetPanel() const { return m_panel; }

    // Show the strip and start the 1 Hz clock.
    //   title      — e.g. "Running powershell", "Waiting"
    //   limitSec   — 0 for none; otherwise rendered as "elapsed / limit"
    //   countdown  — true for the wait tool (progress = elapsed/limit and
    //                the gauge is determinate from the start)
    void Begin(const std::string& title, int limitSec, bool countdown);

    // Latest output line from the running command.  Parses an optional
    // "NN%" / "NN.N%" token to drive the determinate gauge.
    void SetLiveLine(const std::string& line);

    // Hide the strip, stop the clock, clear taskbar progress, and flash
    // the taskbar if the frame isn't active.
    void End();

    bool   IsActive()   const { return m_active; }
    double ElapsedSec() const;

    // Minimum run time before End() flashes the taskbar when the frame is
    // not active.  Short tool calls in an agent loop must not nag.
    static constexpr double kAttentionAfterSec = 20.0;

    void ApplyTheme(const ThemeData& theme);

private:
    class Ticker : public wxTimer {
    public:
        explicit Ticker(ActivityStrip* owner) : m_owner(owner) {}
        void Notify() override { if (m_owner) m_owner->OnTick(); }
    private:
        ActivityStrip* m_owner;
    };

    void OnTick();
    void Render();
    void UpdateTaskbar();

    wxFrame*      m_frame  = nullptr;
    wxPanel*      m_panel  = nullptr;
    wxGauge*      m_gauge  = nullptr;
    wxStaticText* m_label  = nullptr;
    Ticker        m_ticker;

    bool        m_active     = false;
    bool        m_countdown  = false;
    int         m_limitSec   = 0;
    int         m_percent    = -1;     // -1 = unknown → indeterminate
    std::string m_title;
    std::string m_liveLine;
    std::chrono::steady_clock::time_point m_startedAt;
};
