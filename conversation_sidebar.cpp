// conversation_sidebar.cpp
// Implementation of the collapsible conversation sidebar.
// Supports multi-select (Ctrl+Click, Shift+Click) and batch delete.

#include "conversation_sidebar.h"
#include "chat_history.h"
#include "theme.h"
#include "path_safety.h"

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/dnd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>

#include <fstream>
#include <algorithm>
#include <cctype>
#include <array>
#include <cstdint>
#include <ctime>
#include <cstdlib>

#ifdef __WXMSW__
#include <windows.h>   // GetFileAttributesExW — cheap stat in ScanConversations
#endif

namespace {

// Decode JSON-style escape sequences from a string extracted out of a
// JSON value, for single-line UI label display.  Handles:
//   \"  \\  \/  \b  \f  \n  \r  \t
// Whitespace-control escapes (\b \f \n \r \t) collapse to a single
// space because the sidebar shows titles on one line -- letting a real
// newline through would make wxStaticText render a multi-line label,
// which is uglier than the literal-`\n` artifact we are fixing.
// Unknown escapes drop the backslash and keep the following character,
// matching the lenient behaviour of most JSON readers.
//
// \uXXXX is intentionally NOT handled: Poco's Stringifier (used to
// write our conversation files) emits non-ASCII as raw UTF-8 by
// default, so \uXXXX sequences are vanishingly rare in our files.
// If that changes, extend this helper rather than parsing whole JSON.
std::string JsonUnescapeForLabel(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c != '\\' || i + 1 >= in.size()) {
            out += c;
            continue;
        }
        const char next = in[i + 1];
        switch (next) {
        case '"':  out += '"';  ++i; break;
        case '\\': out += '\\'; ++i; break;
        case '/':  out += '/';  ++i; break;
        case 'b': case 'f': case 'n': case 'r': case 't':
            out += ' ';
            ++i;
            break;
        default:
            // Unknown escape: drop the backslash and keep the following
            // character.  Alternative would be to keep both literally;
            // dropping the backslash gives a cleaner-looking label.
            out += next;
            ++i;
            break;
        }
    }
    return out;
}

std::string WxToUtf8String(const wxString& s)
{
    wxCharBuffer buf = s.ToUTF8();
    return buf.data() ? std::string(buf.data()) : std::string();
}


// Parse the ISO-8601 timestamps emitted by ChatHistory::CurrentTimestamp().
// The stored updated_at value is the conversation activity clock; filesystem
// mtime remains a cache invalidation signal and is deliberately not used for
// chronological grouping because metadata-only rewrites legitimately change it.
bool ParseConversationActivityTime(const std::string& text, wxDateTime& out)
{
    auto parseDigits = [&](size_t pos, size_t count, int& value) -> bool {
        if (pos + count > text.size()) return false;
        int v = 0;
        for (size_t i = 0; i < count; ++i) {
            const unsigned char ch = static_cast<unsigned char>(text[pos + i]);
            if (!std::isdigit(ch)) return false;
            v = v * 10 + (ch - '0');
        }
        value = v;
        return true;
    };

    if (text.size() < 19 || text[4] != '-' || text[7] != '-' ||
        (text[10] != 'T' && text[10] != 't' && text[10] != ' ') ||
        text[13] != ':' || text[16] != ':') {
        return false;
    }

    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0, second = 0;
    if (!parseDigits(0, 4, year) || !parseDigits(5, 2, month) ||
        !parseDigits(8, 2, day) || !parseDigits(11, 2, hour) ||
        !parseDigits(14, 2, minute) || !parseDigits(17, 2, second)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 60) {
        return false;
    }

    size_t pos = 19;
    int milliseconds = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < text.size() && std::isdigit(
                   static_cast<unsigned char>(text[pos]))) {
            if (digits < 3)
                milliseconds = milliseconds * 10 + (text[pos] - '0');
            ++digits;
            ++pos;
        }
        while (digits < 3) {
            milliseconds *= 10;
            ++digits;
        }
    }

    int offsetSeconds = 0;
    bool hasExplicitZone = false;
    if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
        hasExplicitZone = true;
        ++pos;
    }
    else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        hasExplicitZone = true;
        const int sign = text[pos] == '+' ? 1 : -1;
        ++pos;
        int zoneHour = 0, zoneMinute = 0;
        if (!parseDigits(pos, 2, zoneHour)) return false;
        pos += 2;
        if (pos < text.size() && text[pos] == ':') ++pos;
        if (!parseDigits(pos, 2, zoneMinute)) return false;
        pos += 2;
        if (zoneHour > 23 || zoneMinute > 59) return false;
        offsetSeconds = sign * (zoneHour * 3600 + zoneMinute * 60);
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = minute;
    tm.tm_sec  = second == 60 ? 59 : second; // tolerate a leap-second stamp
    tm.tm_isdst = -1;

    std::time_t epoch = 0;
    if (hasExplicitZone) {
#ifdef __WXMSW__
        epoch = _mkgmtime(&tm);
#else
        epoch = timegm(&tm);
#endif
        if (epoch == static_cast<std::time_t>(-1)) return false;
        epoch -= offsetSeconds;
    }
    else {
        // Extremely old or hand-edited files may omit a zone. Interpret those
        // as local time rather than rejecting an otherwise useful timestamp.
        epoch = std::mktime(&tm);
        if (epoch == static_cast<std::time_t>(-1)) return false;
    }

    out = wxDateTime(epoch);
    out.SetMillisecond(static_cast<unsigned short>(milliseconds));
    return out.IsValid();
}

bool IsLegacySessionContextTitle(const std::string& title)
{
    return title.rfind("[Session context:", 0) == 0;
}

std::string CollapseSidebarTitleWhitespace(std::string text)
{
    std::string out;
    out.reserve(text.size());
    bool lastWasSpace = false;

    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            if (!lastWasSpace && !out.empty()) out.push_back(' ');
            lastWasSpace = true;
        } else {
            out.push_back(static_cast<char>(ch));
            lastWasSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Old conversation files created after the per-turn clock header was added
// may have persisted that internal header as their title.  For those files
// only, derive a display title from the first real user message while the
// sidebar is already scanning the JSON.  No model/API call is involved.
std::string MeaningfulTitleFromStoredUserContent(std::string content)
{
    if (content.rfind("[Session context:", 0) == 0) {
        const size_t closeBracket = content.find(']');
        if (closeBracket == std::string::npos) return {};
        content.erase(0, closeBracket + 1);
    }

    content = CollapseSidebarTitleWhitespace(std::move(content));
    return content;
}

wxString SidebarDisplayTitleFromUtf8(const std::string& utf8)
{
    // Keep the full decoded title in the label.  The row gives the title a
    // constrained width and clips it at the card edge, while the tooltip
    // exposes the complete text.  Do not append "..." here: the separate
    // conversation-actions control already owns that visual language, and a
    // second ellipsis at the end of a long title looks like a duplicate menu.
    return wxString::FromUTF8(utf8.c_str());
}

std::string SidebarSearchKeyFromUtf8(const std::string& utf8)
{
    // Search against the full title, not the shortened display label.
    // Lowercase in wxString space so Unicode text is handled much better
    // than byte-wise std::tolower on UTF-8.
    wxString title = wxString::FromUTF8(utf8.c_str());
    return WxToUtf8String(title.Lower());
}

wxColour MixSidebarColour(const wxColour& first,
                          const wxColour& second,
                          int secondPercent)
{
    secondPercent = std::clamp(secondPercent, 0, 100);
    const int firstPercent = 100 - secondPercent;

    auto mixChannel = [firstPercent, secondPercent](unsigned char a,
                                                    unsigned char b) {
        return static_cast<unsigned char>(
            (static_cast<int>(a) * firstPercent +
             static_cast<int>(b) * secondPercent + 50) / 100);
    };

    return wxColour(
        mixChannel(first.Red(),   second.Red()),
        mixChannel(first.Green(), second.Green()),
        mixChannel(first.Blue(),  second.Blue()));
}

// ── Monospace type: the sidebar's half of the LlamaBoss terminal idiom ──
// The rest of the app (project status strip, context meter, model pill,
// chat body) is teletype-faced via AppState::CreateMonospaceFont / explicit
// wxFONTFAMILY_TELETYPE "Consolas" fonts.  The sidebar previously used the
// stock proportional GetFont(), which is what made the redesigned cards read
// as generic rather than as part of this app.  We can't reach AppState from
// here (the sidebar only holds a ThemeData*), so mirror the same idiom
// locally: wxFONTFAMILY_TELETYPE guarantees a monospace fallback even when the
// named face is absent, and "Cascadia Mono" / "Consolas" both ship on modern
// Windows and both carry the box-drawing + geometric glyphs the section
// headers and icon tiles use.  Keeping the family as the real signal (not the
// face string) means custom/Linux builds still get a monospace face.
wxFont SidebarMonoFont(int pointSize,
                       wxFontWeight weight = wxFONTWEIGHT_NORMAL,
                       bool italic = false)
{
    wxFont font(pointSize, wxFONTFAMILY_TELETYPE,
                italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
                weight, false, "Cascadia Mono");
    if (!font.IsOk() || font.GetFaceName().empty())
        font = wxFont(pointSize, wxFONTFAMILY_TELETYPE,
                      italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
                      weight, false, "Consolas");
    return font;
}

int SidebarColourBrightness(const wxColour& colour)
{
    // Fast perceived-brightness estimate.  It is only used to decide how
    // aggressively a pale theme accent should be deepened for the primary
    // sidebar action.
    return (299 * colour.Red() +
            587 * colour.Green() +
            114 * colour.Blue()) / 1000;
}

wxColour SidebarPrimaryButtonBackground(const ThemeData& theme)
{
    // The mockup's New Chat action is a deeper, calmer blue than several
    // theme accents (Nord's accent is intentionally pale cyan, for example).
    // Pull pale accents toward LlamaBoss blue while retaining enough of the
    // active theme's original hue to keep the sidebar theme-aware.
    const wxColour brandBlue(43, 105, 210);
    const int brandWeight =
        SidebarColourBrightness(theme.accentButton) > 160 ? 48 : 18;
    return MixSidebarColour(theme.accentButton, brandBlue, brandWeight);
}

wxColour SidebarSearchBackground(const ThemeData& theme)
{
    return MixSidebarColour(theme.bgInputField, theme.bgDialogSurface, 24);
}

wxColour SidebarCardBackground(const ThemeData& theme)
{
    // A slightly lifted surface, but closer to the sidebar canvas than the
    // full dialog surface.  This keeps individual cards legible without the
    // heavy stacked-block appearance of the first prototype.
    return MixSidebarColour(theme.bgSidebar, theme.bgDialogSurface, 72);
}

std::uint32_t SidebarStableHash(const std::string& value)
{
    // FNV-1a gives project colors that remain stable across launches and
    // machines without introducing a new persisted color field in Phase 3.
    std::uint32_t hash = 2166136261u;
    for (unsigned char ch : value) {
        hash ^= static_cast<std::uint32_t>(ch);
        hash *= 16777619u;
    }
    return hash;
}

wxColour SidebarProjectAccent(const ThemeData& theme,
                              const std::string& projectId,
                              bool hasGoal)
{
    // Unassigned conversations intentionally stay neutral.  Their icon tile
    // still responds to selection/hover, but it does not imply a project.
    if (projectId.empty() && !hasGoal) {
        return MixSidebarColour(theme.textMuted, theme.bgDialogSurface, 28);
    }

    // Project-less goals get a consistent purple accent.  A chat carrying a
    // real project uses the project color because the project is its primary
    // container and remains its drag/drop destination.
    if (projectId.empty() && hasGoal) {
        return wxColour(167, 112, 239);
    }

    static const std::array<wxColour, 8> kProjectPalette = {
        wxColour(78, 144, 242),   // blue
        wxColour(45, 184, 166),   // teal
        wxColour(91, 173, 92),    // green
        wxColour(153, 110, 230),  // violet
        wxColour(221, 151, 55),   // amber
        wxColour(218, 100, 132),  // rose
        wxColour(73, 166, 201),   // cyan
        wxColour(202, 115, 69)    // orange
    };

    const wxColour base =
        kProjectPalette[SidebarStableHash(projectId) % kProjectPalette.size()];
    // A small theme-accent contribution keeps the palette harmonious with
    // custom themes without collapsing every project back to one color.
    return MixSidebarColour(base, theme.accentButton, 10);
}

std::string SidebarProjectTagText(const std::string& projectId,
                                  const std::string& projectName,
                                  bool hasGoal)
{
    if (!projectId.empty())
        return projectName.empty() ? std::string("Project") : projectName;
    if (hasGoal)
        return "Goal";
    return {};
}

class SidebarProjectTag final : public wxPanel
{
public:
    SidebarProjectTag(wxWindow* parent,
                      const std::string& label,
                      const wxColour& fill,
                      const wxColour& text)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(56, 20),
                  wxBORDER_NONE)
        , m_fill(fill)
        , m_text(text)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);

        // Mono-faced pill: the project/goal tag is a status token, so it wears
        // the same teletype face as the rest of the LlamaBoss chrome rather
        // than the stock proportional font.
        SetFont(SidebarMonoFont(8, wxFONTWEIGHT_MEDIUM));

        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { PaintTag(); });
        SetTagText(label);
    }

    void SetTagText(const std::string& label)
    {
        const wxString full = wxString::FromUTF8(label.c_str());
        if (full == m_fullLabel) {
            if (full.empty())
                Hide();
            return;
        }

        m_fullLabel = full;
        m_displayLabel = full;
        if (m_displayLabel.length() > 15)
            m_displayLabel = m_displayLabel.Left(12) + wxS("...");

        if (m_fullLabel.empty()) {
            Hide();
            return;
        }

        wxClientDC dc(this);
        dc.SetFont(GetFont());
        wxCoord textW = 0;
        wxCoord textH = 0;
        dc.GetTextExtent(m_displayLabel, &textW, &textH);
        const int width = std::clamp(static_cast<int>(textW) + 16, 42, 104);
        SetMinSize(wxSize(width, 20));
        SetToolTip(m_fullLabel);
        Show();
        InvalidateBestSize();
        Refresh();
    }

    void SetPalette(const wxColour& fill, const wxColour& text)
    {
        if (m_fill == fill && m_text == text)
            return;
        m_fill = fill;
        m_text = text;
        Refresh();
    }

