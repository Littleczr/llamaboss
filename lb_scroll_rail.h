// lb_scroll_rail.h
// Slim, theme-coloured scroll rail that replaces a native vertical scrollbar.
//
// How it works:
//   * The real scrolling window (a wxScrolledWindow, or the chat transcript's
//     wxRichTextCtrl -- anything driven by wxScrollHelper) lives inside a
//     plain "clip" panel.  When the
//     content is taller than the viewport, the list is made wider than the
//     clip by exactly the native scrollbar width, so the native bar is pushed
//     outside the visible area (child windows are clipped to their parent).
//     Native scrolling, the mouse wheel, keyboard navigation and every
//     Scroll()/GetViewStart() call keep working unchanged.
//   * This rail sits beside the clip and draws a thin rounded thumb that
//     mirrors the list's scroll position.  It can be dragged, clicked above or
//     below the thumb to page, and wheel events over it go to the list.
//   * The thumb only appears while the mouse is over the list or rail, while
//     dragging, or briefly after a scroll, like Windows 11 overlay scrollbars.
//
// State is re-synced on idle (cheap getters and a rect compare), so the rail
// follows programmatic scrolls and list rebuilds without call-site hooks.
//
// Native text mode (the composer): a multiline wxTextCtrl is a native EDIT
// control that keeps its scroll state in Win32, not in wxScrollHelper.  The
// wxTextCtrl constructor below reads the position from
// GetScrollInfo(SB_VERT) and scrolls with EM_LINESCROLL; the clip trick is
// the same, using the control's non-client width as the bar width.
// LbFollowClip is the clip for that case: it reports the child's min height
// to the sizer, so the composer's SetMinSize() auto-grow and drag resize
// keep working unchanged.
#pragma once

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#include <algorithm>
#include <functional>
#include "theme.h"

class LbScrollRail : public wxPanel
{
public:
    // `list` is any window that scrolls through wxScrollHelper: the
    // sidebar's wxScrolledWindow, or the chat transcript (wxRichTextCtrl).
    LbScrollRail(wxWindow* parent, wxWindow* clip, wxScrolledWindow* list,
                 std::function<const ThemeData&()> theme)
        : LbScrollRail(parent, clip, list, list, std::move(theme)) {}

    // Native multiline text control (the composer).  See header comment.
    LbScrollRail(wxWindow* parent, wxWindow* clip, wxTextCtrl* text,
                 std::function<const ThemeData&()> theme)
        : LbScrollRail(parent, clip, text, nullptr, std::move(theme))
    {
        m_text = text;
    }

    LbScrollRail(wxWindow* parent, wxWindow* clip, wxWindow* list,
                 wxScrollHelper* scroller,
                 std::function<const ThemeData&()> theme)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                  wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
        , m_clip(clip), m_list(list), m_scroller(scroller), m_theme(std::move(theme))
        , m_timer(this)
    {
        SetMinSize(wxSize(FromDIP(kRailWidth), -1));
        SetBackgroundStyle(wxBG_STYLE_PAINT);

        Bind(wxEVT_PAINT, &LbScrollRail::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &LbScrollRail::OnLeftDown, this);
        Bind(wxEVT_LEFT_DCLICK, &LbScrollRail::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &LbScrollRail::OnLeftUp, this);
        Bind(wxEVT_MOTION, &LbScrollRail::OnMotion, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            EndDrag();
        });
        Bind(wxEVT_MOUSEWHEEL, &LbScrollRail::OnWheel, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            if (!m_dragging) { m_hot = false; Sync(); }
            e.Skip();
        });
        Bind(wxEVT_IDLE, [this](wxIdleEvent& e) { Sync(); e.Skip(); });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Sync(); }, m_timer.GetId());

        if (m_clip) {
            m_clip->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
                LayoutList();
                e.Skip();
            });
        }
    }

    ~LbScrollRail() override
    {
        m_timer.Stop();
    }

    // Call after a theme change.
    void ApplyTheme()
    {
        if (m_clip) m_clip->SetBackgroundColour(Background(m_theme()));
        Refresh();
    }

    // Which ThemeData colour the rail and clip sit on (default: sidebar).
    void SetBackgroundSlot(wxColour ThemeData::* slot)
    {
        m_bgSlot = slot;
        ApplyTheme();
    }

    // Forward wheel events over the rail to this window instead of applying
    // the rail's own step (the chat view has its own faster wheel handling).
    void SetWheelTarget(wxWindow* target) { m_wheelTarget = target; }

    // Called after the rail itself scrolls the list (drag, track click,
    // own wheel step), so the owner can update follow-the-output state.
    void SetScrolledCallback(std::function<void()> cb) { m_onScrolled = std::move(cb); }

