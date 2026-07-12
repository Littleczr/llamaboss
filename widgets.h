// widgets.h
// Self-contained reusable UI widgets extracted from LlamaBoss.cpp.
#pragma once

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>

// Forward declaration
struct ThemeData;

// ─── Custom status dot panel (green/gray circle) ─────────────────
class StatusDot : public wxPanel {
public:
    StatusDot(wxWindow* parent, int size = 8)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(size, size))
        , m_connected(true), m_size(size)
        , m_connectedColor(94, 181, 247)
        , m_disconnectedColor(109, 127, 142)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &StatusDot::OnPaint, this);
    }
    void SetConnected(bool connected) {
        m_connected = connected;
        Refresh();
    }
    void SetColors(const wxColour& connected, const wxColour& disconnected) {
        m_connectedColor = connected;
        m_disconnectedColor = disconnected;
        Refresh();
    }
private:
    bool m_connected;
    int m_size;
    wxColour m_connectedColor;
    wxColour m_disconnectedColor;
    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_connected ? m_connectedColor : m_disconnectedColor));
        dc.DrawCircle(m_size / 2, m_size / 2, m_size / 2 - 1);
    }
};

// ─── Attachment chip (file pill with × remove button) ────────────
// A small colored panel showing icon + filename + clickable ×.
class AttachmentChip : public wxPanel {
public:
    AttachmentChip(wxWindow* parent, size_t index,
                   const std::string& icon, const std::string& name,
                   const wxColour& chipBg, const wxColour& textColor,
                   const wxColour& closeColor,
                   std::function<void(size_t)> onRemove)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(chipBg);
        auto* sizer = new wxBoxSizer(wxHORIZONTAL);

        // Icon + filename
        auto* label = new wxStaticText(this, wxID_ANY,
            wxString::FromUTF8(icon + " " + name));
        { wxFont f = label->GetFont(); f.SetPointSize(9); label->SetFont(f); }
        label->SetForegroundColour(textColor);
        label->SetBackgroundColour(chipBg);
        sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 5);

        // × close button
        auto* closeBtn = new wxStaticText(this, wxID_ANY,
            wxString::FromUTF8(" \xC3\x97"));  // × (U+00D7)
        { wxFont f = closeBtn->GetFont(); f.SetPointSize(11); closeBtn->SetFont(f); }
        closeBtn->SetForegroundColour(closeColor);
        closeBtn->SetBackgroundColour(chipBg);
        closeBtn->SetCursor(wxCursor(wxCURSOR_HAND));

        // Capture callback + index by value — chip may be destroyed during removal.
        // CallAfter on the chip bar (parent) ensures the event handler has returned
        // before RebuildAttachmentChips destroys all chips.
        auto removeFn = onRemove;
        size_t idx = index;
        closeBtn->Bind(wxEVT_LEFT_UP, [parent, removeFn, idx](wxMouseEvent&) {
            parent->CallAfter([removeFn, idx]() {
                if (removeFn) removeFn(idx);
            });
        });

        sizer->Add(closeBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
        sizer->AddSpacer(6);

        SetSizer(sizer);
        Fit();
    }
};

