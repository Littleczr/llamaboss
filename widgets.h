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
