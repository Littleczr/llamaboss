// context_hud.cpp -- see context_hud.h.

#include "context_hud.h"

#include <wx/clipbrd.h>
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/settings.h>

#include <algorithm>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

namespace {

// Same warning colours as the ctx meter (LlamaBoss.cpp RefreshContextMeter),
// so amber/red mean the same thing in both places.
const wxColour kWarn(224, 175, 104);
const wxColour kDanger(224, 108, 117);

wxString U8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }

wxColour Mix(const wxColour& a, const wxColour& b, double t)
{
    auto ch = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(x + (y - x) * t + 0.5);
    };
    return wxColour(ch(a.Red(), b.Red()), ch(a.Green(), b.Green()), ch(a.Blue(), b.Blue()));
}

} // namespace

ContextHud::ContextHud(wxWindow* parent, const ThemeData& theme, HudModel model,
                       Actions actions)
    : wxPopupWindow(parent, wxBORDER_NONE)
    , m_theme(theme)
    , m_model(std::move(model))
    , m_actions(std::move(actions))
    , m_copiedTimer(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(m_theme.bgDialogSurface);

    // Same monospace family as the ctx meter and the Project/Skills strip.
    m_font = wxFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                    wxFONTWEIGHT_NORMAL, false, "Consolas");
    m_bold = m_font.Bold();

    RebuildButtons();

#ifdef __WXMSW__
    // Belt and braces with MSWWindowProc below: a WS_EX_NOACTIVATE window
    // is never activated by clicks or by Windows restoring focus.
    const HWND hwnd = static_cast<HWND>(GetHWND());
    ::SetWindowLongPtr(hwnd, GWL_EXSTYLE,
                       ::GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
#endif

    Bind(wxEVT_PAINT,        &ContextHud::OnPaint,  this);
    Bind(wxEVT_MOTION,       &ContextHud::OnMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, &ContextHud::OnLeave,  this);
    Bind(wxEVT_LEFT_UP,      &ContextHud::OnLeftUp, this);
    Bind(wxEVT_MOUSEWHEEL,   &ContextHud::OnWheel,  this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { m_showCopied = false; Refresh(); },
         m_copiedTimer.GetId());
}

void ContextHud::RebuildButtons()
{
    m_buttons.clear();
    m_hoverButton = -1;

    Button copy;
    copy.label  = "[ Copy stats ]";
    copy.action = [this]() {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(U8(m_model.plainText)));
            wxTheClipboard->Close();
            m_showCopied = true;
            m_copiedTimer.StartOnce(1200);
            Refresh();
        }
    };
    m_buttons.push_back(copy);

    if (m_actions.openLog) {
        Button log;
        log.label  = "[ Open log ]";
        // Stays open: the panel is pinned, the log opens in its own app.
        log.action = [this]() { if (m_actions.openLog) m_actions.openLog(); };
        m_buttons.push_back(log);
    }

    Button close;
    close.label      = "[ Close ]";
    close.alignRight = true;
    // The owner destroys us; CallAfter so we never die inside our own handler.
    close.action = [this]() {
        auto fn = m_actions.close;
        if (fn) CallAfter([fn]() { fn(); });
        else    Hide();
    };
    m_buttons.push_back(close);
}

void ContextHud::SetOpenLog(std::function<void()> openLog)
{
    const bool had = static_cast<bool>(m_actions.openLog);
    m_actions.openLog = std::move(openLog);
    if (had != static_cast<bool>(m_actions.openLog)) {
        RebuildButtons();
        Refresh();
    }
}

wxColour ContextHud::ToneColour(HudRow::Tone tone) const
{
    switch (tone) {
    case HudRow::Muted:  return m_theme.textMuted;
    case HudRow::Warn:   return kWarn;
    case HudRow::Danger: return kDanger;
    case HudRow::Good:   return LbInteractiveAccent(m_theme);
    default:             return m_theme.textPrimary;
    }
}