// ─── Generic tick slider (snaps to discrete values) ──────────────
// Custom-drawn horizontal slider with N preset tick positions. Used
// for the context length (4k → 256k) and font size (10pt → 24pt)
// controls in Settings. Native wxSlider looks gray-Windows on our
// dark theme and can't be recoloured — this draws its own track.
class TickSlider : public wxPanel {
public:
    // tickValues + tickLabels must be the same length (1-to-1).
    // initialValue snaps to the nearest value in tickValues.
    // onChange fires with the new value whenever the knob moves.
    TickSlider(wxWindow* parent,
               std::vector<int> tickValues,
               std::vector<std::string> tickLabels,
               int initialValue,
               std::function<void(int)> onChange = nullptr,
               int height = 56)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, height))
        , m_tickValues(std::move(tickValues))
        , m_tickLabels(std::move(tickLabels))
        , m_onChange(std::move(onChange))
        , m_tickIdx(0)
        , m_dragging(false)
        , m_trackColor   (55, 65, 80)
        , m_fillColor    (94, 181, 247)
        , m_knobColor    (94, 181, 247)
        , m_labelColor   (245, 245, 245)
        , m_labelDimColor(109, 127, 142)
    {
        // Guard against misuse — if someone passes empty arrays we'd
        // divide by zero in TickX. Default to a single phantom tick.
        if (m_tickValues.empty()) {
            m_tickValues = { initialValue };
            m_tickLabels = { std::to_string(initialValue) };
        }
        m_tickIdx = FindClosestTickIndex(initialValue);

        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(380, height));

        Bind(wxEVT_PAINT,     &TickSlider::OnPaint,    this);
        Bind(wxEVT_LEFT_DOWN, &TickSlider::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP,   &TickSlider::OnLeftUp,   this);
        Bind(wxEVT_MOTION,    &TickSlider::OnMotion,   this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST,
            [this](wxMouseCaptureLostEvent&) { m_dragging = false; });
        Bind(wxEVT_SIZE,
            [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
    }

    void SetColors(const wxColour& track, const wxColour& fill, const wxColour& knob,
                   const wxColour& label, const wxColour& labelDim) {
        m_trackColor    = track;
        m_fillColor     = fill;
        m_knobColor     = knob;
        m_labelColor    = label;
        m_labelDimColor = labelDim;
        Refresh();
    }

    int  GetValue() const { return m_tickValues[m_tickIdx]; }
    void SetValue(int v)  { m_tickIdx = FindClosestTickIndex(v); Refresh(); }

private:
    // Layout constants (pixel offsets within the panel)
    static constexpr int kTrackHeight  = 5;
    static constexpr int kKnobRadius   = 8;
    static constexpr int kTrackMarginX = 16;  // knob can't overflow panel edges
    static constexpr int kTrackY       = 14;  // track Y position

    std::vector<int>         m_tickValues;
    std::vector<std::string> m_tickLabels;
    std::function<void(int)> m_onChange;
    int      m_tickIdx;
    bool     m_dragging;
    wxColour m_trackColor, m_fillColor, m_knobColor;
    wxColour m_labelColor, m_labelDimColor;

    int TickCount() const { return (int)m_tickValues.size(); }

    int FindClosestTickIndex(int v) const {
        int best = 0, bestDiff = INT_MAX;
        for (int i = 0; i < TickCount(); ++i) {
            int diff = std::abs(v - m_tickValues[i]);
            if (diff < bestDiff) { best = i; bestDiff = diff; }
        }
        return best;
    }

    int TrackLeft()  const { return kTrackMarginX; }
    int TrackRight() const { return GetClientSize().x - kTrackMarginX; }
    int TrackWidth() const { return std::max(1, TrackRight() - TrackLeft()); }
    int TickX(int idx) const {
        const int denom = std::max(1, TickCount() - 1);
        return TrackLeft() + (TrackWidth() * idx) / denom;
    }

    int ClosestTickFromX(int x) const {
        int best = 0, bestDiff = INT_MAX;
        for (int i = 0; i < TickCount(); ++i) {
            int diff = std::abs(x - TickX(i));
            if (diff < bestDiff) { best = i; bestDiff = diff; }
        }
        return best;
    }

    void SnapTo(int x) {
        int newIdx = std::clamp(ClosestTickFromX(x), 0, TickCount() - 1);
        if (newIdx != m_tickIdx) {
            m_tickIdx = newIdx;
            Refresh();
            if (m_onChange) m_onChange(GetValue());
        }
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();

        const int tl = TrackLeft();
        const int tr = TrackRight();
        const int knobX = TickX(m_tickIdx);

        // Unfilled track (rounded ends)
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_trackColor));
        dc.DrawRoundedRectangle(tl, kTrackY, tr - tl, kTrackHeight, kTrackHeight / 2);

        // Filled portion (left → knob)
        if (knobX > tl) {
            dc.SetBrush(wxBrush(m_fillColor));
            dc.DrawRoundedRectangle(tl, kTrackY, knobX - tl, kTrackHeight, kTrackHeight / 2);
        }

        // Knob: outer halo ring in bg color, then colored centre
        dc.SetBrush(wxBrush(GetParent()->GetBackgroundColour()));
        dc.DrawCircle(knobX, kTrackY + kTrackHeight / 2, kKnobRadius + 2);
        dc.SetBrush(wxBrush(m_knobColor));
        dc.DrawCircle(knobX, kTrackY + kTrackHeight / 2, kKnobRadius);

        // Tick labels beneath the track. Selected tick gets full-opacity
        // color; others are dimmed so the active value reads clearly.
        wxFont labelFont = GetFont();
        labelFont.SetPointSize(8);
        dc.SetFont(labelFont);

        const int labelY = kTrackY + kTrackHeight + 12;
        for (int i = 0; i < TickCount(); ++i) {
            dc.SetTextForeground((i == m_tickIdx) ? m_labelColor : m_labelDimColor);
            wxString lbl = wxString::FromUTF8(m_tickLabels[i]);
            wxSize ts = dc.GetTextExtent(lbl);
            dc.DrawText(lbl, TickX(i) - ts.x / 2, labelY);
        }
    }

    void OnLeftDown(wxMouseEvent& e) {
        m_dragging = true;
        if (!HasCapture()) CaptureMouse();
        SnapTo(e.GetX());
    }
    void OnLeftUp(wxMouseEvent&) {
        if (m_dragging) {
            m_dragging = false;
            if (HasCapture()) ReleaseMouse();
        }
    }
    void OnMotion(wxMouseEvent& e) {
        if (m_dragging) SnapTo(e.GetX());
    }
};

