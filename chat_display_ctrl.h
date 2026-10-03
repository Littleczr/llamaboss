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
#include <functional>
#include <utility>
#include <vector>

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

        // Navigation keys are handled by wxRichTextCtrl after this event.
        // Observe them on every platform so Page Up/Down, Home/End, arrow
        // navigation, and Space can update transcript follow mode once the
        // base control has moved the viewport.
        Bind(wxEVT_KEY_DOWN, &ChatDisplayCtrl::OnKeyDown, this);

#ifdef __WXMSW__
        // Middle-click auto-scroll is a Windows convention; keep the
        // feature MSW-only so the control matches native expectations
        // per platform.
        Bind(wxEVT_MIDDLE_DOWN, &ChatDisplayCtrl::OnMiddleDown, this);
        Bind(wxEVT_MIDDLE_UP, &ChatDisplayCtrl::OnMiddleUp, this);
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

    // ChatDisplay installs this after construction. Direct scrolling done
    // inside this custom control does not consistently emit wxScrollWinEvent,
    // so follow mode cannot rely on the outer scrollbar bindings alone.
    // Notifications are deferred and coalesced so the callback observes the
    // final viewport after wxRichTextCtrl has processed the input event.
    void SetViewportChangedHandler(std::function<void()> handler) {
        m_viewportChangedHandler = std::move(handler);
    }

    // Right bubbles are the standard user-message presentation.
    void SetUserBubbleColor(const wxColour& bubble) {
        m_userBubble = bubble;
        m_userLayoutWidth = -1;
        GetBuffer().Invalidate(wxRICHTEXT_ALL);
        Refresh(false);
    }

    void MarkUserMessage(long start, long end, int naturalWidth) {
        if (end < start) return;
        const long group = ++m_userMessageSerial;
        wxRichTextParagraph* first = nullptr;
        wxRichTextParagraph* last = nullptr;
        for (auto node = GetBuffer().GetChildren().GetFirst(); node; node = node->GetNext()) {
            auto* paragraph = dynamic_cast<wxRichTextParagraph*>(node->GetData());
            if (!paragraph) continue;
            const auto range = paragraph->GetRange();
            if (range.GetEnd() < start) continue;
            if (range.GetStart() > end) break;
            auto& properties = paragraph->GetProperties();
            properties.SetProperty("lb_user_group", group);
            properties.SetProperty("lb_user_natural_width", static_cast<long>(naturalWidth));
            if (!first) first = paragraph;
            last = paragraph;
        }
        if (first) first->GetProperties().SetProperty("lb_user_first", true);
        if (last) last->GetProperties().SetProperty("lb_user_last", true);
        m_userLayoutWidth = -1;
        GetBuffer().Invalidate(wxRICHTEXT_ALL);
    }

    // Live transcript writes are one transaction. Nested renderer/thinking
    // updates share the outer snapshot; replay's existing freeze owns itself.
    void BeginTranscriptUpdate(bool follow) {
        if (m_transcriptUpdateDepth++ != 0) return;
        m_transcriptOwnsFreeze = !IsFrozen();
        if (!m_transcriptOwnsFreeze) return;
        m_transcriptFollow = follow;
        GetViewStart(&m_transcriptViewX, &m_transcriptViewY);
        int ppuX = 0;
        GetScrollPixelsPerUnit(&ppuX, &m_transcriptPpuY);
        GetSelection(&m_transcriptSelectionStart, &m_transcriptSelectionEnd);
        Freeze();
    }

    void EndTranscriptUpdate() {
        if (m_transcriptUpdateDepth <= 0 || --m_transcriptUpdateDepth != 0) return;
        if (!m_transcriptOwnsFreeze) return;
        m_restoreTranscriptView = true;
        Thaw(); // DoThaw below restores against the FINAL scrollbar range.
        m_restoreTranscriptView = false;
        m_transcriptOwnsFreeze = false;
        SuppressCaret();
    }

    bool IsTranscriptUpdateActive() const {
        return m_transcriptUpdateDepth > 0 || m_restoreTranscriptView;
    }

    void SetViewportChangingHandler(std::function<void()> handler) {
        m_viewportChangingHandler = std::move(handler);
    }

protected:
    // wxWidgets 3.3 changed layout APIs to the read-only DC interface.
    // Keep the exact virtual signature on both supported API families.
#if wxCHECK_VERSION(3, 3, 0)
    using LayoutDC = wxReadOnlyDC;
