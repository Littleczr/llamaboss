// chat_display_ctrl.h
// Custom display control with fast drag-scroll.
// wxRichTextCtrl's built-in auto-scroll during drag-select is extremely
// slow.  This subclass uses a timer to scroll faster when the mouse is
// dragged above or below the visible area, scaling speed with distance.
#pragma once

#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <algorithm>

class ChatDisplayCtrl : public wxRichTextCtrl {
public:
    ChatDisplayCtrl(wxWindow* parent, wxWindowID id,
        const wxString& value = wxEmptyString,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = 0)
        : wxRichTextCtrl(parent, id, value, pos, size, style)
        , m_autoScrollTimer(this)
        , m_scrollDirection(0)
        , m_scrollIntensity(0)
        , m_inAutoScroll(false)
    {
        Bind(wxEVT_MOTION, &ChatDisplayCtrl::OnDragMotion, this);
        Bind(wxEVT_LEFT_UP, &ChatDisplayCtrl::OnDragEnd, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &ChatDisplayCtrl::OnCaptureLost, this);
        Bind(wxEVT_TIMER, &ChatDisplayCtrl::OnAutoScrollTimer, this,
            m_autoScrollTimer.GetId());
    }

private:
    wxTimer m_autoScrollTimer;
    int m_scrollDirection;    // -1 = up, +1 = down, 0 = idle
    int m_scrollIntensity;    // lines per tick, scales with distance from edge
    bool m_inAutoScroll;      // guard against re-entry from synthetic events

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
    }

    void OnCaptureLost(wxMouseCaptureLostEvent&) {
        StopAutoScroll();
    }

    void OnAutoScrollTimer(wxTimerEvent&) {
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
    }

    void StopAutoScroll() {
        if (m_autoScrollTimer.IsRunning())
            m_autoScrollTimer.Stop();
        m_scrollDirection = 0;
        m_scrollIntensity = 0;
    }
};
