// chat_display_ctrl.h
// Custom display control with fast drag-scroll, browser-speed wheel
// scrolling, and Windows middle-click auto-scroll (pan mode).
//
// wxRichTextCtrl's built-in behaviors are tuned for an editor, not a
// reading surface:
//   - auto-scroll during drag-select is extremely slow -> timer-driven
//     edge scroll that scales with distance (original feature);
//   - the default wheel handler moves the system 3 lines per notch
//     against small scroll units, which reads as sluggish next to a
//     browser chat (Claude/ChatGPT) -> OnMouseWheel multiplies it and
//     consumes the event;
//   - there is no middle-click auto-scroll -> MSW-only pan mode with
//     the classic origin icon, both press-drag-release and sticky
//     click-then-move styles, matching Windows convention.
#pragma once

#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/caret.h>
#include <wx/popupwin.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <algorithm>
#include <cmath>

#ifdef __WXMSW__
// ── Pan-origin icon ─────────────────────────────────────────────
// Small popup shown at the middle-click anchor while pan mode is
// active: circle, up/down arrows, center dot — the standard Windows
// auto-scroll marker.  A popup window (rather than drawing into the
// richtext DC) survives the control's own repaints while scrolling.
class PanOriginIcon : public wxPopupWindow {
public:
    static constexpr int kSize = 34;

    explicit PanOriginIcon(wxWindow* parent)
        : wxPopupWindow(parent, wxBORDER_NONE)
    {
        SetSize(kSize, kSize);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &PanOriginIcon::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(*wxWHITE_BRUSH);
        dc.Clear();

        wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
        if (!gc) return;

        const double s   = kSize;
        const double mid = s / 2.0;
        const wxColour line(96, 96, 96);

        gc->SetPen(wxPen(line, 1));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawEllipse(1.5, 1.5, s - 3.0, s - 3.0);       // outer circle

        gc->SetBrush(wxBrush(line));
        gc->SetPen(*wxTRANSPARENT_PEN);

        // Up arrow
        wxGraphicsPath up = gc->CreatePath();
        up.MoveToPoint(mid, 5.0);
        up.AddLineToPoint(mid - 4.5, 11.0);
        up.AddLineToPoint(mid + 4.5, 11.0);
        up.CloseSubpath();
        gc->FillPath(up);

        // Down arrow
        wxGraphicsPath down = gc->CreatePath();
        down.MoveToPoint(mid, s - 5.0);
        down.AddLineToPoint(mid - 4.5, s - 11.0);
        down.AddLineToPoint(mid + 4.5, s - 11.0);
        down.CloseSubpath();
        gc->FillPath(down);

        // Center dot (hollow, like the native marker)
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(wxPen(line, 1));
        gc->DrawEllipse(mid - 2.5, mid - 2.5, 5.0, 5.0);

        delete gc;
    }
};
#endif // __WXMSW__

class ChatDisplayCtrl : public wxRichTextCtrl {
public:
    // Explicit, distinct timer ids.  wxTimer(owner) without an id can
    // report wxID_ANY from GetId(), and a Bind filtered on wxID_ANY
    // matches EVERY timer event -- with two timers on this control the
    // pan timer would then also fire the drag-select synthesizer.
    enum {
        kAutoScrollTimerId = wxID_HIGHEST + 101,
        kPanTimerId        = wxID_HIGHEST + 102
    };

    ChatDisplayCtrl(wxWindow* parent, wxWindowID id,
        const wxString& value = wxEmptyString,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = 0)
        : wxRichTextCtrl(parent, id, value, pos, size, style)
        , m_autoScrollTimer(this, kAutoScrollTimerId)
        , m_scrollDirection(0)
        , m_scrollIntensity(0)
        , m_inAutoScroll(false)
        , m_panTimer(this, kPanTimerId)
    {
        Bind(wxEVT_SET_FOCUS, &ChatDisplayCtrl::OnFocusGained, this);
        Bind(wxEVT_LEFT_DOWN, &ChatDisplayCtrl::OnMouseDown, this);
        Bind(wxEVT_MOTION, &ChatDisplayCtrl::OnDragMotion, this);
        Bind(wxEVT_LEFT_UP, &ChatDisplayCtrl::OnDragEnd, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &ChatDisplayCtrl::OnCaptureLost, this);
        Bind(wxEVT_TIMER, &ChatDisplayCtrl::OnAutoScrollTimer, this,
            m_autoScrollTimer.GetId());

        // Reading-speed wheel: consume the event and scroll a multiple
        // of the system line count so the transcript reads like a
        // browser chat instead of an editor.
        Bind(wxEVT_MOUSEWHEEL, &ChatDisplayCtrl::OnMouseWheel, this);

#ifdef __WXMSW__
        // Middle-click auto-scroll is a Windows convention; keep the
        // feature MSW-only so the control matches native expectations
        // per platform.
        Bind(wxEVT_MIDDLE_DOWN, &ChatDisplayCtrl::OnMiddleDown, this);
        Bind(wxEVT_MIDDLE_UP, &ChatDisplayCtrl::OnMiddleUp, this);
        Bind(wxEVT_KEY_DOWN, &ChatDisplayCtrl::OnKeyDownPan, this);
        Bind(wxEVT_TIMER, &ChatDisplayCtrl::OnPanTimer, this,
            m_panTimer.GetId());
#endif

        // The chat transcript should behave like a read-only document, not an
        // editor.  Keep selection/clicking, but do not show a blinking caret.
        CallAfter([this]() { SuppressCaret(); });
    }