#else
    using LayoutDC = wxDC;
#endif
    void DoLayoutBuffer(wxRichTextBuffer& buffer, LayoutDC& dc,
                        wxRichTextDrawingContext& context, const wxRect& rect,
                        const wxRect& parentRect, int flags) override {
        if (m_userLayoutWidth != rect.width) {
            m_userLayoutWidth = rect.width;
            const int padding = FromDIP(14);
            const int available = std::max(1, rect.width - buffer.GetLeftMargin() - buffer.GetRightMargin());
            for (auto node = buffer.GetChildren().GetFirst(); node; node = node->GetNext()) {
                auto* paragraph = dynamic_cast<wxRichTextParagraph*>(node->GetData());
                if (!paragraph || !paragraph->GetProperties().HasProperty("lb_user_group")) continue;
                auto& properties = paragraph->GetProperties();
                auto& attr = paragraph->GetAttributes();
                const int natural = static_cast<int>(properties.GetPropertyLong("lb_user_natural_width"));
                // Short prompts fit their content; long prompts wrap at 78%
                // of the transcript width. On narrow windows use more room.
                const int cap = std::max(1, available < FromDIP(420)
                    ? available * 94 / 100 : available * 78 / 100);
                const int width = std::min(cap, std::max(FromDIP(180), natural + padding * 2));
                const int left = available - width;
                const int inset = std::min(padding, width / 4);
                attr.SetLeftIndent(paragraph->ConvertPixelsToTenthsMM(dc, left + inset), 0);
                attr.SetRightIndent(paragraph->ConvertPixelsToTenthsMM(dc, inset));
                attr.SetParagraphSpacingBefore(properties.HasProperty("lb_user_first")
                    ? paragraph->ConvertPixelsToTenthsMM(dc, padding) : 0);
                attr.SetParagraphSpacingAfter(properties.HasProperty("lb_user_last")
                    ? paragraph->ConvertPixelsToTenthsMM(dc, padding) : 0);
                properties.SetProperty("lb_user_left", static_cast<long>(left));
                properties.SetProperty("lb_user_width", static_cast<long>(width));
                for (auto child = paragraph->GetChildren().GetFirst(); child; child = child->GetNext()) {
                    if (auto* image = dynamic_cast<wxRichTextImage*>(child->GetData())) {
                        image->GetAttributes().GetTextBoxAttr().GetMaxSize().GetWidth().SetValue(
                            std::max(1, width - 2 * inset), wxTEXT_ATTR_UNITS_PIXELS);
                    }
                }
            }
            buffer.Invalidate(wxRICHTEXT_ALL);
        }
        wxRichTextCtrl::DoLayoutBuffer(buffer, dc, context, rect, parentRect, flags);
        // Geometry is final here. Rebuild even when the width is unchanged:
        // streaming, folding tool output, replay and removal can move cards.
        // Store values, never paragraph pointers that edits could invalidate.
        RebuildUserBubbleBounds(buffer);
    }

    void PaintBackground(wxDC& dc) override {
        wxRichTextCtrl::PaintBackground(dc);
        const wxRect visible(GetLogicalPoint(wxPoint(0, 0)), GetClientSize());
        if (visible.IsEmpty() || m_userBubbleBounds.empty()) return;

        // Prefix bottoms are monotonic even if an unusually tall card
        // overlaps later cards. Find the first possible intersection without
        // visiting the transcript or walking all preceding user messages.
        auto first = std::lower_bound(
            m_userBubbleBounds.begin(), m_userBubbleBounds.end(), visible.GetTop(),
            [](const UserBubbleBounds& bubble, int top) {
                return bubble.prefixBottom < top;
            });
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_userBubble));
        const int radius = FromDIP(12);
        for (auto it = first; it != m_userBubbleBounds.end(); ++it) {
            if (it->rect.GetTop() > visible.GetBottom()) break;
            if (it->rect.Intersects(visible))
                dc.DrawRoundedRectangle(it->rect, radius);
        }
    }