// ─── Outlined button ─────────────────────────────────────────────
// Custom-painted button: 1px accent border, transparent fill, accent
// label. Hover blends a faint accent into the fill; pressed inverts
// (solid accent fill, surface-color label) — same visual logic as a
// terminal cursor block. Focus draws a dotted inner ring.
//
// Native wxButton on Windows can't be drawn this way without OS
// chrome leaking through, so we paint it ourselves (same pattern as
// TickSlider above). Emits wxEVT_BUTTON like wxButton, so existing
// Bind(wxEVT_BUTTON, …) call sites work unchanged.
class OutlinedButton : public wxControl {
public:
    OutlinedButton(wxWindow* parent, wxWindowID id, const wxString& label,
                   const wxSize& size = wxDefaultSize)
        : wxControl(parent, id, wxDefaultPosition, size, wxBORDER_NONE)
        , m_label(label)
        , m_state(State::Normal)
        , m_isDefault(false)
        , m_borderColor      (94, 181, 247)
        , m_textColor        (94, 181, 247)
        , m_pressedTextColor (23, 33, 43)
        , m_bgColor          (23, 33, 43)
        , m_disabledBorder   (70, 80, 95)
        , m_disabledText     (109, 127, 142)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        if (size == wxDefaultSize) SetMinSize(wxSize(-1, 32));