private:
    void PaintTag()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()
            ? GetParent()->GetBackgroundColour()
            : wxColour(0, 0, 0)));
        dc.Clear();

        if (m_displayLabel.empty())
            return;

        wxRect rect = GetClientRect();
        rect.Deflate(1);

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_fill));
        // Squarer corners than a consumer pill — closer to the boxed status
        // tokens elsewhere in LlamaBoss while still softening the hard edge.
        dc.DrawRoundedRectangle(rect, 3);

        dc.SetFont(GetFont());
        dc.SetTextForeground(m_text);
        wxCoord textW = 0;
        wxCoord textH = 0;
        dc.GetTextExtent(m_displayLabel, &textW, &textH);
        dc.DrawText(m_displayLabel,
                    rect.x + (rect.width - textW) / 2,
                    rect.y + (rect.height - textH) / 2);
    }

    wxString m_fullLabel;
    wxString m_displayLabel;
    wxColour m_fill;
    wxColour m_text;
};

class SidebarCountBadge final : public wxPanel
{
public:
    SidebarCountBadge(wxWindow* parent,
                      const wxColour& fill,
                      const wxColour& text)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(28, 22),
                  wxBORDER_NONE)
        , m_fill(fill)
        , m_text(text)
    {
        SetMinSize(wxSize(28, 22));
        SetBackgroundStyle(wxBG_STYLE_PAINT);

        // Mono digits to match the boxed counts/meters used elsewhere.
        SetFont(SidebarMonoFont(9, wxFONTWEIGHT_BOLD));

        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { PaintBadge(); });
    }

    void SetCount(int count)
    {
        m_count = std::max(0, count);
        const int width = (m_count >= 100) ? 38 : ((m_count >= 10) ? 32 : 28);
        SetMinSize(wxSize(width, 22));
        Show(m_count > 0);
        Refresh();
    }

    void SetPalette(const wxColour& fill, const wxColour& text)
    {
        m_fill = fill;
        m_text = text;
        Refresh();
    }

private:
    void PaintBadge()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()
            ? GetParent()->GetBackgroundColour()
            : wxColour(0, 0, 0)));
        dc.Clear();

        wxRect rect = GetClientRect();
        rect.Deflate(1);

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_fill));
        dc.DrawRoundedRectangle(rect, 4);

        const wxString label = wxString::Format("%d", m_count);
        dc.SetFont(GetFont());
        dc.SetTextForeground(m_text);

        wxCoord textW = 0;
        wxCoord textH = 0;
        dc.GetTextExtent(label, &textW, &textH);
        dc.DrawText(label,
                    rect.x + (rect.width - textW) / 2,
                    rect.y + (rect.height - textH) / 2);
    }

    int      m_count = 0;
    wxColour m_fill;
    wxColour m_text;
};

// The conversation-actions control deliberately uses plain ASCII "...".
// It stays visible in every row, using a muted color when idle and the primary
// text color when the row is active, selected, or hovered.  This avoids both
// source-encoding problems and Windows Show()/Hide() repaint quirks, while a
// fixed-width slot keeps titles from growing underneath the action target.

} // namespace

// ═══════════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════════

ConversationSidebar::ConversationSidebar(wxWindow* parent,
                                         const ThemeData& theme,
                                         const Callbacks& callbacks,
                                         const std::vector<std::string>& initialCollapsed)
    : m_callbacks(callbacks)
    , m_theme(&theme)
{
    for (const auto& id : initialCollapsed) {
        if (!id.empty()) m_collapsedGroups.insert(id);
    }

    // ── Outer panel (sidebar + right border) ─────────────────────
    m_panel = new wxPanel(parent, wxID_ANY);
    m_panel->SetBackgroundColour(theme.bgSidebar);
    m_panel->SetMinSize(wxSize(360, -1));

    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // ── Content area ─────────────────────────────────────────────
    m_content = new wxPanel(m_panel, wxID_ANY);
    m_content->SetBackgroundColour(theme.bgSidebar);
    auto* contentSizer = new wxBoxSizer(wxVERTICAL);

    // "+ New Chat" button
    m_newChatButton = new wxButton(m_content, wxID_ANY, "+ New Chat",
        wxDefaultPosition, wxSize(-1, 42), wxBORDER_NONE);
    m_newChatButton->SetBackgroundColour(
        SidebarPrimaryButtonBackground(theme));
    m_newChatButton->SetForegroundColour(wxColour(255, 255, 255));
    m_newChatButton->SetFont(SidebarMonoFont(11, wxFONTWEIGHT_MEDIUM));
    contentSizer->Add(m_newChatButton, 0, wxEXPAND | wxALL, 8);

    // "+ New Window" button — shorter and regular-weight so New Chat
    // keeps visual priority, but same colors as New Chat so the pair
    // reads as one action group.  Same behavior as Ctrl+Shift+N; the
    // host frame provides the action.
    m_newWindowButton = new wxButton(m_content, wxID_ANY, "+ New Window",
        wxDefaultPosition, wxSize(-1, 34), wxBORDER_NONE);
    m_newWindowButton->SetBackgroundColour(theme.bgDialogSurface);
    m_newWindowButton->SetForegroundColour(theme.textPrimary);
    m_newWindowButton->SetFont(SidebarMonoFont(10));
    contentSizer->Add(m_newWindowButton, 0,
                      wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // Search box
    m_searchBox = new wxTextCtrl(m_content, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, 32),
        wxTE_PROCESS_ENTER | wxBORDER_SIMPLE);
    m_searchBox->SetBackgroundColour(SidebarSearchBackground(theme));
    m_searchBox->SetForegroundColour(theme.textMuted);
    m_searchBox->SetFont(SidebarMonoFont(10));
    ShowSearchHint();
    m_searchBox->SetToolTip(
        "Search conversations. Use Up/Down to choose a result and Enter to open.");
    contentSizer->Add(m_searchBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // Scrollable conversation list
    m_listWindow = new wxScrolledWindow(m_content, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_listWindow->SetBackgroundColour(theme.bgSidebar);
    m_listWindow->SetScrollRate(0, 8);
    m_listSizer = new wxBoxSizer(wxVERTICAL);
    m_listWindow->SetSizer(m_listSizer);

    contentSizer->Add(m_listWindow, 1, wxEXPAND);

    // Archive browser toggle.  Normal mode shows the archived count;
    // archive mode provides a clear path back to the active history.
    m_archiveButton = new wxButton(
        m_content, wxID_ANY, "Archived",
        wxDefaultPosition, wxSize(-1, 34), wxBORDER_NONE);
    m_archiveButton->SetBackgroundColour(theme.bgDialogSurface);
    m_archiveButton->SetForegroundColour(theme.textMuted);
    m_archiveButton->SetFont(SidebarMonoFont(9));
    contentSizer->Add(
        m_archiveButton, 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 8);
    UpdateArchiveButton();

    m_content->SetSizer(contentSizer);
    outerSizer->Add(m_content, 1, wxEXPAND);

    // Drag-resize handle on the right edge
    m_border = new wxPanel(m_panel, wxID_ANY, wxDefaultPosition, wxSize(BORDER_WIDTH, -1));
    m_border->SetBackgroundColour(theme.borderSubtle);
    m_border->SetCursor(wxCursor(wxCURSOR_SIZEWE));
    outerSizer->Add(m_border, 0, wxEXPAND);

    m_panel->SetSizer(outerSizer);
    m_panel->Hide();  // Start collapsed

    // ── Bind drag-resize events on the border handle ──────────────
    m_border->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        m_dragging = true;
        m_dragStartX = m_border->ClientToScreen(e.GetPosition()).x;
        m_dragStartWidth = m_panel->GetMinSize().x;
        m_border->CaptureMouse();
    });
    m_border->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (!m_dragging) return;

        const int screenX = m_border->ClientToScreen(e.GetPosition()).x;
        const int delta = screenX - m_dragStartX;
        const int newW = std::clamp(m_dragStartWidth + delta,
                                    MIN_WIDTH, MAX_WIDTH);

        // High-frequency mice can deliver many motion events that map to
        // the same integer width.  Avoid a redundant sizer pass in that
        // case, and lay out only the horizontal content sizer rather than
        // the complete frame (toolbar, project strip, etc.).
        if (m_panel->GetMinSize().x == newW) return;

        m_panel->SetMinSize(wxSize(newW, -1));
        if (wxSizer* containingSizer = m_panel->GetContainingSizer())
            containingSizer->Layout();
        else if (wxWindow* parent = m_panel->GetParent())
            parent->Layout();  // Defensive fallback for an unusual host.
    });
    m_border->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (!m_dragging) return;
        m_dragging = false;
        if (m_border->HasCapture()) m_border->ReleaseMouse();
        if (m_callbacks.onResized)
            m_callbacks.onResized(m_panel->GetMinSize().x);
    });
    m_border->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_dragging = false;
    });

    // ── Bind events ──────────────────────────────────────────────
    m_newChatButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_callbacks.onNewChatClicked)
            m_callbacks.onNewChatClicked();
    });

    m_newWindowButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_callbacks.onNewWindowClicked)
            m_callbacks.onNewWindowClicked();
    });

    m_archiveButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_showArchived && m_archivedCount == 0)
            return;

        m_showArchived = !m_showArchived;
        ClearSelection();
        ClearSearch();
        Refresh(m_activeFilePath);
    });

    // ── Search box events ────────────────────────────────────────
    m_searchBox->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!m_searchHintActive)
            FilterRows();
    });
    m_searchBox->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
        HideSearchHint();
        e.Skip();
    });
    m_searchBox->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
        if (m_searchBox->GetValue().empty())
            ShowSearchHint();
        e.Skip();
    });
    m_searchBox->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        const int key = e.GetKeyCode();
        if (key == WXK_ESCAPE) {
            ClearSearch();
            m_listWindow->SetFocusIgnoringChildren();
            return;
        }
        if (key == WXK_DOWN || key == WXK_UP) {
            NavigateVisibleRows(key == WXK_DOWN ? 1 : -1, false);
            return;
        }
        if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            NavigateVisibleRows(0, true);
            return;
        }
        e.Skip();
    });

    // ── Sidebar-wide keyboard navigation and actions ─────────────
    // CHAR_HOOK reaches the focused child within the sidebar.  It gives the
    // history list file-explorer-style navigation without turning every card
    // into a native button (which would change the visual design).
    m_panel->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        const int key = e.GetKeyCode();
        const bool commandDown = e.ControlDown();

        // Ctrl+F / Ctrl+K: focus the conversation search while the user's
        // keyboard focus is anywhere inside the sidebar.
        if (commandDown &&
            (key == 'F' || key == 'f' || key == 'K' || key == 'k')) {
            FocusSearchBox();
            return;
        }

        // Preserve normal text-editing keys while the search field owns focus.
        wxWindow* focused = wxWindow::FindFocus();
        if (dynamic_cast<wxTextCtrl*>(focused) ||
            dynamic_cast<wxButton*>(focused)) {
            e.Skip();
            return;
        }

        if (!e.AltDown() && !commandDown && !e.ShiftDown()) {
            if (key == WXK_UP || key == WXK_DOWN) {
                NavigateVisibleRows(key == WXK_DOWN ? 1 : -1, false);
                return;
            }
            if (key == WXK_HOME || key == WXK_END) {
                if (m_orderedPaths.empty()) {
                    wxBell();
                    return;
                }
                SelectVisiblePath(
                    key == WXK_HOME ? m_orderedPaths.front()
                                    : m_orderedPaths.back(),
                    false);
                return;
            }
            if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
                NavigateVisibleRows(0, true);
                return;
            }
            if (key == WXK_ESCAPE) {
                if (m_showArchived) {
                    m_showArchived = false;
                    ClearSelection();
                    ClearSearch();
                    Refresh(m_activeFilePath);
                }
                else if (!m_selected.empty()) {
                    ClearSelection();
                }
                else {
                    e.Skip();
                }
                return;
            }
        }

        if (key != WXK_DELETE) {
            e.Skip();
            return;
        }

        if (m_callbacks.isBusy && m_callbacks.isBusy()) {
            wxBell();
            return;
        }
        if (m_selected.empty()) {
            wxBell();
            return;
        }

        std::vector<std::string> paths(m_selected.begin(), m_selected.end());
        m_panel->CallAfter([this, paths]() {
            if (m_callbacks.onDeleteRequested)
                m_callbacks.onDeleteRequested(paths);
        });
        // Intentionally not Skip()'d — we've consumed the event.
    });
}