    void SuppressCaret() {
        if (wxCaret* caret = GetCaret()) {
            caret->Hide();
        }
    }

private:
    // ── Tuning constants ────────────────────────────────────────
    // Wheel: 3x the system lines-per-notch.  With the default system
    // setting of 3 lines that is 9 scroll units per notch, which
    // matches the ~100px-per-notch feel of browser chat transcripts.
    static constexpr int kWheelSpeedMultiplier = 3;

    // Pan: pixels of dead zone around the anchor before scrolling
    // starts, and the divisor converting pixel distance to scroll
    // units per 30ms tick (smaller = faster).
    static constexpr int    kPanDeadZonePx = 12;
    static constexpr double kPanSpeedDivisor = 16.0;

    wxTimer m_autoScrollTimer;
    int m_scrollDirection;    // -1 = up, +1 = down, 0 = idle
    int m_scrollIntensity;    // lines per tick, scales with distance from edge
    bool m_inAutoScroll;      // guard against re-entry from synthetic events

    int m_wheelAccum = 0;     // sub-notch rotation (trackpads, free wheels)

    // Middle-click pan state (MSW).  Members exist on all platforms so
    // the class shape doesn't change per-build; only the bindings are
    // conditional.
    wxTimer m_panTimer;
    bool    m_panning     = false;
    bool    m_panCaptured = false;
    bool    m_panMoved    = false;   // left the dead zone while held?
    wxPoint m_panAnchor;             // client coords of middle-click
    double  m_panAccum    = 0.0;     // fractional scroll units carryover
#ifdef __WXMSW__
    PanOriginIcon* m_panIcon = nullptr;
#endif

    void SuppressCaretSoon() {
        CallAfter([this]() { SuppressCaret(); });
    }

    void OnFocusGained(wxFocusEvent& evt) {
        evt.Skip();
        SuppressCaretSoon();
    }

    void OnMouseDown(wxMouseEvent& evt) {
        // Any other click ends sticky pan mode (Windows behavior) and
        // swallows that click so it doesn't also move the selection.
        if (m_panning) { StopPan(); return; }
        evt.Skip();
        SuppressCaretSoon();
    }

    // ── Wheel speed ─────────────────────────────────────────────
    void OnMouseWheel(wxMouseEvent& evt) {
        if (m_panning) StopPan();     // wheel input cancels pan mode

        // Only own plain vertical scrolling.  Horizontal (shift/tilt)
        // and modified wheels keep default routing.
        if (evt.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL ||
            evt.ControlDown() || evt.ShiftDown()) {
            evt.Skip();
            return;
        }

        int delta = evt.GetWheelDelta();
        if (delta <= 0) delta = 120;

        // Accumulate sub-notch rotation so precision trackpads and
        // free-spinning wheels stay smooth instead of quantizing to
        // whole notches.
        m_wheelAccum += evt.GetWheelRotation();
        const int notches = m_wheelAccum / delta;
        if (notches == 0) return;     // not a full notch yet
        m_wheelAccum -= notches * delta;

        int lines = evt.GetLinesPerAction();
        if (lines <= 0) lines = 3;

        ScrollLines(-notches * lines * kWheelSpeedMultiplier);
        // Deliberately no evt.Skip(): the base handler would scroll a
        // second, slower time.
    }

#ifdef __WXMSW__
    // ── Middle-click auto-scroll (pan mode) ─────────────────────
    void OnMiddleDown(wxMouseEvent& evt) {
        if (m_panning) { StopPan(); return; }  // second click toggles off
        StartPan(evt.GetPosition());
    }

    void OnMiddleUp(wxMouseEvent&) {
        // Press-drag-release style: if the user moved out of the dead
        // zone while holding, releasing ends the pan.  A click-in-place
        // release keeps sticky mode running until the next click, wheel,
        // or key press.
        if (m_panning && m_panMoved) StopPan();
    }