        Bind(wxEVT_PAINT,        &OutlinedButton::OnPaint,    this);
        Bind(wxEVT_LEFT_DOWN,    &OutlinedButton::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP,      &OutlinedButton::OnLeftUp,   this);
        Bind(wxEVT_ENTER_WINDOW, &OutlinedButton::OnEnter,    this);
        Bind(wxEVT_LEAVE_WINDOW, &OutlinedButton::OnLeave,    this);
        Bind(wxEVT_KEY_DOWN,     &OutlinedButton::OnKeyDown,  this);
        Bind(wxEVT_SET_FOCUS,    [this](wxFocusEvent& e){ Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS,   [this](wxFocusEvent& e){ Refresh(); e.Skip(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST,
            [this](wxMouseCaptureLostEvent&){ m_state = State::Normal; Refresh(); });

        SetCursor(wxCursor(wxCURSOR_HAND));
    }

    // Apply theme palette. Border + label use accentButton; pressed
    // label uses bgColor (so the inversion reads against the surface);
    // disabled tones come from textMuted + a darker border blend.
    void SetColors(const wxColour& accent,
                   const wxColour& bg,
                   const wxColour& muted)
    {
        m_borderColor      = accent;
        m_textColor        = accent;
        m_pressedTextColor = bg;
        m_bgColor          = bg;
        m_disabledText     = muted;
        m_disabledBorder   = BlendTowards(muted, bg, 0.5);
        Refresh();
    }

    // wxButton API parity. Currently a paint hint only — slightly thicker
    // border so the eye picks it as primary in a footer pair. Dialog-level
    // Enter-triggers-OK is wired separately in the dialog (CHAR_HOOK).
    void SetDefault() { m_isDefault = true; Refresh(); }

    void SetLabel(const wxString& label) override {
        m_label = label;
        InvalidateBestSize();
        Refresh();
    }
    wxString GetLabel() const override { return m_label; }

    bool AcceptsFocus()             const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return IsEnabled(); }

protected:
    wxSize DoGetBestSize() const override {
        wxClientDC dc(const_cast<OutlinedButton*>(this));
        dc.SetFont(GetFont());
        wxSize ts = dc.GetTextExtent(m_label);
        // 18px horizontal padding each side, 8px vertical.
        return wxSize(ts.x + 36, std::max(ts.y + 16, 30));
    }

private:
    enum class State { Normal, Hover, Pressed };

    wxString m_label;
    State    m_state;
    bool     m_isDefault;
    wxColour m_borderColor;
    wxColour m_textColor;
    wxColour m_pressedTextColor;
    wxColour m_bgColor;
    wxColour m_disabledBorder;
    wxColour m_disabledText;

    static wxColour BlendTowards(const wxColour& a, const wxColour& b, double t) {
        const double inv = 1.0 - t;
        return wxColour(
            (unsigned char)(a.Red()   * inv + b.Red()   * t),
            (unsigned char)(a.Green() * inv + b.Green() * t),
            (unsigned char)(a.Blue()  * inv + b.Blue()  * t));
    }

    void SendClickEvent() {
        wxCommandEvent ev(wxEVT_BUTTON, GetId());
        ev.SetEventObject(this);
        ProcessWindowEvent(ev);
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        // Match the parent surface so the rounded corners blend cleanly.
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();

        const wxSize sz = GetClientSize();
        const wxRect r(0, 0, sz.x, sz.y);
        const bool enabled = IsEnabled();

        wxColour border, fill, text;
        if (!enabled) {
            border = m_disabledBorder;
            fill   = m_bgColor;
            text   = m_disabledText;
        }
        else if (m_state == State::Pressed) {
            border = m_borderColor;
            fill   = m_borderColor;                                // solid accent
            text   = m_pressedTextColor;
        }
        else if (m_state == State::Hover) {
            border = m_borderColor;
            fill   = BlendTowards(m_bgColor, m_borderColor, 0.12); // ~12% accent
            text   = m_borderColor;
        }
        else { // Normal
            border = m_borderColor;
            fill   = m_bgColor;                                    // transparent
            text   = m_borderColor;
        }

        // Body fill (rounded)
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(fill));
        dc.DrawRoundedRectangle(r, 3);

        // Border — slightly thicker if this is the default item, except
        // when pressed (the solid fill carries enough weight already).
        const int borderW = (m_isDefault && m_state != State::Pressed && enabled) ? 2 : 1;
        dc.SetPen(wxPen(border, borderW));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        wxRect br = r;
        br.Deflate(borderW / 2, borderW / 2);
        dc.DrawRoundedRectangle(br, 3);

        // Focus ring — dotted inner border, only when focused + enabled
        if (HasFocus() && enabled) {
            dc.SetPen(wxPen(border, 1, wxPENSTYLE_DOT));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            wxRect fr = r;
            fr.Deflate(3, 3);
            dc.DrawRoundedRectangle(fr, 2);
        }

        // Label
        dc.SetFont(GetFont());
        dc.SetTextForeground(text);
        wxSize ts = dc.GetTextExtent(m_label);
        dc.DrawText(m_label, (sz.x - ts.x) / 2, (sz.y - ts.y) / 2);
    }

    void OnLeftDown(wxMouseEvent&) {
        if (!IsEnabled()) return;
        m_state = State::Pressed;
        if (!HasCapture()) CaptureMouse();
        SetFocus();
        Refresh();
    }
    void OnLeftUp(wxMouseEvent& e) {
        if (HasCapture()) ReleaseMouse();
        const bool wasPressed = (m_state == State::Pressed);
        wxRect r(GetClientSize());
        const bool inside = r.Contains(e.GetPosition());
        m_state = (IsEnabled() && inside) ? State::Hover : State::Normal;
        Refresh();
        if (wasPressed && IsEnabled() && inside) SendClickEvent();
    }
    void OnEnter(wxMouseEvent&) {
        if (!IsEnabled()) return;
        if (m_state != State::Pressed) m_state = State::Hover;
        Refresh();
    }
    void OnLeave(wxMouseEvent&) {
        if (!IsEnabled()) return;
        if (m_state != State::Pressed) m_state = State::Normal;
        Refresh();
    }
    void OnKeyDown(wxKeyEvent& e) {
        if (!IsEnabled()) { e.Skip(); return; }
        const int kc = e.GetKeyCode();
        if (kc == WXK_RETURN || kc == WXK_NUMPAD_ENTER || kc == WXK_SPACE) {
            SendClickEvent();
        } else {
            e.Skip();
        }
    }
};