// ═══════════════════════════════════════════════════════════════════
//  Visibility
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::Show()
{
    m_panel->Show();
}

void ConversationSidebar::Hide()
{
    m_panel->Hide();
}

bool ConversationSidebar::IsVisible() const
{
    return m_panel->IsShown();
}

void ConversationSidebar::Toggle()
{
    if (IsVisible())
        Hide();
    else
        Show();
}

int ConversationSidebar::GetWidth() const
{
    return m_panel->GetMinSize().x;
}

void ConversationSidebar::SetWidth(int w)
{
    w = std::clamp(w, MIN_WIDTH, MAX_WIDTH);
    m_panel->SetMinSize(wxSize(w, -1));
}

// ═══════════════════════════════════════════════════════════════════
//  Content — incremental refresh instead of full rebuild
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::Refresh(const std::string& activeFilePath)
{
    if (!m_listSizer || !m_listWindow) return;

    // Incremental refreshes should not make a long history jump back to the
    // top.  Save the current scroll units and restore them after layout unless
    // the active conversation changed, in which case revealing that card is
    // the more useful behavior.
    int previousViewX = 0;
    int previousViewY = 0;
    m_listWindow->GetViewStart(&previousViewX, &previousViewY);

    const std::string previousActiveFilePath = m_activeFilePath;
    m_activeFilePath = activeFilePath;
    const bool activeChatChanged = (m_activeFilePath != previousActiveFilePath);

    // Scan all conversations, then select normal or archived mode.
    auto entries = ScanConversations();
    m_archivedCount = static_cast<size_t>(std::count_if(
        entries.begin(), entries.end(),
        [](const ConversationEntry& entry) { return entry.archived; }));

    // If the final archived conversation was restored or deleted while the
    // archive browser was open, immediately return to the normal history.
    // Leaving the user in an empty archive screen with a "0 archived" back
    // button feels broken and hides the active conversation list.
    if (m_showArchived && m_archivedCount == 0)
        m_showArchived = false;

    entries.erase(
        std::remove_if(
            entries.begin(), entries.end(),
            [this](const ConversationEntry& entry) {
                return m_showArchived ? !entry.archived : entry.archived;
            }),
        entries.end());
    UpdateArchiveButton();

    // ── Primary grouping: project / Goals / Unassigned ───────────
    // Phase 3 deliberately keeps project headers as the primary containers.
    // They own collapse state, context menus, and drag/drop destinations.
    // Time sections are nested beneath them, which adds the mockup's useful
    // Today/Yesterday hierarchy without regressing any existing project
    // workflow.
    struct Group {
        std::string id;
        std::string displayName;
        std::vector<const ConversationEntry*> entries;
    };
    std::unordered_map<std::string, Group> groupsById;

    for (const auto& e : entries) {
        std::string id;
        if (!e.projectId.empty())          id = e.projectId;
        else if (!e.goalObjective.empty()) id = kGoalsId;
        else                               id = kUnassignedId;

        auto& g = groupsById[id];
        if (g.id.empty()) {
            g.id = id;
            if (id == kUnassignedId)
                g.displayName = "Unassigned";
            else if (id == kGoalsId)
                g.displayName = "Goals";
            else
                g.displayName = e.projectName.empty()
                    ? std::string("(unnamed project)")
                    : e.projectName;
        }
        g.entries.push_back(&e);
    }

    std::vector<Group*> orderedGroups;
    orderedGroups.reserve(groupsById.size());
    for (auto& [id, g] : groupsById) {
        if (id != kUnassignedId && id != kGoalsId)
            orderedGroups.push_back(&g);
    }
    std::sort(orderedGroups.begin(), orderedGroups.end(),
        [](const Group* a, const Group* b) {
            return SidebarSearchKeyFromUtf8(a->displayName)
                 < SidebarSearchKeyFromUtf8(b->displayName);
        });
    if (auto it = groupsById.find(kGoalsId); it != groupsById.end())
        orderedGroups.push_back(&it->second);
    if (auto it = groupsById.find(kUnassignedId); it != groupsById.end())
        orderedGroups.push_back(&it->second);

    // ── Recompute active-chat auto-reveal override ───────────────
    if (activeChatChanged) {
        m_overrideExpandedGroups.clear();
        if (!m_activeFilePath.empty()) {
            for (const auto* g : orderedGroups) {
                for (const auto* e : g->entries) {
                    if (e->filePath == m_activeFilePath) {
                        if (m_collapsedGroups.count(g->id))
                            m_overrideExpandedGroups.insert(g->id);
                        break;
                    }
                }
            }
        }
    }

    m_orderedPaths.clear();
    m_orderedPaths.reserve(entries.size());

    std::set<std::string> validPaths;
    for (const auto& e : entries)
        validPaths.insert(e.filePath);

    for (auto it = m_selected.begin(); it != m_selected.end(); ) {
        if (validPaths.find(*it) == validPaths.end())
            it = m_selected.erase(it);
        else
            ++it;
    }

    m_listWindow->Freeze();

    // Remove rows that disappeared.
    for (auto it = m_rows.begin(); it != m_rows.end(); ) {
        if (validPaths.find(it->first) == validPaths.end()) {
            if (it->second.panel) {
                m_listSizer->Detach(it->second.panel);
                it->second.panel->Destroy();
            }
            it = m_rows.erase(it);
        }
        else {
            ++it;
        }
    }

    // Remove project headers for groups that disappeared.
    std::set<std::string> validGroupIds;
    for (const auto* g : orderedGroups) validGroupIds.insert(g->id);
    for (auto it = m_projectHeaders.begin(); it != m_projectHeaders.end(); ) {
        if (validGroupIds.find(it->first) == validGroupIds.end()) {
            if (it->second.panel) {
                m_listSizer->Detach(it->second.panel);
                it->second.panel->Destroy();
            }
            it = m_projectHeaders.erase(it);
        }
        else {
            ++it;
        }
    }

    static constexpr int kCardMargin = 6;
    size_t sizerIdx = 0;
    auto placeAtSizerIndex = [&](wxPanel* panel, int flags, int border) {
        wxSizerItem* currentItem =
            (sizerIdx < m_listSizer->GetItemCount())
                ? m_listSizer->GetItem(sizerIdx)
                : nullptr;
        wxWindow* currentWindow = currentItem ? currentItem->GetWindow() : nullptr;
        if (currentWindow != panel) {
            m_listSizer->Detach(panel);
            m_listSizer->Insert(sizerIdx, panel, 0, flags, border);
        }
        ++sizerIdx;
    };

    // The unused Pinned bucket is intentionally first.  Phase 4 only needs
    // to persist/toggle ConversationEntry::pinned; the layout and search
    // plumbing are already prepared here.
    static const std::array<std::string, 5> kDateBucketOrder = {
        "pinned", "today", "yesterday", "previous7", "older"
    };

    std::set<std::string> validDateHeaderKeys;

    for (const auto* g : orderedGroups) {
        const bool collapsed = IsGroupCollapsed(g->id);
        const int chatCount = static_cast<int>(g->entries.size());

        auto headerIt = m_projectHeaders.find(g->id);
        if (headerIt == m_projectHeaders.end()) {
            HeaderWidgets header = CreateProjectHeader(
                g->id, g->displayName, chatCount, collapsed);
            auto [insertedIt, _] =
                m_projectHeaders.emplace(g->id, std::move(header));
            headerIt = insertedIt;
        }
        else {
            UpdateProjectHeader(headerIt->second, g->displayName,
                                chatCount, collapsed);
        }
        if (headerIt->second.panel) {
            headerIt->second.panel->Show();
            placeAtSizerIndex(headerIt->second.panel, wxEXPAND, 0);
        }

        std::unordered_map<std::string,
                           std::vector<const ConversationEntry*>> byDateBucket;
        for (const auto* e : g->entries) {
            byDateBucket[DateBucketIdFor(e->modTime, e->pinned,
                                         e->hasActivityTime)].push_back(e);
        }

        for (const auto& bucketId : kDateBucketOrder) {
            auto bucketIt = byDateBucket.find(bucketId);
            if (bucketIt == byDateBucket.end() || bucketIt->second.empty())
                continue;

            const std::string dateHeaderKey =
                MakeDateHeaderKey(g->id, bucketId);
            validDateHeaderKeys.insert(dateHeaderKey);

            auto dateHeaderIt = m_dateHeaders.find(dateHeaderKey);
            if (dateHeaderIt == m_dateHeaders.end()) {
                DateHeaderWidgets header = CreateDateHeader(
                    g->id, bucketId, DateBucketLabel(bucketId));
                auto [insertedIt, _] =
                    m_dateHeaders.emplace(dateHeaderKey, std::move(header));
                dateHeaderIt = insertedIt;
            }
            else {
                UpdateDateHeader(dateHeaderIt->second,
                                 DateBucketLabel(bucketId));
            }

            if (dateHeaderIt->second.panel) {
                if (collapsed) {
                    dateHeaderIt->second.panel->Hide();
                    if (m_listSizer->GetItem(dateHeaderIt->second.panel))
                        m_listSizer->Detach(dateHeaderIt->second.panel);
                }
                else {
                    dateHeaderIt->second.panel->Show();
                    placeAtSizerIndex(
                        dateHeaderIt->second.panel,
                        wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                        kCardMargin);
                }
            }

            for (const auto* e : bucketIt->second) {
                const auto& entry = *e;

                auto rowIt = m_rows.find(entry.filePath);
                if (rowIt == m_rows.end()) {
                    RowWidgets row = CreateRow(entry);
                    auto [insertedIt, _] =
                        m_rows.emplace(entry.filePath, std::move(row));
                    rowIt = insertedIt;
                }
                else {
                    UpdateRow(rowIt->second, entry);
                }
                rowIt->second.groupId = g->id;
                rowIt->second.dateBucketId = bucketId;

                wxPanel* panel = rowIt->second.panel;
                if (!panel) continue;

                if (collapsed) {
                    panel->Hide();
                    if (m_listSizer->GetItem(panel))
                        m_listSizer->Detach(panel);
                    continue;
                }

                panel->Show();
                placeAtSizerIndex(panel,
                                  wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                                  kCardMargin);
                m_orderedPaths.push_back(entry.filePath);

                const wxColour rowBg = GetRowBackground(entry.filePath);
                if (panel->GetBackgroundColour() != rowBg)
                    ApplyRowAppearance(rowIt->second, false);
            }
        }
    }

    // Remove date headers for buckets that disappeared or moved to another
    // bucket after an mtime change.
    for (auto it = m_dateHeaders.begin(); it != m_dateHeaders.end(); ) {
        if (validDateHeaderKeys.find(it->first) == validDateHeaderKeys.end()) {
            if (it->second.panel) {
                m_listSizer->Detach(it->second.panel);
                it->second.panel->Destroy();
            }
            it = m_dateHeaders.erase(it);
        }
        else {
            ++it;
        }
    }

    m_listWindow->FitInside();
    m_listWindow->Layout();
    m_listWindow->Thaw();

    if (!m_searchFilter.empty())
        FilterRows();

    if (activeChatChanged && !m_activeFilePath.empty()) {
        const std::string activePath = m_activeFilePath;
        m_listWindow->CallAfter([this, activePath]() {
            ScrollPathIntoView(activePath);
        });
    }
    else if (m_searchFilter.empty()) {
        m_listWindow->CallAfter([this, previousViewX, previousViewY]() {
            if (m_listWindow)
                m_listWindow->Scroll(previousViewX, previousViewY);
        });
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Selection
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::ClearSelection()
{
    m_selected.clear();
    m_anchorPath.clear();
    RefreshAllRowBackgrounds();
}

bool ConversationSidebar::AreAllPinned(
    const std::vector<std::string>& paths) const
{
    if (paths.empty())
        return false;

    for (const auto& path : paths) {
        auto rowIt = m_rows.find(path);
        if (rowIt != m_rows.end()) {
            if (!rowIt->second.pinned)
                return false;
            continue;
        }

        auto cacheIt = m_metaCache.find(path);
        if (cacheIt == m_metaCache.end() || !cacheIt->second.pinned)
            return false;
    }
    return true;
}

bool ConversationSidebar::IsSelected(const std::string& path) const
{
    return m_selected.find(path) != m_selected.end();
}

void ConversationSidebar::SelectRange(const std::string& from,
                                      const std::string& to)
{
    int idxFrom = -1, idxTo = -1;
    for (int i = 0; i < (int)m_orderedPaths.size(); ++i) {
        if (m_orderedPaths[i] == from) idxFrom = i;
        if (m_orderedPaths[i] == to)   idxTo   = i;
    }

    if (idxFrom < 0 || idxTo < 0) return;

    int lo = std::min(idxFrom, idxTo);
    int hi = std::max(idxFrom, idxTo);

    m_selected.clear();
    for (int i = lo; i <= hi; ++i)
        m_selected.insert(m_orderedPaths[i]);
}


void ConversationSidebar::RebuildOrderedPathsFromVisibleRows()
{
    m_orderedPaths.clear();

    if (!m_listSizer || !m_listWindow)
        return;

    for (size_t i = 0; i < m_listSizer->GetItemCount(); ++i) {
        wxSizerItem* item = m_listSizer->GetItem(i);
        wxWindow* win = item ? item->GetWindow() : nullptr;
        if (!win || !win->IsShown())
            continue;

        std::string path = PathFromWidget(win, m_listWindow);
        if (!path.empty())
            m_orderedPaths.push_back(path);
    }
}

void ConversationSidebar::SelectVisiblePath(
    const std::string& path, bool openConversation)
{
    if (path.empty() ||
        std::find(m_orderedPaths.begin(), m_orderedPaths.end(), path) ==
            m_orderedPaths.end()) {
        wxBell();
        return;
    }

    m_selected.clear();
    m_selected.insert(path);
    m_anchorPath = path;
    RefreshAllRowBackgrounds();
    ScrollPathIntoView(path);
    m_listWindow->SetFocusIgnoringChildren();

    if (openConversation && m_callbacks.onConversationClicked) {
        m_listWindow->CallAfter([this, path]() {
            if (m_callbacks.onConversationClicked)
                m_callbacks.onConversationClicked(path);
        });
    }
}

void ConversationSidebar::NavigateVisibleRows(
    int delta, bool openConversation)
{
    if (m_orderedPaths.empty()) {
        wxBell();
        return;
    }

    int currentIndex = -1;

    // Prefer the selected card, then the active conversation.  Iterating in
    // visual order makes multi-selection navigation deterministic.
    for (size_t i = 0; i < m_orderedPaths.size(); ++i) {
        if (IsSelected(m_orderedPaths[i])) {
            currentIndex = static_cast<int>(i);
            break;
        }
    }
    if (currentIndex < 0 && !m_activeFilePath.empty()) {
        auto activeIt = std::find(
            m_orderedPaths.begin(), m_orderedPaths.end(), m_activeFilePath);
        if (activeIt != m_orderedPaths.end())
            currentIndex = static_cast<int>(
                std::distance(m_orderedPaths.begin(), activeIt));
    }

    int targetIndex = currentIndex;
    if (delta == 0) {
        if (targetIndex < 0) targetIndex = 0;
    }
    else if (targetIndex < 0) {
        targetIndex = delta > 0
            ? 0
            : static_cast<int>(m_orderedPaths.size()) - 1;
    }
    else {
        targetIndex = std::clamp(
            targetIndex + delta, 0,
            static_cast<int>(m_orderedPaths.size()) - 1);
    }

    SelectVisiblePath(m_orderedPaths[targetIndex], openConversation);
}

void ConversationSidebar::ScrollPathIntoView(const std::string& path)
{
    auto rowIt = m_rows.find(path);
    if (rowIt == m_rows.end() || !rowIt->second.panel ||
        !rowIt->second.panel->IsShown()) {
        return;
    }

    int pixelsPerUnitX = 0;
    int pixelsPerUnitY = 0;
    m_listWindow->GetScrollPixelsPerUnit(
        &pixelsPerUnitX, &pixelsPerUnitY);
    if (pixelsPerUnitY <= 0)
        return;

    int viewX = 0;
    int viewY = 0;
    m_listWindow->GetViewStart(&viewX, &viewY);

    const int viewportTop = viewY * pixelsPerUnitY;
    const int viewportBottom =
        viewportTop + m_listWindow->GetClientSize().GetHeight();
    const int rowTop = rowIt->second.panel->GetPosition().y;
    const int rowBottom = rowTop + rowIt->second.panel->GetSize().GetHeight();

    int targetViewY = viewY;
    if (rowTop < viewportTop) {
        targetViewY = std::max(0, rowTop / pixelsPerUnitY);
    }
    else if (rowBottom > viewportBottom) {
        const int targetTopPixels = std::max(
            0, rowBottom - m_listWindow->GetClientSize().GetHeight());
        targetViewY =
            (targetTopPixels + pixelsPerUnitY - 1) / pixelsPerUnitY;
    }

    if (targetViewY != viewY)
        m_listWindow->Scroll(viewX, targetViewY);
}

// ═══════════════════════════════════════════════════════════════════
//  Theming
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::ApplyTheme(const ThemeData& theme)
{
    m_theme = &theme;

    m_panel->SetBackgroundColour(theme.bgSidebar);
    m_content->SetBackgroundColour(theme.bgSidebar);
    m_newChatButton->SetBackgroundColour(
        SidebarPrimaryButtonBackground(theme));
    m_newChatButton->SetForegroundColour(wxColour(255, 255, 255));
    m_newWindowButton->SetBackgroundColour(theme.bgDialogSurface);
    m_newWindowButton->SetForegroundColour(theme.textPrimary);
    m_searchBox->SetBackgroundColour(SidebarSearchBackground(theme));
    m_searchBox->SetForegroundColour(
        m_searchHintActive ? theme.textMuted : theme.textPrimary);
    m_listWindow->SetBackgroundColour(theme.bgSidebar);
    m_archiveButton->SetBackgroundColour(theme.bgDialogSurface);
    m_archiveButton->SetForegroundColour(theme.textMuted);
    m_border->SetBackgroundColour(theme.borderSubtle);

    // Cached rows do not get rebuilt on theme changes, so recolor every
    // child surface and then re-apply the active/selected visual state.
    for (auto& [path, row] : m_rows) {
        if (row.titleLabel)
            row.titleLabel->SetForegroundColour(theme.textPrimary);
        if (row.timeLabel)
            row.timeLabel->SetForegroundColour(theme.textMuted);
        // The custom-painted action dots take both background and foreground
        // from ApplyRowAppearance below, including whether the dots should be
        // painted for the current hover/active/selected state.
        ApplyRowAppearance(row, false);
    }

    // Recolor cached project headers — same lifetime story as rows.
    for (auto& [id, header] : m_projectHeaders) {
        if (header.panel)
            header.panel->SetBackgroundColour(theme.bgSidebar);
        if (header.triangle)
            header.triangle->SetForegroundColour(theme.textMuted);
        if (header.nameLabel)
            header.nameLabel->SetForegroundColour(theme.textPrimary);
        if (header.countBadge) {
            auto* badge =
                static_cast<SidebarCountBadge*>(header.countBadge);
            badge->SetPalette(
                MixSidebarColour(theme.attachChipBg,
                                 theme.accentButton, 14),
                theme.textPrimary);
        }
    }

    for (auto& [key, header] : m_dateHeaders) {
        if (header.panel)
            header.panel->SetBackgroundColour(theme.bgSidebar);
        if (header.label) {
            header.label->SetBackgroundColour(theme.bgSidebar);
            header.label->SetForegroundColour(theme.textMuted);
        }
    }

    RefreshAllRowBackgrounds();
    m_listWindow->Refresh();
}

// ═══════════════════════════════════════════════════════════════════
//  Row appearance logic
// ═══════════════════════════════════════════════════════════════════

wxColour ConversationSidebar::GetRowBackground(const std::string& filePath) const
{
    const wxColour base = SidebarCardBackground(*m_theme);

    // Active chat: tint the entire card with the current theme's primary
    // action color.  The thin accent bar remains the strongest edge cue, but
    // the full-width band now reads immediately as the open conversation.
    // Multi-selection uses a related, slightly softer treatment so it remains
    // distinguishable from the single active chat.
    if (filePath == m_activeFilePath)
        return MixSidebarColour(
            base, SidebarPrimaryButtonBackground(*m_theme), 38);
    if (IsSelected(filePath))
        return MixSidebarColour(base, m_theme->accentButton, 30);
    return base;
}

void ConversationSidebar::ApplyRowAppearance(RowWidgets& row, bool hovered)
{
    if (!row.panel) return;

    const bool active   = (row.filePath == m_activeFilePath);
    const bool selected = IsSelected(row.filePath);

    wxColour cardBg = GetRowBackground(row.filePath);
    if (hovered && !active && !selected) {
        cardBg = MixSidebarColour(
            SidebarCardBackground(*m_theme),
            m_theme->sidebarHover, 62);
    }

    row.panel->SetBackgroundColour(cardBg);

    if (row.accentBar) {
        row.accentBar->SetBackgroundColour(
            (active || selected) ? m_theme->accentButton : cardBg);
    }

    const wxColour projectAccent = SidebarProjectAccent(
        *m_theme, row.projectId, row.hasGoal);
    const bool neutral = row.projectId.empty() && !row.hasGoal;

    // Assigned projects receive stable, distinct icon colors.  Unassigned
    // rows stay neutral so color always carries meaning rather than becoming
    // decoration.  Active/selected state still strengthens either treatment.
    const int iconWeight = neutral
        ? ((active || selected) ? 34 : (hovered ? 22 : 15))
        : ((active || selected) ? 50 : (hovered ? 34 : 26));
    const wxColour iconBg = MixSidebarColour(cardBg, projectAccent, iconWeight);
    const wxColour iconFg = (active || selected)
        ? wxColour(255, 255, 255)
        : (neutral
            ? m_theme->textMuted
            : MixSidebarColour(projectAccent, m_theme->textPrimary, 16));

    if (row.iconPanel)
        row.iconPanel->SetBackgroundColour(iconBg);
    if (row.iconLabel) {
        row.iconLabel->SetBackgroundColour(iconBg);
        row.iconLabel->SetForegroundColour(iconFg);
    }

    if (row.titleLabel) {
        row.titleLabel->SetBackgroundColour(cardBg);
        row.titleLabel->SetForegroundColour(m_theme->textPrimary);
    }
    if (row.timeLabel) {
        row.timeLabel->SetBackgroundColour(cardBg);
        row.timeLabel->SetForegroundColour(
            (active || selected)
                ? MixSidebarColour(m_theme->textMuted,
                                   m_theme->textPrimary, 34)
                : m_theme->textMuted);
    }
    if (row.projectTag) {
        const wxColour tagFill = MixSidebarColour(
            cardBg, projectAccent, (active || selected) ? 38 : 29);
        const wxColour tagText = neutral
            ? m_theme->textMuted
            : MixSidebarColour(projectAccent, m_theme->textPrimary, 28);
        static_cast<SidebarProjectTag*>(row.projectTag)->SetPalette(
            tagFill, tagText);
        row.projectTag->Refresh();
    }
    if (row.menuBtn) {
        // Keep the fixed action slot in the layout at all times, but paint the
        // dots only for the active/selected/hovered row.  This avoids visual
        // clutter without reintroducing the Windows Show()/Hide() layout and
        // repaint problems that originally made the dots unreliable.
        const bool showActions = hovered || active || selected;
        row.menuBtn->SetBackgroundColour(cardBg);
        row.menuBtn->SetForegroundColour(
            showActions ? m_theme->textPrimary : cardBg);
        if (!row.menuBtn->IsShown())
            row.menuBtn->Show();
    }

    row.panel->Refresh();
    if (row.accentBar) row.accentBar->Refresh();
    if (row.iconPanel) row.iconPanel->Refresh();
    if (row.menuBtn) row.menuBtn->Refresh();
}

void ConversationSidebar::RefreshAllRowBackgrounds()
{
    for (auto& [path, row] : m_rows) {
        ApplyRowAppearance(row, false);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Search / Filter
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::FilterRows()
{
    wxString raw = m_searchHintActive
        ? wxString()
        : m_searchBox->GetValue();
    std::string newFilter = WxToUtf8String(raw.Lower());
    const bool wasFiltered = !m_searchFilter.empty();
    const bool nowFiltered = !newFilter.empty();
    const bool transition  = (wasFiltered != nowFiltered);

    m_searchFilter = newFilter;

    if (transition) {
        Refresh(m_activeFilePath);
        return;
    }

    m_listWindow->Freeze();

    std::set<std::string> groupsWithMatches;
    std::set<std::string> dateSectionsWithMatches;

    for (auto& [path, row] : m_rows) {
        if (!row.panel) continue;

        bool show = true;
        if (nowFiltered) {
            show = (row.searchTitleLower.find(m_searchFilter)
                    != std::string::npos);
        }

        row.panel->Show(show);
        if (show && !row.groupId.empty()) {
            groupsWithMatches.insert(row.groupId);
            if (!row.dateBucketId.empty()) {
                dateSectionsWithMatches.insert(
                    MakeDateHeaderKey(row.groupId, row.dateBucketId));
            }
        }
    }

    for (auto& [id, header] : m_projectHeaders) {
        if (!header.panel) continue;
        header.panel->Show(!nowFiltered || groupsWithMatches.count(id) != 0);
    }

    for (auto& [key, header] : m_dateHeaders) {
        if (!header.panel) continue;
        if (nowFiltered) {
            header.panel->Show(dateSectionsWithMatches.count(key) != 0);
        }
        else {
            header.panel->Show(!IsGroupCollapsed(header.groupId));
        }
    }

    RebuildOrderedPathsFromVisibleRows();

    m_listWindow->FitInside();
    m_listWindow->Layout();
    m_listWindow->Thaw();

    // A filtered result set should begin at its first match rather than
    // preserving a scroll offset from the unfiltered history.
    if (nowFiltered)
        m_listWindow->Scroll(0, 0);
}

void ConversationSidebar::UpdateArchiveButton()
{
    if (!m_archiveButton)
        return;

    if (m_showArchived) {
        m_archiveButton->Show(true);
        m_archiveButton->SetLabel(
            wxString::Format("< Back to conversations   (%zu archived)",
                             m_archivedCount));
        m_archiveButton->Enable(true);
        m_archiveButton->SetToolTip(
            "Return to the normal conversation history. Press Esc to go back.");
    }
    else if (m_archivedCount > 0) {
        m_archiveButton->Show(true);
        m_archiveButton->SetLabel(
            wxString::Format("Archived   %zu", m_archivedCount));
        m_archiveButton->Enable(true);
        m_archiveButton->SetToolTip(
            "Browse archived conversations.");
    }
    else {
        // A permanently disabled "Archived 0" footer looked like an empty
        // toolbar and consumed useful height.  Hide it until it has work to do.
        m_archiveButton->Enable(false);
        m_archiveButton->Show(false);
    }

    if (wxSizer* sizer = m_archiveButton->GetContainingSizer())
        sizer->Layout();
}

void ConversationSidebar::ShowSearchHint()
{
    if (!m_searchBox || m_searchBox->HasFocus() ||
        !m_searchBox->GetValue().empty()) {
        return;
    }

    m_searchHintActive = true;
    m_searchBox->ChangeValue("Search conversations...");
    if (m_theme)
        m_searchBox->SetForegroundColour(m_theme->textMuted);
}

void ConversationSidebar::HideSearchHint()
{
    if (!m_searchBox || !m_searchHintActive)
        return;

    m_searchHintActive = false;
    m_searchBox->ChangeValue(wxEmptyString);
    if (m_theme)
        m_searchBox->SetForegroundColour(m_theme->textPrimary);
}

void ConversationSidebar::ClearSearch()
{
    const bool hadFilter = !m_searchFilter.empty();

    m_searchHintActive = false;
    m_searchBox->ChangeValue(wxEmptyString);
    if (m_theme)
        m_searchBox->SetForegroundColour(m_theme->textPrimary);

    if (hadFilter) {
        m_searchFilter.clear();
        Refresh(m_activeFilePath);
    }

    if (!m_searchBox->HasFocus())
        ShowSearchHint();
}

void ConversationSidebar::FocusSearchBox()
{
    if (!m_searchBox)
        return;

    HideSearchHint();
    m_searchBox->SetFocus();
    m_searchBox->SelectAll();
}

// ═══════════════════════════════════════════════════════════════════
//  Static helper — resolve a child widget click to a file path
// ═══════════════════════════════════════════════════════════════════

std::string ConversationSidebar::PathFromWidget(wxWindow* win, wxWindow* stop)
{
    while (win && win != stop) {
        wxString name = win->GetName();
        if (name.EndsWith(".json"))
            return name.ToUTF8().data();
        win = win->GetParent();
    }
    return {};
}

void ConversationSidebar::InvalidateMetadata(
    const std::vector<std::string>& paths)
{
    for (const auto& path : paths) {
        if (!path.empty())
            m_metaCache.erase(path);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Internal — scan conversation files from disk
// ═══════════════════════════════════════════════════════════════════

std::vector<ConversationSidebar::ConversationEntry>
ConversationSidebar::ScanConversations()
{
    std::vector<ConversationEntry> entries;

    std::string convDir = ChatHistory::GetConversationsDir();
    wxDir dir(wxString::FromUTF8(convDir));
    if (!dir.IsOpened()) return entries;

    wxString filename;
    bool found = dir.GetFirst(&filename, "*.json", wxDIR_FILES);
    while (found) {
        wxString fullPath = wxString::FromUTF8(convDir) +
            wxFileName::GetPathSeparator() + filename;

        ConversationEntry entry;
        entry.filePath = fullPath.ToUTF8().data();

        // ── Cheap stat ───────────────────────────────────────────
        // Cache key uses millisecond-resolution mtime (NTFS file times are
        // sub-second) plus the file size.  GetTicks() was seconds-only, so a
        // second save inside the same wall-clock second produced an identical
        // key and the sidebar reused stale title/project metadata.  The size
        // is a belt-and-suspenders change check that holds even where the
        // filesystem mtime can't resolve sub-second.
        //
        // On MSW this used to be wxFileName::GetTimes() + GetSize() — each
        // opens a file handle (CreateFile), so every sidebar refresh paid
        // TWO handle opens per conversation, and Refresh runs once per
        // completed turn (AutoSaveConversation → Refresh).  With hundreds
        // of conversations plus Defender in the open path, that stacked a
        // few ms onto the same end-of-turn moment as the autosave itself.
        // GetFileAttributesExW returns size + last-write time in a single
        // metadata query with no handle open.
        long long          thisMtimeMs = 0;
        unsigned long long thisSize    = 0;
#ifdef __WXMSW__
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            const std::wstring wPath = path_safety::Utf8ToWide(entry.filePath);
            if (!wPath.empty() &&
                ::GetFileAttributesExW(wPath.c_str(), GetFileExInfoStandard, &fad)) {
                // FILETIME (100ns ticks since 1601 UTC) → ms since Unix epoch.
                ULARGE_INTEGER uli;
                uli.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
                uli.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                if (uli.QuadPart >= 116444736000000000ULL) {
                    thisMtimeMs = (long long)((uli.QuadPart
                                   - 116444736000000000ULL) / 10000ULL);
                }
                thisSize = ((unsigned long long)fad.nFileSizeHigh << 32)
                           | fad.nFileSizeLow;

                // entry.modTime is only used for the newest-first sort and
                // must agree with the cache key.  Build it from the same ms
                // value: wxDateTime(time_t) + SetMillisecond reproduces
                // exactly GetValue() == thisMtimeMs.
                entry.modTime = wxDateTime((time_t)(thisMtimeMs / 1000));
                entry.modTime.SetMillisecond(
                    (unsigned short)(thisMtimeMs % 1000));
            }
        }
#else
        {
            wxFileName fn(fullPath);
            fn.GetTimes(nullptr, &entry.modTime, nullptr);
            thisMtimeMs =
                entry.modTime.IsValid() ? entry.modTime.GetValue().GetValue() : 0;
            thisSize = fn.GetSize().GetValue();
        }
#endif
        // A file deleted between enumeration and the stat leaves modTime
        // invalid; wxDateTime comparison asserts on invalid operands in
        // debug builds.  Pin it to the epoch so the entry just sorts last.
        if (!entry.modTime.IsValid())
            entry.modTime = wxDateTime((time_t)0);

        // ── Cache lookup ─────────────────────────────────────────
        // If we've seen this path before and its mtime hasn't moved,
        // reuse the cached metadata.  The JSON file hasn't been
        // rewritten so title and project association are unchanged.
        auto cachedIt = m_metaCache.find(entry.filePath);
        bool cacheHit = (cachedIt != m_metaCache.end() &&
                         cachedIt->second.mtimeMs == thisMtimeMs &&
                         cachedIt->second.size    == thisSize &&
                         thisMtimeMs != 0);

        if (cacheHit) {
            entry.title         = cachedIt->second.title;
            entry.projectId     = cachedIt->second.projectId;
            entry.projectName   = cachedIt->second.projectName;
            entry.goalObjective = cachedIt->second.goalObjective;
            entry.pinned        = cachedIt->second.pinned;
            entry.archived      = cachedIt->second.archived;
            if (cachedIt->second.activityTimeMs > 0) {
                entry.modTime = wxDateTime(
                    static_cast<time_t>(cachedIt->second.activityTimeMs / 1000));
                entry.modTime.SetMillisecond(static_cast<unsigned short>(
                    cachedIt->second.activityTimeMs % 1000));
                // activityTimeMs is only stored when updated_at parsed, so a
                // positive value is exactly the "has real activity clock"
                // signal the date bucketing needs.
                entry.hasActivityTime = true;
            }
        }
        else {
            // Cache miss (new file, or rewritten since we last looked).
            // Pull title + project_id + project_name out of the JSON by
            // scanning lines for each key.  preserveInsertOrder=true in
            // SaveToFile keeps these near the top, so this typically
            // only reads the first handful of lines before exiting.
            //
            // We exit early once all three fields have been seen, OR
            // the moment we hit a "messages" line — past that point the
            // metadata fields can't appear anymore and we'd just be
            // reading the body for nothing.
            try {
                std::ifstream file(path_safety::Utf8ToWide(entry.filePath), std::ios::in);
                if (file.is_open()) {
                    bool sawTitle = false, sawProjectId = false, sawProjectName = false;
                    bool sawObjective = false, sawUpdatedAt = false;
                    auto extractStringField = [](const std::string& line,
                                                 const std::string& key,
                                                 std::string& out) -> bool {
                        std::string needle = "\"" + key + "\"";
                        size_t keyPos = line.find(needle);
                        if (keyPos == std::string::npos) return false;

                        // Only accept real top-level keys emitted by Poco's
                        // pretty-printer.  Escaped text inside user-controlled
                        // strings can contain "project_id" etc., but those
                        // occurrences are not the first token on the line.
                        size_t firstToken = line.find_first_not_of(" \t");
                        if (firstToken == std::string::npos || firstToken != keyPos)
                            return false;

                        size_t colonPos = line.find(':', keyPos + needle.size());
                        if (colonPos == std::string::npos) return false;
                        size_t valuePos = colonPos + 1;
                        while (valuePos < line.size() &&
                               std::isspace(static_cast<unsigned char>(line[valuePos]))) {
                            ++valuePos;
                        }
                        // Only accept actual JSON string values.  Without
                        // this guard, a malformed one-line fragment such as
                        // "project_id": null, "project_name": "Foo"
                        // could skip past null and accidentally grab the
                        // next key name/value.
                        if (valuePos >= line.size() || line[valuePos] != '"')
                            return false;

                        size_t openQuote = valuePos;
                        size_t end = openQuote + 1;
                        while (end < line.size()) {
                            if (line[end] == '"') {
                                // Count consecutive backslashes immediately
                                // preceding this candidate terminator.  An
                                // EVEN count (including zero) means the
                                // quote is unescaped and really is the
                                // closing quote.  An ODD count means the
                                // immediately-preceding backslash escapes
                                // the quote, which is then string data.
                                //
                                // Naive `line[end - 1] != '\\'` mishandles
                                // values like "C:\\path\\" -- the closing
                                // quote is preceded by a single backslash,
                                // but that backslash is the second half of
                                // a `\\` pair encoding a literal backslash
                                // in the value, so the quote really does
                                // terminate.  We have to look further back.
                                size_t backslashes = 0;
                                size_t k = end;
                                while (k > openQuote + 1 &&
                                       line[k - 1] == '\\') {
                                    ++backslashes;
                                    --k;
                                }
                                if ((backslashes % 2) == 0) break;
                            }
                            ++end;
                        }
                        if (end >= line.size()) return false;
                        out = line.substr(openQuote + 1, end - openQuote - 1);
                        out = JsonUnescapeForLabel(out);
                        return true;
                    };

                    auto extractBoolField = [](const std::string& line,
                                               const std::string& key,
                                               bool& out) -> bool {
                        const std::string needle = "\"" + key + "\"";
                        const size_t keyPos = line.find(needle);
                        if (keyPos == std::string::npos) return false;

                        const size_t firstToken = line.find_first_not_of(" \t");
                        if (firstToken == std::string::npos || firstToken != keyPos)
                            return false;

                        const size_t colonPos =
                            line.find(':', keyPos + needle.size());
                        if (colonPos == std::string::npos) return false;

                        size_t valuePos = colonPos + 1;
                        while (valuePos < line.size() &&
                               std::isspace(
                                   static_cast<unsigned char>(line[valuePos]))) {
                            ++valuePos;
                        }

                        if (line.compare(valuePos, 4, "true") == 0) {
                            out = true;
                            return true;
                        }
                        if (line.compare(valuePos, 5, "false") == 0) {
                            out = false;
                            return true;
                        }
                        return false;
                    };

                    std::string line;
                    bool scanningMessagesForLegacyTitle = false;
                    bool nextContentBelongsToUser = false;

                    while (std::getline(file, line)) {
                        if (!scanningMessagesForLegacyTitle &&
                            line.find("\"messages\"") != std::string::npos) {
                            // Normal files stop at the message array exactly as
                            // before.  Legacy session-context titles get one
                            // narrow fallback scan for the first user message.
                            if (!IsLegacySessionContextTitle(entry.title)) break;
                            scanningMessagesForLegacyTitle = true;
                            continue;
                        }

                        if (scanningMessagesForLegacyTitle) {
                            std::string role;
                            if (extractStringField(line, "role", role)) {
                                nextContentBelongsToUser = (role == "user");
                                continue;
                            }

                            if (nextContentBelongsToUser) {
                                std::string storedContent;
                                if (extractStringField(line, "content", storedContent)) {
                                    std::string repaired =
                                        MeaningfulTitleFromStoredUserContent(
                                            std::move(storedContent));
                                    if (!repaired.empty()) entry.title = std::move(repaired);
                                    break;
                                }
                            }
                            continue;
                        }

                        if (!sawTitle && extractStringField(line, "title", entry.title))
                            sawTitle = true;
                        if (!sawProjectId && extractStringField(line, "project_id", entry.projectId))
                            sawProjectId = true;
                        if (!sawProjectName && extractStringField(line, "project_name", entry.projectName))
                            sawProjectName = true;
                        // "objective" lives one level down inside the "goal"
                        // object, which SaveToFile writes before "messages".
                        // extractStringField keys off the first token on the
                        // line, so the indented "objective": line still
                        // matches, and the key is unique (the contract block
                        // has no "objective"), so there's no ambiguity.
                        if (!sawObjective && extractStringField(line, "objective", entry.goalObjective))
                            sawObjective = true;
                        if (!sawUpdatedAt) {
                            std::string updatedAt;
                            if (extractStringField(line, "updated_at", updatedAt)) {
                                wxDateTime activity;
                                if (ParseConversationActivityTime(updatedAt, activity)) {
                                    entry.modTime = activity;
                                    entry.hasActivityTime = true;
                                }
                                sawUpdatedAt = true;
                            }
                        }

                        // Optional top-level sidebar metadata.  Missing keys
                        // retain the false defaults used by legacy files.
                        // pinned/archived are written sparsely (false values
                        // omitted) and always BEFORE the messages array, so
                        // the messages terminator below guarantees they are
                        // read whenever present — regardless of field order.
                        extractBoolField(line, "pinned", entry.pinned);
                        extractBoolField(line, "archived", entry.archived);

                        // Definitive terminator: "messages" is always the last
                        // top-level key SaveToFile writes, after every field the
                        // sidebar reads (title/project/pinned/archived/updated_at
                        // and the goal object holding objective).  Stopping here
                        // is order-independent — it does not depend on a goal's
                        // "objective" existing, so it also avoids scanning the
                        // whole (large) message body for goal-less chats — and it
                        // cannot skip a pinned/archived flag no matter how the
                        // metadata keys in chat_history.cpp are later reordered.
                        {
                            size_t firstToken =
                                line.find_first_not_of(" \t");
                            if (firstToken != std::string::npos &&
                                line.compare(firstToken, 10, "\"messages\"") == 0) {
                                break;
                            }
                        }

                        // Fast path for the common goal-bearing chat: once every
                        // required field is in hand we can stop early rather than
                        // waiting for the messages line.  Goal-less chats fall
                        // through to the messages terminator above.
                        if (sawTitle && sawProjectId && sawProjectName &&
                            sawObjective && sawUpdatedAt &&
                            !IsLegacySessionContextTitle(entry.title)) {
                            break;
                        }
                    }
                }
            }
            catch (...) {
                // Skip files we can't read
            }

            if (entry.title.empty()) {
                entry.title = filename.ToUTF8().data();
            }

            // Populate cache for next refresh.  activityTimeMs is stored
            // ONLY when updated_at actually parsed — otherwise entry.modTime
            // still holds the filesystem mtime, and writing that here would
            // make the field's ">0 means real activity clock" contract lie,
            // re-seeding mtime into date bucketing on the next cache hit
            // (the exact bug this fix closes).
            m_metaCache[entry.filePath] = {
                entry.title, entry.projectId, entry.projectName,
                entry.goalObjective, entry.pinned, entry.archived,
                (entry.hasActivityTime && entry.modTime.IsValid())
                    ? entry.modTime.GetValue().GetValue()
                    : 0,
                thisMtimeMs, thisSize
            };
        }

        entries.push_back(entry);
        found = dir.GetNext(&filename);
    }

    // Prune cache entries for files that no longer exist. Not strictly
    // required (stale entries cost ~200 bytes each) but keeps the cache
    // bounded to what's actually on disk.
    if (m_metaCache.size() > entries.size()) {
        std::set<std::string> currentPaths;
        for (const auto& e : entries)
            currentPaths.insert(e.filePath);
        for (auto it = m_metaCache.begin(); it != m_metaCache.end(); ) {
            if (currentPaths.find(it->first) == currentPaths.end())
                it = m_metaCache.erase(it);
            else
                ++it;
        }
    }

    std::sort(entries.begin(), entries.end(),
        [](const ConversationEntry& a, const ConversationEntry& b) {
            return a.modTime.IsLaterThan(b.modTime);
        });

    return entries;
}

// ═══════════════════════════════════════════════════════════════════
//  Project header — CRUD + collapse handling
// ═══════════════════════════════════════════════════════════════════

namespace {
// Unicode geometric shapes used as collapse indicators.  Matches the
// project status strip's monospace aesthetic; both glyphs are present
// in Consolas, Cascadia Mono, DejaVu, and Segoe UI Symbol so we don't
// need to ship a fallback font.
constexpr const char* kTriangleExpanded  = "\xE2\x96\xBC";  // ▼
constexpr const char* kTriangleCollapsed = "\xE2\x96\xB6";  // ▶

// Squared distance threshold to consider a press-and-move a drag.
// 5 px ⇒ 25 px².  Matches Win32 SM_CXDRAG/SM_CYDRAG defaults closely
// enough that drags feel native without the extra GetSystemMetrics
// plumbing.
constexpr int kDragThresholdSq = 25;

// ── Custom DnD payload format ────────────────────────────────────
// Private clipboard format so external apps can't accidentally
// accept these drops, and so we can't accidentally accept anything
// they generate.  Payload is the chat file paths joined by '\n'
// (conversation paths are generated by us under
// ChatHistory::GetConversationsDir() and never contain newlines).
const wxDataFormat& SidebarChatsFormat()
{
    static wxDataFormat fmt(wxString("application/x-llamaboss-chats"));
    return fmt;
}

class ChatPathsDataObject : public wxCustomDataObject
{
public:
    ChatPathsDataObject() : wxCustomDataObject(SidebarChatsFormat()) {}

    void SetPaths(const std::vector<std::string>& paths)
    {
        std::string joined;
        for (size_t i = 0; i < paths.size(); ++i) {
            if (i > 0) joined += '\n';
            joined += paths[i];
        }
        SetData(joined.size(), joined.data());
    }

    std::vector<std::string> GetPaths() const
    {
        std::vector<std::string> out;
        const size_t n = GetSize();
        if (n == 0) return out;
        const char* data = static_cast<const char*>(GetData());
        if (!data) return out;

        std::string raw(data, n);
        size_t start = 0;
        while (start <= raw.size()) {
            size_t nl = raw.find('\n', start);
            std::string token = (nl == std::string::npos)
                ? raw.substr(start)
                : raw.substr(start, nl - start);
            if (!token.empty()) out.push_back(token);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        return out;
    }
};

} // namespace

// Drop target installed on each project header panel.  Lives at file
// scope (rather than the anonymous namespace above) so the friend
// declaration in conversation_sidebar.h can reference it by name.
// The sidebar owns the actual logic; this class is just a wxWidgets-
// shaped adapter that calls back into the sidebar with the dropped
// paths.
class HeaderDropTarget : public wxDropTarget
{
public:
    HeaderDropTarget(ConversationSidebar* sb, std::string groupId)
        : m_sidebar(sb)
        , m_groupId(std::move(groupId))
    {
        // Drop target takes ownership of the data object and deletes
        // it in its destructor.  We own one instance per target.
        SetDataObject(new ChatPathsDataObject());
    }

    wxDragResult OnEnter(wxCoord, wxCoord, wxDragResult def) override
    {
        m_sidebar->OnDragHoverHeader(m_groupId, true);
        return def;
    }

    wxDragResult OnDragOver(wxCoord, wxCoord, wxDragResult def) override
    {
        // Always allow a move — the sidebar knows how to skip no-ops
        // (drops onto the chat's existing project become quiet
        // skipped-counts in MoveChatsToProject's summary).
        return def == wxDragNone ? wxDragNone : wxDragMove;
    }

    void OnLeave() override
    {
        m_sidebar->OnDragHoverHeader(m_groupId, false);
    }

    wxDragResult OnData(wxCoord, wxCoord, wxDragResult def) override
    {
        m_sidebar->OnDragHoverHeader(m_groupId, false);
        if (!GetData()) return wxDragNone;

        auto* obj = dynamic_cast<ChatPathsDataObject*>(GetDataObject());
        if (!obj) return wxDragNone;

        m_sidebar->OnChatsDroppedOnHeader(obj->GetPaths(), m_groupId);
        return def;
    }

private:
    ConversationSidebar* m_sidebar;  // Not owned
    std::string          m_groupId;  // Real project id, or kUnassignedId
};

bool ConversationSidebar::IsGroupCollapsed(const std::string& groupId) const
{
    // While a search filter is active, every group renders expanded so
    // FilterRows() can decide visibility per-row.  The user's persisted
    // collapsed set is untouched and resumes effect when the filter clears.
    if (!m_searchFilter.empty()) return false;
    if (m_overrideExpandedGroups.count(groupId)) return false;
    return m_collapsedGroups.count(groupId) != 0;
}

std::vector<std::string> ConversationSidebar::CollapsedGroupsAsVector() const
{
    return std::vector<std::string>(m_collapsedGroups.begin(),
                                    m_collapsedGroups.end());
}

ConversationSidebar::HeaderWidgets
ConversationSidebar::CreateProjectHeader(const std::string& groupId,
                                         const std::string& displayName,
                                         int chatCount,
                                         bool collapsed)
{
    HeaderWidgets header;
    header.groupId     = groupId;
    header.displayName = displayName;
    header.chatCount   = chatCount;
    header.collapsed   = collapsed;

    header.panel = new wxPanel(m_listWindow, wxID_ANY);
    header.panel->SetBackgroundColour(m_theme->bgSidebar);
    header.panel->SetCursor(wxCursor(wxCURSOR_HAND));

    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    // Compact typography keeps section labels visually quiet so the
    // conversation cards remain the main focus — but in the app's teletype
    // face so the project/section names read as part of the LlamaBoss chrome.
    // The ▼/▶ collapse triangles are already box-drawing glyphs, so the mono
    // face renders them identically while unifying the type.
    wxFont headerFont = SidebarMonoFont(10, wxFONTWEIGHT_BOLD);

    header.triangle = new wxStaticText(header.panel, wxID_ANY,
        wxString::FromUTF8(collapsed ? kTriangleCollapsed : kTriangleExpanded));
    header.triangle->SetForegroundColour(m_theme->textMuted);
    header.triangle->SetFont(headerFont);
    header.triangle->SetCursor(wxCursor(wxCURSOR_HAND));
    sizer->Add(header.triangle, 0,
               wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 14);

    header.nameLabel = new wxStaticText(header.panel, wxID_ANY,
        wxString::FromUTF8(displayName));
    header.nameLabel->SetForegroundColour(m_theme->textPrimary);
    header.nameLabel->SetFont(headerFont);
    header.nameLabel->SetCursor(wxCursor(wxCURSOR_HAND));
    sizer->Add(header.nameLabel, 0,
               wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 7);

    // Chat count — a compact rounded badge aligned to the right.  A custom
    // painted panel avoids the square native-static-text background seen in
    // the first pass while remaining theme-aware and DPI-safe.
    header.countBadge = new SidebarCountBadge(
        header.panel,
        MixSidebarColour(m_theme->attachChipBg,
                         m_theme->accentButton, 14),
        m_theme->textPrimary);
    header.countBadge->SetCursor(wxCursor(wxCURSOR_HAND));

    // Spacer first → count is pushed to the right edge, independent of
    // how long the project name is.  Right margin clears the list's
    // vertical scrollbar.
    sizer->AddStretchSpacer(1);
    sizer->Add(header.countBadge, 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT | wxTOP | wxBOTTOM, 10);

    header.panel->SetSizer(sizer);

    // Update count label content based on initial state
    UpdateProjectHeader(header, displayName, chatCount, collapsed);

    // ── Click → toggle collapse ─────────────────────────────────
    // Whole row is the hit target, including the triangle and the
    // name label.  The lambda captures groupId by value so each
    // header gets its own dedicated handler — no widget-tree walk
    // and no fragile name-stash needed.
    auto clickToggle = [this, groupId](wxMouseEvent&) {
        OnProjectHeaderClicked(groupId);
    };

    header.panel->Bind(wxEVT_LEFT_UP, clickToggle);
    header.triangle->Bind(wxEVT_LEFT_UP, clickToggle);
    header.nameLabel->Bind(wxEVT_LEFT_UP, clickToggle);
    header.countBadge->Bind(wxEVT_LEFT_UP, clickToggle);

    // ── Right-click → header context menu ──────────────────────
    // Frame builds the popup (Attach this chat / open folders /
    // delete project).  Sentinel sections (Unassigned, Goals) pass an
    // empty groupId up so the frame shows nothing — neither is a real
    // project, so there are no project actions to offer.
    const std::string contextId =
        (groupId == kUnassignedId || groupId == kGoalsId)
            ? std::string() : groupId;
    auto rightClickMenu =
        [this, contextId, panel = header.panel](wxMouseEvent&) {
            if (m_callbacks.onProjectHeaderContextMenuRequested) {
                m_callbacks.onProjectHeaderContextMenuRequested(contextId, panel);
            }
        };

    header.panel->Bind(wxEVT_RIGHT_UP, rightClickMenu);
    header.triangle->Bind(wxEVT_RIGHT_UP, rightClickMenu);
    header.nameLabel->Bind(wxEVT_RIGHT_UP, rightClickMenu);
    header.countBadge->Bind(wxEVT_RIGHT_UP, rightClickMenu);

    // ── Drop target ─────────────────────────────────────────────
    // Owned by the panel (wxWindow::SetDropTarget takes ownership).
    // Unassigned sections receive drops too — that's how chats leave
    // a project via DnD.  The OS routes drops over child static-text
    // widgets up to the parent panel since the children have no drop
    // target of their own.
    //
    // The Goals section is deliberately NOT a drop target: membership
    // there is derived from whether a chat has a goal, which can't be
    // set by dragging.  Without a target the OS shows the no-drop cursor
    // over it, which is the correct affordance.  (OnChatsDroppedOnHeader
    // also guards kGoalsId defensively in case the wiring ever changes.)
    if (groupId != kGoalsId) {
        header.panel->SetDropTarget(new HeaderDropTarget(this, groupId));
    }

    return header;
}

void ConversationSidebar::UpdateProjectHeader(HeaderWidgets& header,
                                              const std::string& displayName,
                                              int chatCount,
                                              bool collapsed)
{
    header.chatCount = chatCount;

    if (header.collapsed != collapsed) {
        header.collapsed = collapsed;
        if (header.triangle) {
            header.triangle->SetLabel(wxString::FromUTF8(
                collapsed ? kTriangleCollapsed : kTriangleExpanded));
        }
    }

    if (header.displayName != displayName) {
        header.displayName = displayName;
        if (header.nameLabel) {
            header.nameLabel->SetLabel(wxString::FromUTF8(displayName));
        }
    }

    // Count is always shown now, expanded or collapsed.  Zero is hidden,
    // though in practice a rendered header always has at least one chat.
    if (header.countBadge) {
        auto* badge =
            static_cast<SidebarCountBadge*>(header.countBadge);
        badge->SetCount(chatCount);
    }

    if (header.panel) {
        header.panel->Layout();
    }
}

void ConversationSidebar::RemoveProjectHeader(const std::string& groupId)
{
    auto it = m_projectHeaders.find(groupId);
    if (it == m_projectHeaders.end()) return;

    if (it->second.panel) {
        m_listSizer->Detach(it->second.panel);
        it->second.panel->Destroy();
        it->second.panel = nullptr;
    }
}

std::string ConversationSidebar::MakeDateHeaderKey(
    const std::string& groupId,
    const std::string& dateBucketId)
{
    // Both pieces are controlled identifiers.  Unit Separator is not legal in
    // project IDs and cannot occur in our fixed bucket names, so the key is
    // unambiguous without allocating a nested map.
    return groupId + '\x1F' + dateBucketId;
}

ConversationSidebar::DateHeaderWidgets
ConversationSidebar::CreateDateHeader(const std::string& groupId,
                                      const std::string& dateBucketId,
                                      const std::string& displayLabel)
{
    DateHeaderWidgets header;
    header.groupId = groupId;
    header.dateBucketId = dateBucketId;
    header.displayLabel = displayLabel;
    header.key = MakeDateHeaderKey(groupId, dateBucketId);

    header.panel = new wxPanel(m_listWindow, wxID_ANY,
                               wxDefaultPosition, wxSize(-1, 24),
                               wxBORDER_NONE);
    header.panel->SetMinSize(wxSize(-1, 24));
    header.panel->SetBackgroundColour(m_theme->bgSidebar);

    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    header.label = new wxStaticText(
        header.panel, wxID_ANY, wxString::FromUTF8(displayLabel.c_str()));
    // The date dividers (TODAY / YESTERDAY / PREVIOUS 7 DAYS / OLDER) are the
    // most terminal-flavored labels in the sidebar; the teletype face makes
    // them read as section rules in the LlamaBoss idiom rather than generic
    // app captions.
    header.label->SetFont(SidebarMonoFont(8, wxFONTWEIGHT_BOLD));
    header.label->SetForegroundColour(m_theme->textMuted);
    header.label->SetBackgroundColour(m_theme->bgSidebar);

    // Align the date label with the conversation title column rather than
    // the card edge.  Use a dedicated spacer for the 54 px indent.  Passing
    // 54 as the border while also specifying wxTOP | wxBOTTOM applies that
    // same 54 px to the vertical edges, which pushes the label outside this
    // 24 px header panel and leaves only a blank gap visible.
    sizer->AddSpacer(54);
    sizer->Add(header.label, 0, wxALIGN_CENTER_VERTICAL);
    header.panel->SetSizer(sizer);
    header.panel->Layout();
    return header;
}

void ConversationSidebar::UpdateDateHeader(DateHeaderWidgets& header,
                                           const std::string& displayLabel)
{
    if (header.displayLabel == displayLabel)
        return;
    header.displayLabel = displayLabel;
    if (header.label)
        header.label->SetLabel(wxString::FromUTF8(displayLabel.c_str()));
    if (header.panel)
        header.panel->Layout();
}

void ConversationSidebar::RemoveDateHeader(const std::string& key)
{
    auto it = m_dateHeaders.find(key);
    if (it == m_dateHeaders.end()) return;
    if (it->second.panel) {
        m_listSizer->Detach(it->second.panel);
        it->second.panel->Destroy();
        it->second.panel = nullptr;
    }
}

void ConversationSidebar::OnProjectHeaderClicked(const std::string& groupId)
{
    if (groupId.empty()) return;

    // Toggle based on what the user actually sees, not the raw persisted
    // flag.  A group can be persisted-collapsed yet displayed expanded
    // because it holds the active chat (auto-reveal); in that case the
    // header shows ▼ and a click should collapse it, even though
    // m_collapsedGroups already contains it.
    if (!IsGroupCollapsed(groupId)) {
        // Currently shown expanded → collapse it.  Dropping it from the
        // auto-reveal override (which is only rebuilt when the active chat
        // changes) is what makes the collapse stick even while this group
        // holds the active chat.
        m_collapsedGroups.insert(groupId);
        m_overrideExpandedGroups.erase(groupId);
    }
    else {
        // Currently shown collapsed → expand it.
        m_collapsedGroups.erase(groupId);
    }

    if (m_callbacks.onCollapsedProjectsChanged) {
        m_callbacks.onCollapsedProjectsChanged(CollapsedGroupsAsVector());
    }

    Refresh(m_activeFilePath);
}

// ═══════════════════════════════════════════════════════════════════
//  Drag-and-drop — internal handlers
// ═══════════════════════════════════════════════════════════════════

std::vector<std::string>
ConversationSidebar::PathsForDragFrom(const std::string& path) const
{
    // If the row that started the drag is part of the multi-selection,
    // drag everything that's selected.  If it isn't, drag only that
    // one row — without disturbing the selection.  This matches the
    // file-explorer convention users are already familiar with.
    if (path.empty()) return {};

    if (m_selected.count(path)) {
        return std::vector<std::string>(m_selected.begin(), m_selected.end());
    }
    return { path };
}

void ConversationSidebar::MaybeStartDragFrom(const std::string& path,
                                             wxWindow* origin)
{
    auto paths = PathsForDragFrom(path);
    if (paths.empty()) return;

    // wxDropSource needs the data object to outlive DoDragDrop; stack
    // allocation is the simplest way to guarantee that.  The drop
    // target on the receiving end has its own data-object instance
    // that GetData() copies into — so the source's local object can
    // safely vanish when this function returns.
    ChatPathsDataObject obj;
    obj.SetPaths(paths);

    wxDropSource source(obj, origin ? origin : m_listWindow);
    source.DoDragDrop(wxDrag_DefaultMove);
    // We don't act on the result — the drop target invokes
    // OnChatsDroppedOnHeader directly when a drop succeeds.
}

void ConversationSidebar::OnDragHoverHeader(const std::string& groupId,
                                            bool hovering)
{
    auto it = m_projectHeaders.find(groupId);
    if (it == m_projectHeaders.end() || !it->second.panel) return;

    // Reuse sidebarHover for highlight — same color the chat rows
    // use on mouse-over, so the visual language is consistent.
    wxColour bg = hovering ? m_theme->sidebarHover : m_theme->bgDialogSurface;
    it->second.panel->SetBackgroundColour(bg);
    it->second.panel->Refresh();
}

void ConversationSidebar::OnChatsDroppedOnHeader(
    const std::vector<std::string>& paths,
    const std::string& groupId)
{
    if (!m_callbacks.onChatsDroppedOnProject) return;
    if (paths.empty()) return;

    // The Goals section isn't a real bucket you can drop into — its
    // membership comes from each chat's goal state, not from project
    // assignment.  The header has no drop target installed, so this
    // shouldn't fire for it, but guard anyway so a stray drop can never
    // be misread as "clear project."
    if (groupId == kGoalsId) return;

    // Translate the Unassigned sentinel back to the empty-string
    // convention MoveChatsToProject already uses.  The frame doesn't
    // know about kUnassignedId — keep that abstraction local.
    const std::string targetId =
        (groupId == kUnassignedId) ? std::string() : groupId;

    m_callbacks.onChatsDroppedOnProject(paths, targetId);
}

// ═══════════════════════════════════════════════════════════════════
//  Internal — create a cached conversation row
// ═══════════════════════════════════════════════════════════════════

ConversationSidebar::RowWidgets
ConversationSidebar::CreateRow(const ConversationEntry& entry)
{
    RowWidgets row;
    row.filePath = entry.filePath;
    row.modTime = entry.modTime;
    row.hasActivityTime = entry.hasActivityTime;
    row.projectId = entry.projectId;
    row.projectName = entry.projectName;
    row.hasGoal = !entry.goalObjective.empty();
    row.pinned = entry.pinned;
    row.archived = entry.archived;
    row.dateBucketId = DateBucketIdFor(entry.modTime, entry.pinned,
                                       entry.hasActivityTime);
    if (!entry.projectId.empty())
        row.groupId = entry.projectId;
    else if (row.hasGoal)
        row.groupId = kGoalsId;
    else
        row.groupId = kUnassignedId;

    wxString displayTitle = SidebarDisplayTitleFromUtf8(entry.title);
    const wxString fullTitle = wxString::FromUTF8(entry.title.c_str());
    row.displayedTitle = WxToUtf8String(displayTitle);
    row.searchTitleLower = SidebarSearchKeyFromUtf8(entry.title);
    row.displayedTime = RelativeTimeString(entry.modTime);

    // ── Card shell ──────────────────────────────────────────────
    row.panel = new wxPanel(m_listWindow, wxID_ANY);
    row.panel->SetName(wxString::FromUTF8(entry.filePath));
    row.panel->SetMinSize(wxSize(-1, 66));
    row.panel->SetCursor(wxCursor(wxCURSOR_HAND));

    auto* cardSizer = new wxBoxSizer(wxHORIZONTAL);

    // A fixed-width bar avoids any title movement when the row becomes
    // active.  Inactive rows paint it the same color as the card.
    row.accentBar = new wxPanel(row.panel, wxID_ANY,
                                wxDefaultPosition, wxSize(4, -1));
    cardSizer->Add(row.accentBar, 0, wxEXPAND);

    auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);

    // Compact conversation icon tile.  A shell-prompt chevron in the app's
    // teletype face replaces the previous color-emoji speech bubble: it keeps
    // the tile as the per-conversation color carrier (tinted by project via
    // ApplyRowAppearance) while reading as part of LlamaBoss's terminal idiom
    // instead of a consumer chat glyph.  The chevron exists in Cascadia Mono /
    // Consolas, so no image asset or emoji-capable fallback font is needed.
    row.iconPanel = new wxPanel(row.panel, wxID_ANY,
                                wxDefaultPosition, wxSize(34, 34));
    row.iconPanel->SetMinSize(wxSize(34, 34));
    row.iconPanel->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* iconSizer = new wxBoxSizer(wxVERTICAL);
    row.iconLabel = new wxStaticText(row.iconPanel, wxID_ANY,
        wxString::FromUTF8("\xE2\x9D\xAF"),   // ❯ shell-prompt chevron
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    row.iconLabel->SetFont(SidebarMonoFont(13, wxFONTWEIGHT_BOLD));
    row.iconLabel->SetCursor(wxCursor(wxCURSOR_HAND));
    iconSizer->AddStretchSpacer(1);
    iconSizer->Add(row.iconLabel, 0, wxALIGN_CENTER_HORIZONTAL);
    iconSizer->AddStretchSpacer(1);
    row.iconPanel->SetSizer(iconSizer);
    contentSizer->Add(row.iconPanel, 0,
                      wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);

    // Title and metadata stack.
    auto* textSizer = new wxBoxSizer(wxVERTICAL);
    row.titleLabel = new wxStaticText(
        row.panel, wxID_ANY, displayTitle,
        wxDefaultPosition, wxSize(0, -1),
        wxST_NO_AUTORESIZE);
    // A zero horizontal minimum lets the title surrender space to the fixed
    // action slot instead of pushing the dots beyond the clipped card edge.
    // Native clipping is intentional here: the full title remains available
    // through the tooltip, and the row no longer displays a second "..."
    // beside the conversation-actions control.
    row.titleLabel->SetMinSize(wxSize(0, -1));
    // Title in the app's teletype face, bold for hierarchy against the muted
    // mono timestamp below it.
    row.titleLabel->SetFont(SidebarMonoFont(11, wxFONTWEIGHT_BOLD));
    row.titleLabel->SetCursor(wxCursor(wxCURSOR_HAND));
    row.titleLabel->SetToolTip(fullTitle);
    textSizer->Add(row.titleLabel, 0, wxEXPAND | wxTOP, 9);

    auto* metadataSizer = new wxBoxSizer(wxHORIZONTAL);

    row.timeLabel = new wxStaticText(row.panel, wxID_ANY,
        wxString::FromUTF8(row.displayedTime));
    // Muted mono timestamp — same face as the title, lighter weight and
    // color (set in ApplyRowAppearance) so the hierarchy reads clearly.
    row.timeLabel->SetFont(SidebarMonoFont(9));
    row.timeLabel->SetCursor(wxCursor(wxCURSOR_HAND));
    metadataSizer->Add(row.timeLabel, 0, wxALIGN_CENTER_VERTICAL);
    metadataSizer->AddStretchSpacer(1);

    const std::string projectTagText = SidebarProjectTagText(
        row.projectId, row.projectName, row.hasGoal);
    const wxColour projectAccent = SidebarProjectAccent(
        *m_theme, row.projectId, row.hasGoal);
    row.projectTag = new SidebarProjectTag(
        row.panel, projectTagText,
        MixSidebarColour(SidebarCardBackground(*m_theme),
                         projectAccent, 29),
        MixSidebarColour(projectAccent, m_theme->textPrimary, 28));
    row.projectTag->SetCursor(wxCursor(wxCURSOR_HAND));
    metadataSizer->Add(row.projectTag, 0,
                       wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

    textSizer->Add(metadataSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 3);

    contentSizer->Add(textSizer, 1, wxEXPAND | wxRIGHT, 6);

    // Fixed ASCII three-dot actions affordance.  The widget always reserves
    // its slot, but ApplyRowAppearance paints it only for the active,
    // selected, or hovered row.  This keeps the menu reliable on Windows and
    // leaves long monospace titles visually clean.
    row.menuBtn = new wxStaticText(
        row.panel, wxID_ANY, wxS("..."),
        wxDefaultPosition, wxSize(32, 28),
        wxALIGN_CENTER_HORIZONTAL | wxST_NO_AUTORESIZE);
    row.menuBtn->SetMinSize(wxSize(32, 28));
    row.menuBtn->SetMaxSize(wxSize(32, 28));
    row.menuBtn->SetFont(SidebarMonoFont(13, wxFONTWEIGHT_BOLD));
    row.menuBtn->SetCursor(wxCursor(wxCURSOR_HAND));
    row.menuBtn->SetToolTip("Conversation actions");
    contentSizer->Add(
        row.menuBtn, 0,
        wxALIGN_CENTER_VERTICAL | wxRIGHT,
        8);

    cardSizer->Add(contentSizer, 1, wxEXPAND);
    row.panel->SetSizer(cardSizer);

    ApplyRowAppearance(row, false);

    // ── Selection / open handling ───────────────────────────────
    auto clickHandler = [this](wxMouseEvent& evt) {
        std::string path = PathFromWidget(
            dynamic_cast<wxWindow*>(evt.GetEventObject()), m_listWindow);
        if (path.empty()) return;

        if (evt.ControlDown()) {
            if (IsSelected(path))
                m_selected.erase(path);
            else
                m_selected.insert(path);
            m_anchorPath = path;
            RefreshAllRowBackgrounds();
        }
        else if (evt.ShiftDown() && !m_anchorPath.empty()) {
            SelectRange(m_anchorPath, path);
            RefreshAllRowBackgrounds();
        }
        else {
            m_selected.clear();
            m_selected.insert(path);
            m_anchorPath = path;
            RefreshAllRowBackgrounds();
            if (m_callbacks.onConversationClicked) {
                m_listWindow->CallAfter([this, path]() {
                    m_callbacks.onConversationClicked(path);
                });
            }
        }

        m_listWindow->SetFocusIgnoringChildren();
    };

    auto rightClickHandler = [this](wxMouseEvent& evt) {
        std::string path = PathFromWidget(
            dynamic_cast<wxWindow*>(evt.GetEventObject()), m_listWindow);
        if (path.empty()) return;

        if (!IsSelected(path)) {
            m_selected.clear();
            m_selected.insert(path);
            m_anchorPath = path;
            RefreshAllRowBackgrounds();
        }

        m_listWindow->SetFocusIgnoringChildren();
        m_listWindow->CallAfter([this, path]() {
            ShowContextMenu(path);
        });
    };

    const std::vector<wxWindow*> rowTargets = {
        row.panel, row.iconPanel, row.iconLabel,
        row.titleLabel, row.timeLabel, row.projectTag
    };
    for (wxWindow* target : rowTargets) {
        target->Bind(wxEVT_LEFT_UP, clickHandler);
        target->Bind(wxEVT_RIGHT_UP, rightClickHandler);
    }

    // ── Drag initiation ─────────────────────────────────────────
    auto pressDown = [this, path = entry.filePath](wxMouseEvent& evt) {
        m_dragState.pressActive = true;
        m_dragState.pressPath   = path;
        m_dragState.pressPoint  = evt.GetPosition();
        evt.Skip();
    };

    auto pressMotion = [this](wxMouseEvent& evt) {
        if (!m_dragState.pressActive) {
            evt.Skip();
            return;
        }
        if (!evt.LeftIsDown()) {
            m_dragState.pressActive = false;
            evt.Skip();
            return;
        }

        const wxPoint cur = evt.GetPosition();
        const int dx = cur.x - m_dragState.pressPoint.x;
        const int dy = cur.y - m_dragState.pressPoint.y;
        if (dx * dx + dy * dy < kDragThresholdSq) {
            evt.Skip();
            return;
        }

        const std::string fromPath = m_dragState.pressPath;
        m_dragState.pressActive = false;

        wxWindow* origin = dynamic_cast<wxWindow*>(evt.GetEventObject());
        MaybeStartDragFrom(fromPath, origin);
    };

    auto pressEnd = [this](wxMouseEvent& evt) {
        m_dragState.pressActive = false;
        evt.Skip();
    };

    for (wxWindow* target : rowTargets) {
        target->Bind(wxEVT_LEFT_DOWN, pressDown);
        target->Bind(wxEVT_MOTION, pressMotion);
        target->Bind(wxEVT_LEFT_UP, pressEnd);
    }

    // ── Three-dot menu click ────────────────────────────────────
    row.menuBtn->Bind(wxEVT_LEFT_UP,
        [this, panel = row.panel](wxMouseEvent&) {
            std::string path = panel->GetName().ToUTF8().data();
            if (path.empty()) return;

            if (!IsSelected(path)) {
                m_selected.clear();
                m_selected.insert(path);
                m_anchorPath = path;
                RefreshAllRowBackgrounds();
            }

            m_listWindow->SetFocusIgnoringChildren();
            panel->CallAfter([this, path]() {
                ShowContextMenu(path);
            });
        });
    row.menuBtn->Bind(wxEVT_RIGHT_UP, rightClickHandler);

    // ── Hover treatment ─────────────────────────────────────────
    // Defer leave evaluation so moving between child widgets does not flash
    // the card back to its normal background.
    auto enterHandler = [panel = row.panel, this](wxMouseEvent&) {
        const std::string path = panel->GetName().ToUTF8().data();
        auto it = m_rows.find(path);
        if (it != m_rows.end())
            ApplyRowAppearance(it->second, true);
    };

    auto leaveHandler = [panel = row.panel, this](wxMouseEvent&) {
        panel->CallAfter([panel, this]() {
            if (!panel) return;
            const wxPoint mouseScreen = wxGetMousePosition();
            if (panel->GetScreenRect().Contains(mouseScreen)) return;

            const std::string path = panel->GetName().ToUTF8().data();
            auto it = m_rows.find(path);
            if (it != m_rows.end())
                ApplyRowAppearance(it->second, false);
        });
    };

    for (wxWindow* target : rowTargets) {
        target->Bind(wxEVT_ENTER_WINDOW, enterHandler);
        target->Bind(wxEVT_LEAVE_WINDOW, leaveHandler);
    }
    row.menuBtn->Bind(wxEVT_ENTER_WINDOW, enterHandler);
    row.menuBtn->Bind(wxEVT_LEAVE_WINDOW, leaveHandler);

    return row;
}

void ConversationSidebar::UpdateRow(RowWidgets& row,
                                    const ConversationEntry& entry)
{
    row.filePath = entry.filePath;
    row.modTime = entry.modTime;
    row.hasActivityTime = entry.hasActivityTime;

    wxString displayTitle = SidebarDisplayTitleFromUtf8(entry.title);
    const wxString fullTitle = wxString::FromUTF8(entry.title.c_str());
    std::string newDisplayedTitle = WxToUtf8String(displayTitle);
    std::string newSearchTitleLower = SidebarSearchKeyFromUtf8(entry.title);
    std::string newTime = RelativeTimeString(entry.modTime);

    if (newDisplayedTitle != row.displayedTitle) {
        row.displayedTitle = newDisplayedTitle;
        if (row.titleLabel)
            row.titleLabel->SetLabel(displayTitle);
    }
    if (row.titleLabel)
        row.titleLabel->SetToolTip(fullTitle);

    if (newSearchTitleLower != row.searchTitleLower)
        row.searchTitleLower = newSearchTitleLower;

    if (newTime != row.displayedTime) {
        row.displayedTime = newTime;
        if (row.timeLabel)
            row.timeLabel->SetLabel(wxString::FromUTF8(newTime.c_str()));
    }

    const bool newHasGoal = !entry.goalObjective.empty();
    const bool projectMetadataChanged =
        row.projectId != entry.projectId ||
        row.projectName != entry.projectName ||
        row.hasGoal != newHasGoal;

    row.projectId = entry.projectId;
    row.projectName = entry.projectName;
    row.hasGoal = newHasGoal;
    row.pinned = entry.pinned;
    row.archived = entry.archived;
    row.dateBucketId = DateBucketIdFor(entry.modTime, entry.pinned,
                                       entry.hasActivityTime);
    if (!entry.projectId.empty())
        row.groupId = entry.projectId;
    else if (newHasGoal)
        row.groupId = kGoalsId;
    else
        row.groupId = kUnassignedId;

    if (projectMetadataChanged && row.projectTag) {
        static_cast<SidebarProjectTag*>(row.projectTag)->SetTagText(
            SidebarProjectTagText(row.projectId, row.projectName, row.hasGoal));
        row.panel->Layout();
    }

    ApplyRowAppearance(row, false);
}

void ConversationSidebar::RemoveRow(const std::string& filePath)
{
    auto found = m_rows.find(filePath);
    if (found == m_rows.end())
        return;

    if (found->second.panel) {
        m_listSizer->Detach(found->second.panel);
        found->second.panel->Destroy();
        found->second.panel = nullptr;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Internal — context menu for selected conversation(s)
// ═══════════════════════════════════════════════════════════════════
//
// The sidebar no longer builds the menu itself.  It just hands the
// selection (and an anchor window) up to the frame, which has the
// project metadata it needs to build a Move-to-project submenu.

void ConversationSidebar::ShowContextMenu(const std::string& filePath)
{
    // Build the path list the frame will operate on.  If the
    // right-clicked file is part of the current multi-selection we
    // operate on all selected; otherwise we operate on just that one.
    std::vector<std::string> paths;
    if (!filePath.empty() && IsSelected(filePath) && m_selected.size() > 1) {
        paths.assign(m_selected.begin(), m_selected.end());
    }
    else if (!filePath.empty()) {
        paths.push_back(filePath);
    }
    else {
        paths.assign(m_selected.begin(), m_selected.end());
    }

    if (paths.empty()) return;

    if (m_callbacks.onChatContextMenuRequested) {
        m_callbacks.onChatContextMenuRequested(paths, m_panel);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Static helper — human-readable relative time string
// ═══════════════════════════════════════════════════════════════════

std::string ConversationSidebar::RelativeTimeString(const wxDateTime& dt)
{
    if (!dt.IsValid()) return "";

    wxDateTime now = wxDateTime::Now();
    wxTimeSpan diff = now.Subtract(dt);

    int minutes = (int)diff.GetMinutes();
    if (minutes < 1) return "Just now";
    if (minutes < 60) return std::to_string(minutes) + " min ago";

    int hours = (int)diff.GetHours();
    if (hours < 24) return std::to_string(hours) + "h ago";

    int days = diff.GetDays();
    if (days == 1) return "Yesterday";
    if (days < 7) return std::to_string(days) + " days ago";
    if (days < 30) return std::to_string(days / 7) + "w ago";

    return dt.Format("%b %d").ToUTF8().data();
}

std::string ConversationSidebar::DateBucketIdFor(const wxDateTime& dt,
                                                 bool pinned,
                                                 bool hasActivityTime)
{
    if (pinned)
        return "pinned";
    // No parseable activity clock → do not let the filesystem mtime seeded
    // into dt decide the section (a pin/archive rewrite would move it).
    // Bucket as "older" instead; the newest-first sort still uses dt.
    if (!hasActivityTime)
        return "older";
    if (!dt.IsValid())
        return "older";

    wxDateTime today = wxDateTime::Now();
    today.ResetTime();
    wxDateTime itemDate = dt;
    itemDate.ResetTime();

    const long days = today.Subtract(itemDate).GetDays();
    if (days <= 0) return "today";
    if (days == 1) return "yesterday";
    if (days < 7)  return "previous7";
    return "older";
}

std::string ConversationSidebar::DateBucketLabel(
    const std::string& bucketId)
{
    if (bucketId == "pinned")     return "PINNED";
    if (bucketId == "today")      return "TODAY";
    if (bucketId == "yesterday")  return "YESTERDAY";
    if (bucketId == "previous7")  return "PREVIOUS 7 DAYS";
    return "OLDER";
}