private:
    static constexpr int kRailWidth   = 8;    // DIP, total column width
    static constexpr int kThumbIdle   = 4;    // DIP
    static constexpr int kThumbHot    = 6;    // DIP
    static constexpr int kPad         = 4;    // DIP, top/bottom track inset
    static constexpr int kMinThumb    = 28;   // DIP
    static constexpr long kLingerMs   = 900;  // thumb stays after a scroll

    struct Metrics {
        int  contentH = 0;   // full list height (px)
        int  clientH  = 0;   // visible height (px)
        int  scrollY  = 0;   // current offset (px)
        int  unitY    = 1;   // pixels per scroll unit
        bool needed() const { return contentH > clientH && clientH > 0; }
        int  maxScroll() const { return std::max(1, contentH - clientH); }
    };

    wxWindow*         m_clip;
    wxWindow*         m_list;
    wxScrollHelper*   m_scroller;
    wxTextCtrl*       m_text = nullptr;   // native text mode when set
    wxColour ThemeData::* m_bgSlot = &ThemeData::bgSidebar;
    wxWindow*         m_wheelTarget = nullptr;
    std::function<void()> m_onScrolled;

    wxColour Background(const ThemeData& t) const { return t.*m_bgSlot; }
    std::function<const ThemeData&()> m_theme;
    wxTimer           m_timer;

    Metrics   m_last;
    bool      m_visible  = false;
    bool      m_hot      = false;   // mouse over the thumb
    bool      m_dragging = false;
    int       m_dragOffset = 0;
    int       m_wheelAccum = 0;
    wxLongLong m_lastScrollAt = 0;

    // ── Geometry ────────────────────────────────────────────────

    Metrics Measure() const
    {
        Metrics m;
        if (m_text) return MeasureText();
        if (!m_list || !m_scroller) return m;
        int ux = 0, uy = 0;
        m_scroller->GetScrollPixelsPerUnit(&ux, &uy);
        m.unitY = std::max(1, uy);
        int vx = 0, vy = 0;
        m_scroller->GetViewStart(&vx, &vy);
        m.scrollY  = vy * m.unitY;
        m.contentH = m_list->GetVirtualSize().GetHeight();
        m.clientH  = m_list->GetClientSize().GetHeight();
        return m;
    }

    // Native EDIT control: Win32 scroll info is in lines; convert to pixels
    // with the line height so the shared thumb maths stays unchanged.
    Metrics MeasureText() const
    {
        Metrics m;
        if (!m_text) return m;
        m.unitY = std::max(1, m_text->GetCharHeight());
#ifdef __WXMSW__
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        if (!::GetScrollInfo(static_cast<HWND>(m_text->GetHWND()),
                             SB_VERT, &si))
            return m;
        const int lines = si.nMax - si.nMin + 1;
        const int page  = static_cast<int>(si.nPage);
        if (page <= 0 || lines <= page) return m;   // nothing to scroll
        m.contentH = lines * m.unitY;
        m.clientH  = page  * m.unitY;
        m.scrollY  = (si.nPos - si.nMin) * m.unitY;
#endif
        return m;
    }

    void ScrollTextLines(int delta)
    {
        if (!m_text || delta == 0) return;
#ifdef __WXMSW__
        ::SendMessage(static_cast<HWND>(m_text->GetHWND()),
                      EM_LINESCROLL, 0, static_cast<LPARAM>(delta));
#else
        m_text->ScrollLines(delta);
#endif
    }

    wxRect ThumbRect(const Metrics& m) const
    {
        const wxSize sz = GetClientSize();
        const int pad    = FromDIP(kPad);
        const int trackH = std::max(0, sz.y - 2 * pad);
        int thumbH = static_cast<int>(
            static_cast<long long>(trackH) * m.clientH / std::max(1, m.contentH));
        thumbH = std::clamp(thumbH, std::min(FromDIP(kMinThumb), trackH), trackH);
        const int travel = trackH - thumbH;
        const int y = pad + static_cast<int>(
            static_cast<long long>(travel) *
            std::clamp(m.scrollY, 0, m.maxScroll()) / m.maxScroll());
        const int w = FromDIP((m_hot || m_dragging) ? kThumbHot : kThumbIdle);
        return wxRect((sz.x - w) / 2, y, w, thumbH);
    }

    // Push the native scrollbar outside the clip when it would be shown,
    // and give rows the full clip width when it would not.
    void LayoutList()
    {
        if (!m_clip || !m_list) return;
        const wxSize clip = m_clip->GetClientSize();
        if (m_text) {
            // The EDIT control shows its own bar in the non-client area;
            // push exactly that width outside the clip, bar or no bar.
            const int ncw = std::max(0,
                m_list->GetSize().x - m_list->GetClientSize().x);
            const wxRect want(0, 0, clip.x + ncw, clip.y);
            if (m_list->GetRect() != want)
                m_list->SetSize(want);
            return;
        }
        const bool needsBar = m_list->GetVirtualSize().GetHeight() > clip.y;
        const int sbw = needsBar
            ? wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, m_list) : 0;
        const wxRect want(0, 0, clip.x + sbw, clip.y);
        if (m_list->GetRect() != want)
            m_list->SetSize(want);
    }

    bool MouseOverArea() const
    {
        const wxPoint p = wxGetMousePosition();
        return (m_clip && m_clip->IsShownOnScreen() &&
                m_clip->GetScreenRect().Contains(p)) ||
               GetScreenRect().Contains(p);
    }

    // ── State sync ──────────────────────────────────────────────

    void Sync()
    {
        if (!m_list || !IsShownOnScreen()) return;
        LayoutList();

        const Metrics m = Measure();
        const bool moved = (m.scrollY != m_last.scrollY);
        if (moved)
            m_lastScrollAt = wxGetLocalTimeMillis();

        // Compare as 64-bit: m_lastScrollAt starts at 0, so the first
        // difference is the whole clock value and would overflow a long
        // (wxLongLong::ToLong asserts on that in debug builds).
        const bool recent =
            (wxGetLocalTimeMillis() - m_lastScrollAt) < wxLongLong(kLingerMs);
        const bool visible =
            m.needed() && (m_dragging || recent || MouseOverArea());

        if (moved || visible != m_visible ||
            m.contentH != m_last.contentH || m.clientH != m_last.clientH) {
            m_last = m;
            m_visible = visible;
            Refresh();
        }

        // Poll only while the thumb is up, so it can hide after the mouse
        // leaves the window entirely (no more idle events arrive then).
        if (m_visible && !m_timer.IsRunning())
            m_timer.Start(150);
        else if (!m_visible && m_timer.IsRunning())
            m_timer.Stop();
    }

    void ScrollToPixel(int px)
    {
        const Metrics m = Measure();
        px = std::clamp(px, 0, m.maxScroll());
        if (m_text) {
            ScrollTextLines((px + m.unitY / 2) / m.unitY - m.scrollY / m.unitY);
            if (m_onScrolled) m_onScrolled();
            Sync();
            return;
        }
        m_scroller->Scroll(-1, (px + m.unitY / 2) / m.unitY);
        if (m_onScrolled) m_onScrolled();
        Sync();
    }

    // ── Events ──────────────────────────────────────────────────

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const ThemeData& t = m_theme();
        dc.SetBackground(wxBrush(Background(t)));
        dc.Clear();
        if (!m_visible) return;

        const Metrics m = Measure();
        if (!m.needed()) return;

        auto mix = [](const wxColour& a, const wxColour& b, int pct) {
            auto ch = [pct](int x, int y) {
                return static_cast<unsigned char>(
                    (x * (100 - pct) + y * pct + 50) / 100);
            };
            return wxColour(ch(a.Red(), b.Red()), ch(a.Green(), b.Green()),
                            ch(a.Blue(), b.Blue()));
        };
        const wxColour fill = m_dragging
            ? LbInteractiveAccent(t)
            : mix(Background(t), t.textMuted, m_hot ? 80 : 50);

        const wxRect r = ThumbRect(m);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(fill));
        dc.DrawRoundedRectangle(r, r.width / 2.0);
    }

    void OnLeftDown(wxMouseEvent& e)
    {
        const Metrics m = Measure();
        if (!m.needed()) return;
        const wxRect thumb = ThumbRect(m);
        const int y = e.GetPosition().y;

        if (y >= thumb.GetTop() && y <= thumb.GetBottom()) {
            m_dragging = true;
            m_dragOffset = y - thumb.y;
            if (!HasCapture()) CaptureMouse();
            Refresh();
        }
        else {
            // Page toward the click, like a native track click.
            const int page = std::max(m.unitY, m.clientH - FromDIP(24));
            ScrollToPixel(m.scrollY + (y < thumb.y ? -page : page));
        }
    }

    void OnLeftUp(wxMouseEvent&)
    {
        EndDrag();
    }

    void EndDrag()
    {
        if (!m_dragging) return;
        m_dragging = false;
        if (HasCapture()) ReleaseMouse();
        m_hot = GetClientRect().Contains(ScreenToClient(wxGetMousePosition()));
        Refresh();
    }

    void OnMotion(wxMouseEvent& e)
    {
        const Metrics m = Measure();
        if (m_dragging) {
            const wxRect thumb = ThumbRect(m);
            const int pad    = FromDIP(kPad);
            const int travel = std::max(1,
                GetClientSize().y - 2 * pad - thumb.height);
            const int top = e.GetPosition().y - m_dragOffset - pad;
            ScrollToPixel(static_cast<int>(
                static_cast<long long>(top) * m.maxScroll() / travel));
            return;
        }
        // Widen the thumb anywhere over the rail column: an 8 px target is
        // easier to hit than the thumb alone.
        if (!m_hot) {
            m_hot = true;
            Refresh();
        }
        e.Skip();
    }

    void OnWheel(wxMouseEvent& e)
    {
        // Same step as wxScrolled's own wheel handling: one scroll unit per
        // "line", lines-per-action lines per notch.  Accumulate rotation so
        // high-resolution wheels and touchpads scroll smoothly.
        if (!m_list || e.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL) {
            e.Skip();
            return;
        }
        if (m_wheelTarget) {
            // Let the target's own wheel handler (speed, follow logic) run.
            wxMouseEvent copy(e);
            copy.SetEventObject(m_wheelTarget);
            copy.SetId(m_wheelTarget->GetId());
            m_wheelTarget->GetEventHandler()->ProcessEvent(copy);
            Sync();
            return;
        }
        const int delta = std::max(1, e.GetWheelDelta());
        m_wheelAccum += e.GetWheelRotation();
        const int notches = m_wheelAccum / delta;
        if (notches == 0) return;
        m_wheelAccum -= notches * delta;
        if (m_text) {
            ScrollTextLines(-notches * e.GetLinesPerAction());
            Sync();
            return;
        }
        const Metrics m = Measure();
        ScrollToPixel(m.scrollY - notches * e.GetLinesPerAction() * m.unitY);
    }
};

// Clip panel for LbScrollRail's native text mode.  The child is positioned
// by the rail (not a sizer), so this panel reports the child's min height to
// its own sizer: SetMinSize() on the child still drives the row height.  The
// width is deliberately small and fixed, so the child's pushed-out width
// never feeds back into the clip's best size.
class LbFollowClip : public wxPanel
{
public:
    explicit LbFollowClip(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                  wxBORDER_NONE | wxTAB_TRAVERSAL) {}

    void SetFollowed(wxWindow* child) { m_child = child; }

    wxSize GetMinSize() const override
    {
        int h = -1;
        if (m_child) {
            h = m_child->GetMinSize().y;
            if (h <= 0) h = m_child->GetBestSize().y;
        }
        return wxSize(FromDIP(40), h);
    }

protected:
    wxSize DoGetBestSize() const override { return GetMinSize(); }

private:
    wxWindow* m_child = nullptr;
};