// ─── Bracket-token button ────────────────────────────────────────
// Renders as `[ label ]` — text only, no fill, no border. Same visual
// vocabulary as the project status strip's `[ + attach ]` and `[ ... ]`
// affordances. For utility / dismiss / inline actions where a full
// button would be too loud against the rest of the chrome.
//
// `Accent` tone uses the theme accent color (active affordance).
// `Muted`  tone uses the muted text color (cancel / secondary).
//
// Emits wxEVT_BUTTON like wxButton.
class BracketButton : public wxControl {
public:
    enum class Tone { Accent, Muted };

    BracketButton(wxWindow* parent, wxWindowID id, const wxString& label,
                  Tone tone = Tone::Accent)
        : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_label(label)
        , m_tone(tone)
        , m_hover(false)
        , m_pressed(false)
        , m_normalColor   (94, 181, 247)
        , m_hoverColor    (245, 245, 245)
        , m_disabledColor (80, 90, 105)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));

        Bind(wxEVT_PAINT,        &BracketButton::OnPaint,    this);
        Bind(wxEVT_LEFT_DOWN,    &BracketButton::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP,      &BracketButton::OnLeftUp,   this);
        Bind(wxEVT_ENTER_WINDOW, &BracketButton::OnEnter,    this);
        Bind(wxEVT_LEAVE_WINDOW, &BracketButton::OnLeave,    this);
        Bind(wxEVT_KEY_DOWN,     &BracketButton::OnKeyDown,  this);
        Bind(wxEVT_SET_FOCUS,    [this](wxFocusEvent& e){ Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS,   [this](wxFocusEvent& e){ Refresh(); e.Skip(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST,
            [this](wxMouseCaptureLostEvent&){ m_pressed = false; Refresh(); });
    }

    // accent = primary tone color; muted = secondary tone color;
    // hover = brightened color when cursor is over the token.
    void SetColors(const wxColour& accent,
                   const wxColour& muted,
                   const wxColour& hover)
    {
        m_normalColor   = (m_tone == Tone::Accent) ? accent : muted;
        m_hoverColor    = hover;
        m_disabledColor = muted;
        Refresh();
    }

    void SetTone(Tone tone) { m_tone = tone; Refresh(); }
    Tone GetTone() const    { return m_tone; }

    void SetLabel(const wxString& label) override {
        m_label = label;
        InvalidateBestSize();
        Refresh();
    }
    wxString GetLabel() const override { return m_label; }

    bool AcceptsFocus()             const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return IsEnabled(); }

protected:
    wxSize DoGetBestSize() const override {
        wxClientDC dc(const_cast<BracketButton*>(this));
        dc.SetFont(GetFont());
        wxSize ts = dc.GetTextExtent(BracketedLabel());
        // 6px each side breathing, 10px vertical for click affordance.
        return wxSize(ts.x + 12, ts.y + 10);
    }

private:
    wxString m_label;
    Tone     m_tone;
    bool     m_hover;
    bool     m_pressed;
    wxColour m_normalColor;
    wxColour m_hoverColor;
    wxColour m_disabledColor;

    wxString BracketedLabel() const {
        return wxString::FromUTF8("[ ") + m_label + wxString::FromUTF8(" ]");
    }

    void SendClickEvent() {
        wxCommandEvent ev(wxEVT_BUTTON, GetId());
        ev.SetEventObject(this);
        ProcessWindowEvent(ev);
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();

        const wxSize sz = GetClientSize();
        wxColour text;
        if (!IsEnabled())              text = m_disabledColor;
        else if (m_hover || m_pressed) text = m_hoverColor;
        else                           text = m_normalColor;

        dc.SetFont(GetFont());
        dc.SetTextForeground(text);
        wxString out = BracketedLabel();
        wxSize ts = dc.GetTextExtent(out);
        const int x = (sz.x - ts.x) / 2;
        const int y = (sz.y - ts.y) / 2;
        dc.DrawText(out, x, y);

        // Focus indicator — dotted underline beneath the inner span.
        if (HasFocus() && IsEnabled()) {
            wxSize bracketWide = dc.GetTextExtent(wxString::FromUTF8("[ "));
            const int innerStart = x + bracketWide.x;
            const int innerEnd   = x + ts.x - bracketWide.x;
            const int uy         = y + ts.y - 1;
            dc.SetPen(wxPen(text, 1, wxPENSTYLE_DOT));
            dc.DrawLine(innerStart, uy, innerEnd, uy);
        }
    }

    void OnLeftDown(wxMouseEvent&) {
        if (!IsEnabled()) return;
        m_pressed = true;
        if (!HasCapture()) CaptureMouse();
        SetFocus();
        Refresh();
    }
    void OnLeftUp(wxMouseEvent& e) {
        if (HasCapture()) ReleaseMouse();
        const bool wasPressed = m_pressed;
        m_pressed = false;
        wxRect r(GetClientSize());
        const bool inside = r.Contains(e.GetPosition());
        m_hover = inside && IsEnabled();
        Refresh();
        if (wasPressed && inside && IsEnabled()) SendClickEvent();
    }
    void OnEnter(wxMouseEvent&) { if (IsEnabled()) { m_hover = true;  Refresh(); } }
    void OnLeave(wxMouseEvent&) { if (IsEnabled()) { m_hover = false; Refresh(); } }
    void OnKeyDown(wxKeyEvent& e) {
        if (!IsEnabled()) { e.Skip(); return; }
        const int kc = e.GetKeyCode();
        if (kc == WXK_RETURN || kc == WXK_NUMPAD_ENTER || kc == WXK_SPACE) {
            SendClickEvent();
        } else {
            e.Skip();
        }
    }
};