wxSize ContextHud::Measure()
{
    wxClientDC dc(this);
    dc.SetFont(m_font);

    m_pad   = FromDIP(14);
    m_gap   = FromDIP(14);
    m_barW  = FromDIP(96);
    m_barH  = FromDIP(6);
    m_lineH = dc.GetCharHeight() + FromDIP(5);

    // Column widths: labels, then values of rows that carry a bar (the
    // bar column starts after them).  Bar-less rows may run past it.
    m_labelW = 0;
    m_valueW = 0;
    int longest = 0;   // widest bar-less row / title line
    for (const HudSection& s : m_model.sections) {
        dc.SetFont(m_bold);
        int title = dc.GetTextExtent(U8(s.title)).x;
        dc.SetFont(m_font);
        if (!s.rightText.empty())
            title += m_gap + dc.GetTextExtent(U8(s.rightText)).x;
        longest = std::max(longest, title);

        for (const HudRow& r : s.rows)
            if (!r.label.empty())
                m_labelW = std::max(m_labelW, dc.GetTextExtent(U8(r.label)).x);
    }
    for (const HudSection& s : m_model.sections) {
        for (const HudRow& r : s.rows) {
            const int vw = dc.GetTextExtent(U8(r.value)).x;
            if (r.bar >= 0) {
                m_valueW = std::max(m_valueW, vw);
            } else {
                const int lead = r.label.empty() ? 0 : m_labelW + m_gap;
                longest = std::max(longest, lead + vw);
            }
        }
    }

    int buttons = 0;
    for (const Button& b : m_buttons)
        buttons += dc.GetTextExtent(b.label).x + m_gap;

    const int barred = m_labelW + m_gap + m_valueW + m_gap + m_barW;
    const int inner  = std::max({ barred, longest, buttons, FromDIP(360) });
    m_width = inner + 2 * m_pad;

    int sh = 0;
    for (size_t i = 0; i < m_model.sections.size(); ++i) {
        const HudSection& s = m_model.sections[i];
        if (i > 0) sh += FromDIP(10);                   // divider band
        sh += m_lineH;                                   // title
        if (s.headerBar >= 0) sh += m_barH + FromDIP(8);
        sh += m_lineH * static_cast<int>(s.rows.size());
    }
    m_sectionsH = sh;
    m_footerH   = FromDIP(10) + m_lineH;
    return wxSize(m_width, m_pad + m_sectionsH + m_footerH + m_pad);
}

wxRect ContextHud::WorkArea() const
{
    int d = wxNOT_FOUND;
    if (m_area) d = wxDisplay::GetFromWindow(m_area);
    if (d == wxNOT_FOUND) d = wxDisplay::GetFromWindow(this);
    if (d == wxNOT_FOUND) d = 0;
    return wxDisplay(static_cast<unsigned>(d)).GetClientArea();
}

wxSize ContextHud::FittedSize()
{
    wxSize size = Measure();
    // Leave a margin so the panel never touches the screen edges, and keep
    // room for at least the first rows + the footer on absurdly small
    // work areas.
    const wxRect work = WorkArea();
    const int margin = FromDIP(12);
    const int maxW = std::max(FromDIP(240), work.GetWidth()  - 2 * margin);
    const int maxH = std::max(2 * m_pad + m_footerH + 3 * m_lineH,
                              work.GetHeight() - 2 * margin);
    size.x = std::min(size.x, maxW);
    size.y = std::min(size.y, maxH);
    ClampScroll(size.y);
    return size;
}

int ContextHud::SectionsViewHeight(int clientH) const
{
    return std::max(0, clientH - 2 * m_pad - m_footerH);
}

void ContextHud::ClampScroll(int clientH)
{
    const int maxScroll = std::max(0, m_sectionsH - SectionsViewHeight(clientH));
    m_scrollY = std::max(0, std::min(m_scrollY, maxScroll));
}

void ContextHud::OnWheel(wxMouseEvent& e)
{
    const int clientH = GetClientSize().y;
    if (m_sectionsH <= SectionsViewHeight(clientH)) { e.Skip(); return; }
    const int delta = e.GetWheelDelta() > 0 ? e.GetWheelDelta() : 120;
    const int lines = e.GetLinesPerAction() > 0 ? e.GetLinesPerAction() : 3;
    const int before = m_scrollY;
    m_scrollY -= e.GetWheelRotation() * lines * m_lineH / delta;
    ClampScroll(clientH);
    if (m_scrollY != before) Refresh();
}