private:
    struct UserBubbleBounds {
        wxRect rect;       // Same scaled document coordinates as painting.
        int prefixBottom; // Maximum bottom among this and preceding cards.
    };
    std::vector<UserBubbleBounds> m_userBubbleBounds;

    void RebuildUserBubbleBounds(wxRichTextBuffer& buffer) {
        m_userBubbleBounds.clear(); // Also removes cards after clear/replay.
        long group = 0;
        wxRect card;
        auto cacheCard = [&]() {
            if (group == 0 || card.IsEmpty()) return;
            const wxRect scaled = GetScaledRect(card);
            if (!scaled.IsEmpty())
                m_userBubbleBounds.push_back({scaled, scaled.GetBottom()});
        };
        for (auto node = buffer.GetChildren().GetFirst(); node; node = node->GetNext()) {
            auto* paragraph = dynamic_cast<wxRichTextParagraph*>(node->GetData());
            const long next = paragraph && paragraph->GetProperties().HasProperty("lb_user_group")
                ? paragraph->GetProperties().GetPropertyLong("lb_user_group") : 0;
            if (next != group) { cacheCard(); group = next; card = wxRect(); }
            if (group == 0 || !paragraph) continue;
            const auto& properties = paragraph->GetProperties();
            wxRect part = paragraph->GetRect();
            part.x += static_cast<int>(properties.GetPropertyLong("lb_user_left"));
            part.width = static_cast<int>(properties.GetPropertyLong("lb_user_width"));
            if (card.IsEmpty()) card = part;
            else card.Union(part);
        }
        cacheCard();

        // Paragraphs normally arrive in vertical order. Avoid sorting that
        // common case while keeping the index correct for unusual layouts.
        auto above = [](const UserBubbleBounds& a, const UserBubbleBounds& b) {
            return a.rect.GetTop() < b.rect.GetTop();
        };
        if (!std::is_sorted(m_userBubbleBounds.begin(), m_userBubbleBounds.end(), above))
            std::stable_sort(m_userBubbleBounds.begin(), m_userBubbleBounds.end(), above);
        for (std::size_t i = 1; i < m_userBubbleBounds.size(); ++i)
            m_userBubbleBounds[i].prefixBottom = std::max(
                m_userBubbleBounds[i - 1].prefixBottom,
                m_userBubbleBounds[i].rect.GetBottom());
    }

protected:
    bool ScrollIntoView(long position, int keyCode) override {
        // Programmatic writes must not run the editor's caret-follow scroll.
        // User keyboard navigation outside a transcript update still uses it.
        if (IsTranscriptUpdateActive()) return false;
        return wxRichTextCtrl::ScrollIntoView(position, keyCode);
    }

    void DoThaw() override {
        if (!m_restoreTranscriptView) {
            wxRichTextCtrl::DoThaw();
            return;
        }

        // Match wxRichTextCtrl::DoThaw's layout ordering, inserting viewport
        // restoration before wxWindow re-enables painting. SetupScrollbars
        // intentionally does nothing while frozen, so the old pre-Thaw fix
        // used stale virtual dimensions and could scroll twice per frame.
        if (GetBuffer().IsDirty()) LayoutContent();
        else SetupScrollbars();

        if (m_transcriptSelectionStart != m_transcriptSelectionEnd) {
            const long end = GetLastPosition();
            SetSelection(std::min(m_transcriptSelectionStart, end),
                         std::min(m_transcriptSelectionEnd, end));
        }

        int ppuX = 0, ppuY = 0;
        GetScrollPixelsPerUnit(&ppuX, &ppuY);
        if (ppuY > 0) {
            const int maxTop = std::max(0, GetVirtualSize().y - GetClientSize().y);
            const int bottom = (maxTop + ppuY - 1) / ppuY;
            const int anchor = m_transcriptPpuY > 0
                ? static_cast<int>((static_cast<long long>(m_transcriptViewY) * m_transcriptPpuY) / ppuY)
                : m_transcriptViewY;
            Scroll(m_transcriptViewX, m_transcriptFollow ? bottom : std::min(anchor, bottom));
        }
        wxWindow::DoThaw();
    }