    void OnKeyDownPan(wxKeyEvent& evt) {
        if (m_panning) { StopPan(); return; }
        evt.Skip();
    }

    void StartPan(const wxPoint& clientPos) {
        m_panning  = true;
        m_panMoved = false;
        m_panAnchor = clientPos;
        m_panAccum  = 0.0;

        if (!m_panIcon) m_panIcon = new PanOriginIcon(this);
        const wxPoint screen = ClientToScreen(clientPos);
        m_panIcon->Position(
            screen - wxPoint(PanOriginIcon::kSize / 2,
                             PanOriginIcon::kSize / 2),
            wxSize(0, 0));
        m_panIcon->Show();

        SetCursor(wxCursor(wxCURSOR_SIZENS));

        if (!HasCapture()) {
            CaptureMouse();
            m_panCaptured = true;
        }
        m_panTimer.Start(30);
    }

    void StopPan() {
        if (!m_panning) return;
        m_panning = false;
        m_panTimer.Stop();
        if (m_panCaptured && HasCapture()) ReleaseMouse();
        m_panCaptured = false;
        if (m_panIcon) m_panIcon->Hide();
        SetCursor(wxNullCursor);
        m_panAccum = 0.0;
    }

    void OnPanTimer(wxTimerEvent&) {
        if (!m_panning) return;

        const wxPoint p = ScreenToClient(wxGetMousePosition());
        int dy = p.y - m_panAnchor.y;

        if (std::abs(dy) > kPanDeadZonePx) m_panMoved = true;

        if (dy > kPanDeadZonePx)       dy -= kPanDeadZonePx;
        else if (dy < -kPanDeadZonePx) dy += kPanDeadZonePx;
        else                           dy = 0;

        if (dy == 0) return;

        // Fractional accumulation keeps slow drags smooth (a 1px
        // offset still crawls) while large offsets scroll fast.
        m_panAccum += static_cast<double>(dy) / kPanSpeedDivisor;
        const int units = static_cast<int>(m_panAccum);
        if (units != 0) {
            m_panAccum -= units;
            ScrollLines(units);
        }
    }
#endif // __WXMSW__

    void OnDragMotion(wxMouseEvent& evt) {
        evt.Skip();  // always let base class handle selection
        if (m_inAutoScroll) return;

        if (!evt.Dragging() || !evt.LeftIsDown()) {
            StopAutoScroll();
            return;
        }

        int y = evt.GetPosition().y;
        int h = GetClientSize().y;

        if (y < 0) {
            m_scrollDirection = -1;
            m_scrollIntensity = std::min(std::max((-y) / 15 + 1, 1), 12);
            if (!m_autoScrollTimer.IsRunning())
                m_autoScrollTimer.Start(30);
        }
        else if (y > h) {
            m_scrollDirection = 1;
            m_scrollIntensity = std::min(std::max((y - h) / 15 + 1, 1), 12);
            if (!m_autoScrollTimer.IsRunning())
                m_autoScrollTimer.Start(30);
        }
        else {
            StopAutoScroll();
        }
    }

    void OnDragEnd(wxMouseEvent& evt) {
        StopAutoScroll();
        evt.Skip();
        SuppressCaretSoon();
    }

    void OnCaptureLost(wxMouseCaptureLostEvent&) {
        StopAutoScroll();
#ifdef __WXMSW__
        // Losing capture (alt-tab, popup steal) must not leave a
        // ghost pan running with a stranded origin icon.
        if (m_panning) {
            m_panCaptured = false;   // capture is already gone
            StopPan();
        }
#endif
    }

    void OnAutoScrollTimer(wxTimerEvent& evt) {
        // Two timers share wxEVT_TIMER on this handler; route by id so
        // the pan timer never triggers a synthetic drag-select event.
        if (evt.GetId() != m_autoScrollTimer.GetId()) { evt.Skip(); return; }
        if (m_scrollDirection == 0) return;

        ScrollLines(m_scrollDirection * m_scrollIntensity);

        // Synthesize a mouse-move at the visible edge so the base class
        // extends the selection to match the new scroll position.
        m_inAutoScroll = true;
        wxMouseEvent fake(wxEVT_MOTION);
        fake.SetLeftDown(true);
        fake.SetX(GetClientSize().x / 2);
        fake.SetY(m_scrollDirection < 0 ? 0 : GetClientSize().y - 1);
        fake.SetEventObject(this);
        HandleWindowEvent(fake);
        m_inAutoScroll = false;
        SuppressCaretSoon();
    }

    void StopAutoScroll() {
        if (m_autoScrollTimer.IsRunning())
            m_autoScrollTimer.Stop();
        m_scrollDirection = 0;
        m_scrollIntensity = 0;
    }
};