void ContextHud::ShowInCorner(wxWindow* area)
{
    m_area = area;
    SetSize(FittedSize());
    Reposition();
    if (!IsShown()) Show();
}

void ContextHud::Reposition()
{
    const wxSize size = GetSize();
    const wxRect screen = WorkArea();
    wxRect box = screen;
    if (m_area) box = m_area->GetScreenRect();
    const int inset  = FromDIP(12);
    const int scroll = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, m_area);
    wxPoint pos(box.GetRight()  - scroll - inset - size.x,
                box.GetBottom() - inset - size.y);
    // Keep the whole panel on screen even if the chat view is tiny.
    pos.x = std::max(screen.GetLeft(), std::min(pos.x, screen.GetRight()  - size.x));
    pos.y = std::max(screen.GetTop(),  std::min(pos.y, screen.GetBottom() - size.y));
    if (pos != GetPosition()) SetPosition(pos);
}

void ContextHud::SetModel(HudModel model)
{
    m_model = std::move(model);
    const wxSize size = FittedSize();
    if (size != GetSize()) {
        SetSize(size);
        Reposition();   // grows up and left from the pinned corner
    }
    Refresh();
}

void ContextHud::ApplyTheme(const ThemeData& theme)
{
    m_theme = theme;
    SetBackgroundColour(m_theme.bgDialogSurface);
    Refresh();
}