private:
    int m_userLayoutWidth = -1;
    long m_userMessageSerial = 0;
    wxColour m_userBubble{65, 80, 99};

    int m_transcriptUpdateDepth = 0;
    bool m_transcriptOwnsFreeze = false;
    bool m_restoreTranscriptView = false;
    bool m_transcriptFollow = true;
    int m_transcriptViewX = 0, m_transcriptViewY = 0, m_transcriptPpuY = 0;
    long m_transcriptSelectionStart = 0, m_transcriptSelectionEnd = 0;
    std::function<void()> m_viewportChangingHandler;

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

    std::function<void()> m_viewportChangedHandler;
    bool m_viewportNotifyPending = false;

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

    void NotifyViewportChangedSoon() {
        // Pause follow immediately, before another queued stream frame can
        // undo the user's wheel/key/pan movement. Recheck after input settles.
        if (m_viewportChangingHandler) m_viewportChangingHandler();
        if (m_viewportNotifyPending) return;
        m_viewportNotifyPending = true;

        CallAfter([this]() {
            m_viewportNotifyPending = false;
            if (m_viewportChangedHandler) m_viewportChangedHandler();
        });
    }

    void OnFocusGained(wxFocusEvent& evt) {
        evt.Skip();
        SuppressCaretSoon();
    }

    void OnMouseDown(wxMouseEvent& evt) {
        // Any other click ends sticky pan mode (Windows behavior) and
        // swallows that click so it doesn't also move the selection.
#ifdef __WXMSW__
        if (m_panning) { StopPan(); return; }
#endif
        evt.Skip();
        SuppressCaretSoon();
    }

    // ── Wheel speed ─────────────────────────────────────────────
    void OnMouseWheel(wxMouseEvent& evt) {
#ifdef __WXMSW__
        if (m_panning) StopPan();     // wheel input cancels pan mode
#endif

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
        NotifyViewportChangedSoon();
        // Deliberately no evt.Skip(): the base handler would scroll a
        // second, slower time.
    }

    void OnKeyDown(wxKeyEvent& evt) {
#ifdef __WXMSW__
        // Any key exits sticky middle-click pan mode. Preserve the existing
        // behavior: the cancelling key is consumed instead of also moving
        // the caret/selection beneath the pan marker.
        if (m_panning) { StopPan(); return; }
#endif

        const int key = evt.GetKeyCode();
        const bool mayMoveViewport =
            key == WXK_UP       || key == WXK_DOWN ||
            key == WXK_PAGEUP   || key == WXK_PAGEDOWN ||
            key == WXK_HOME     || key == WXK_END ||
            key == WXK_SPACE;

        evt.Skip();
        if (mayMoveViewport) NotifyViewportChangedSoon();
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
        NotifyViewportChangedSoon();
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
            NotifyViewportChangedSoon();
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

        NotifyViewportChangedSoon();
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
        NotifyViewportChangedSoon();
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
        NotifyViewportChangedSoon();
        SuppressCaretSoon();
    }

    void StopAutoScroll() {
        if (m_autoScrollTimer.IsRunning())
            m_autoScrollTimer.Stop();
        m_scrollDirection = 0;
        m_scrollIntensity = 0;
    }
};

// Scoped transcript mutation used by both ChatDisplay and MarkdownRenderer.
// The fallback keeps standalone renderers usable with a plain wxRichTextCtrl.
class TranscriptUpdateGuard {
public:
    TranscriptUpdateGuard(wxRichTextCtrl* ctrl, bool follow, bool enabled = true)
        : m_ctrl(enabled ? ctrl : nullptr), m_follow(follow) {
        if (!m_ctrl) return;
        m_transcript = dynamic_cast<ChatDisplayCtrl*>(m_ctrl);
        if (m_transcript) m_transcript->BeginTranscriptUpdate(follow);
        else if (!m_ctrl->IsFrozen()) {
            m_fallback = true;
            m_ctrl->GetViewStart(&m_x, &m_y);
            m_ctrl->Freeze();
        }
    }
    ~TranscriptUpdateGuard() {
        if (m_transcript) m_transcript->EndTranscriptUpdate();
        else if (m_fallback) {
            m_ctrl->Thaw();
            if (m_follow) {
                int ppuX = 0, ppuY = 0;
                m_ctrl->GetScrollPixelsPerUnit(&ppuX, &ppuY);
                if (ppuY > 0) {
                    const int maxTop = std::max(0, m_ctrl->GetVirtualSize().y - m_ctrl->GetClientSize().y);
                    m_ctrl->Scroll(m_x, (maxTop + ppuY - 1) / ppuY);
                }
            } else m_ctrl->Scroll(m_x, m_y);
        }
    }
    TranscriptUpdateGuard(const TranscriptUpdateGuard&) = delete;
    TranscriptUpdateGuard& operator=(const TranscriptUpdateGuard&) = delete;
private:
    wxRichTextCtrl* m_ctrl = nullptr;
    ChatDisplayCtrl* m_transcript = nullptr;
    bool m_follow = false, m_fallback = false;
    int m_x = 0, m_y = 0;
};
