// widgets.h
// Self-contained reusable UI widgets extracted from LlamaBoss.cpp.
#pragma once

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <functional>
#include <string>

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