// ─── Dialog-theming helpers ──────────────────────────────────────

// Recurse the widget tree and apply sensible colors to labels and buttons.
// The default wxDialog::SetBackgroundColour only affects the dialog surface;
// labels and buttons nested inside sizers (especially wxStdDialogButtonSizer)
// need to be touched individually. Call this from a dialog's CreateControls
// after building the widget tree.
inline void ApplyDialogThemeRecursive(wxWindow* w,
                                      const wxColour& fg,
                                      const wxColour& btnBg,
                                      const wxColour& btnFg)
{
    if (!w) return;
    for (wxWindow* child : w->GetChildren()) {
        if (auto* lbl = dynamic_cast<wxStaticText*>(child)) {
            lbl->SetForegroundColour(fg);
        }
        else if (auto* btn = dynamic_cast<wxButton*>(child)) {
            btn->SetBackgroundColour(btnBg);
            btn->SetForegroundColour(btnFg);
        }
        // Recurse into panels / containers
        if (child->GetChildren().GetCount() > 0)
            ApplyDialogThemeRecursive(child, fg, btnBg, btnFg);
    }
}

// Windows-specific: tell DWM to draw the title bar in dark mode so the
// caption matches the dialog body. No-op on other platforms.
inline void ApplyDarkTitleBar(wxWindow* w, bool useDark)
{
#ifdef __WXMSW__
    if (!w) return;
    HWND hwnd = (HWND)w->GetHWND();
    if (!hwnd) return;
    BOOL value = useDark ? TRUE : FALSE;
    // Attribute 20 = DWMWA_USE_IMMERSIVE_DARK_MODE on Win11 / recent Win10.
    // Attribute 19 is the older name on pre-20H1 Windows 10 — try both.
    // If dwmapi.dll/this attribute isn't available the call just fails silently.
    typedef HRESULT(WINAPI* DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (!hDwm) return;
    auto pDwmSet = (DwmSetWindowAttributeFn)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    if (pDwmSet) {
        pDwmSet(hwnd, 20, &value, sizeof(value));
        pDwmSet(hwnd, 19, &value, sizeof(value));
    }
    FreeLibrary(hDwm);
#else
    (void)w; (void)useDark;
#endif
}