void ContextHud::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    const wxSize sz = GetClientSize();

    dc.SetBackground(wxBrush(m_theme.bgDialogSurface));
    dc.Clear();
    dc.SetPen(wxPen(m_theme.borderSubtle));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(0, 0, sz.x, sz.y);

    const int left  = m_pad;
    const int right = sz.x - m_pad;
    const int valueX = left + m_labelW + m_gap;
    const int barX   = valueX + m_valueW + m_gap;
    const wxColour track = Mix(m_theme.bgDialogSurface, m_theme.textMuted, 0.18);

    // maxW <= 0 means "no limit".  Text wider than the room it has (a
    // capped panel: long model name, huge scaling) is ellipsized rather
    // than drawn past the edge.
    auto text = [&](const wxString& s, int x, int y, const wxColour& c, int maxW = 0) {
        dc.SetTextForeground(c);
        wxString t = s;
        if (maxW > 0 && dc.GetTextExtent(t).x > maxW)
            t = wxControl::Ellipsize(t, dc, wxELLIPSIZE_END, maxW);
        dc.DrawText(t, x, y + (m_lineH - dc.GetCharHeight()) / 2);
    };
    auto bar = [&](int x, int y, int w, double frac, const wxColour& fill) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(track));
        dc.DrawRectangle(x, y, w, m_barH);
        const int fw = static_cast<int>(w * std::min(1.0, std::max(0.0, frac)) + 0.5);
        if (fw > 0) {
            dc.SetBrush(wxBrush(fill));
            dc.DrawRectangle(x, y, std::max(fw, FromDIP(2)), m_barH);
        }
    };

    // Sections scroll inside [m_pad, m_pad + viewH); the footer stays put.
    const int viewTop = m_pad;
    const int viewH   = SectionsViewHeight(sz.y);
    const bool overflow = m_sectionsH > viewH;
    ClampScroll(sz.y);
    // Bars need a little room; on a width-capped panel they may not fit.
    const int barRoom = right - barX;
    const bool drawBars = barRoom >= FromDIP(24);

    dc.SetClippingRegion(0, viewTop, sz.x, viewH);
    int y = viewTop - m_scrollY;
    for (size_t i = 0; i < m_model.sections.size(); ++i) {
        const HudSection& s = m_model.sections[i];
        if (i > 0) {
            dc.SetPen(wxPen(m_theme.borderSubtle));
            dc.DrawLine(left, y + FromDIP(4), right, y + FromDIP(4));
            y += FromDIP(10);
        }

        int titleRoom = right - left;
        if (!s.rightText.empty()) {
            const wxString r = U8(s.rightText);
            dc.SetFont(m_font);
            const int rw = std::min(dc.GetTextExtent(r).x, (right - left) / 2);
            text(r, right - rw, y,
                 s.headerBar >= 0 && s.headerTone != HudRow::Good
                     ? ToneColour(s.headerTone) : m_theme.textMuted, rw);
            titleRoom -= rw + m_gap;
        }
        dc.SetFont(m_bold);
        text(U8(s.title), left, y, m_theme.textPrimary, std::max(1, titleRoom));
        dc.SetFont(m_font);
        y += m_lineH;

        if (s.headerBar >= 0) {
            bar(left, y + FromDIP(2), right - left, s.headerBar, ToneColour(s.headerTone));
            y += m_barH + FromDIP(8);
        }

        for (const HudRow& r : s.rows) {
            const wxColour vc = ToneColour(r.tone);
            // Barred rows keep their value inside the value column (when
            // the bar is drawn); bar-less rows may run to the right edge.
            const int valueRoom = (r.bar >= 0 && drawBars)
                ? std::min(m_valueW, right - valueX) : right - valueX;
            if (!r.label.empty())
                text(U8(r.label), left, y, m_theme.textMuted,
                     std::max(1, std::min(m_labelW, right - left)));
            text(U8(r.value), valueX, y, vc, std::max(1, valueRoom));
            if (r.bar >= 0 && drawBars)
                bar(barX, y + (m_lineH - m_barH) / 2, barRoom, r.bar,
                    LbInteractiveAccent(m_theme));
            y += m_lineH;
        }
    }

    dc.DestroyClippingRegion();

    // Scroll thumb: a thin bar on the right edge, only when the sections
    // are taller than the panel.  Wheel over the panel scrolls.
    if (overflow && viewH > 0) {
        const int trackW = FromDIP(3);
        const int thumbH = std::max(FromDIP(16),
                                    viewH * viewH / std::max(1, m_sectionsH));
        const int maxScroll = std::max(1, m_sectionsH - viewH);
        const int thumbY = viewTop + (viewH - thumbH) * m_scrollY / maxScroll;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(track));
        dc.DrawRectangle(sz.x - trackW - FromDIP(3), viewTop, trackW, viewH);
        dc.SetBrush(wxBrush(m_theme.textMuted));
        dc.DrawRectangle(sz.x - trackW - FromDIP(3), thumbY, trackW, thumbH);
    }

    // Footer actions: pinned to the bottom so they stay reachable while
    // the sections scroll.  (Unscrolled and uncapped, this is exactly
    // where the footer used to follow the last section.)
    y = viewTop + viewH + FromDIP(10);
    dc.SetFont(m_font);
    int x = left;
    for (size_t i = 0; i < m_buttons.size(); ++i) {
        Button& b = m_buttons[i];
        const wxString label = (i == 0 && m_showCopied) ? wxString("[ Copied ]") : b.label;
        const wxSize ext = dc.GetTextExtent(b.label);
        const int bx = b.alignRight ? right - ext.x : x;
        b.rect = wxRect(bx, y, ext.x, m_lineH);
        text(label, bx, y, static_cast<int>(i) == m_hoverButton
                               ? LbInteractiveAccent(m_theme) : m_theme.textMuted);
        if (!b.alignRight) x += ext.x + m_gap;
    }
}

void ContextHud::OnMotion(wxMouseEvent& e)
{
    int hit = -1;
    for (size_t i = 0; i < m_buttons.size(); ++i)
        if (m_buttons[i].rect.Contains(e.GetPosition())) hit = static_cast<int>(i);
    if (hit != m_hoverButton) {
        m_hoverButton = hit;
        SetCursor(hit >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
        Refresh();
    }
    e.Skip();
}

void ContextHud::OnLeave(wxMouseEvent& e)
{
    if (m_hoverButton != -1) {
        m_hoverButton = -1;
        SetCursor(wxNullCursor);
        Refresh();
    }
    e.Skip();
}

void ContextHud::OnLeftUp(wxMouseEvent& e)
{
    for (const Button& b : m_buttons) {
        if (b.rect.Contains(e.GetPosition()) && b.action) {
            b.action();
            return;
        }
    }
    e.Skip();
}

#ifdef __WXMSW__
WXLRESULT ContextHud::MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam)
{
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;   // clicks still delivered
    return wxPopupWindow::MSWWindowProc(msg, wParam, lParam);
}
#endif
