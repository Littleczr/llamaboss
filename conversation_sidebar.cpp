// conversation_sidebar.cpp
// Implementation of the collapsible conversation sidebar.
// Supports multi-select (Ctrl+Click, Shift+Click) and batch delete.

#include "conversation_sidebar.h"
#include "chat_history.h"
#include "theme.h"

#include <wx/dir.h>
#include <wx/filename.h>

#include <Poco/JSON/Parser.h>

#include <fstream>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════════

ConversationSidebar::ConversationSidebar(wxWindow* parent,
                                         const ThemeData& theme,
                                         const Callbacks& callbacks)
    : m_callbacks(callbacks)
    , m_theme(&theme)
{
    // ── Outer panel (sidebar + right border) ─────────────────────
    m_panel = new wxPanel(parent, wxID_ANY);
    m_panel->SetBackgroundColour(theme.bgSidebar);
    m_panel->SetMinSize(wxSize(260, -1));

    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // ── Content area ─────────────────────────────────────────────
    m_content = new wxPanel(m_panel, wxID_ANY);
    m_content->SetBackgroundColour(theme.bgSidebar);
    auto* contentSizer = new wxBoxSizer(wxVERTICAL);

    // "+ New Chat" button
    m_newChatButton = new wxButton(m_content, wxID_ANY, "+ New Chat",
        wxDefaultPosition, wxSize(-1, 42), wxBORDER_NONE);
    m_newChatButton->SetBackgroundColour(theme.modelPillBg);
    m_newChatButton->SetForegroundColour(theme.textPrimary);
    wxFont ncFont = m_newChatButton->GetFont();
    ncFont.SetPointSize(11);
    ncFont.SetWeight(wxFONTWEIGHT_MEDIUM);
    m_newChatButton->SetFont(ncFont);
    contentSizer->Add(m_newChatButton, 0, wxEXPAND | wxALL, 8);

    // Scrollable conversation list
    m_listWindow = new wxScrolledWindow(m_content, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_listWindow->SetBackgroundColour(theme.bgSidebar);
    m_listWindow->SetScrollRate(0, 8);
    m_listSizer = new wxBoxSizer(wxVERTICAL);
    m_listWindow->SetSizer(m_listSizer);

    contentSizer->Add(m_listWindow, 1, wxEXPAND);
    m_content->SetSizer(contentSizer);
    outerSizer->Add(m_content, 1, wxEXPAND);

    // Vertical border on the right edge
    m_border = new wxPanel(m_panel, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
    m_border->SetBackgroundColour(theme.borderSubtle);
    outerSizer->Add(m_border, 0, wxEXPAND);

    m_panel->SetSizer(outerSizer);
    m_panel->Hide();  // Start collapsed

    // ── Bind events ──────────────────────────────────────────────
    m_newChatButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_callbacks.onNewChatClicked)
            m_callbacks.onNewChatClicked();
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

// ═══════════════════════════════════════════════════════════════════
//  Content — rebuild the conversation list from disk
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::Refresh(const std::string& activeFilePath)
{
    if (!m_listSizer) return;

    m_activeFilePath = activeFilePath;

    // Clear existing UI entries
    m_listSizer->Clear(true);

    // Scan and sort
    auto entries = ScanConversations();

    // Rebuild the ordered path list for Shift+Click range logic
    m_orderedPaths.clear();
    m_orderedPaths.reserve(entries.size());

    // Prune selection: remove any paths that no longer exist on disk
    std::set<std::string> validPaths;
    for (const auto& e : entries)
        validPaths.insert(e.filePath);

    for (auto it = m_selected.begin(); it != m_selected.end(); ) {
        if (validPaths.find(*it) == validPaths.end())
            it = m_selected.erase(it);
        else
            ++it;
    }

    // Build UI for each entry
    for (const auto& entry : entries) {
        m_orderedPaths.push_back(entry.filePath);
        BuildListEntry(entry);
    }

    m_listWindow->FitInside();
    m_listWindow->Layout();
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

bool ConversationSidebar::IsSelected(const std::string& path) const
{
    return m_selected.find(path) != m_selected.end();
}

void ConversationSidebar::SelectRange(const std::string& from,
                                      const std::string& to)
{
    // Find indices in the ordered list
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

// ═══════════════════════════════════════════════════════════════════
//  Theming
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::ApplyTheme(const ThemeData& theme)
{
    m_theme = &theme;

    m_panel->SetBackgroundColour(theme.bgSidebar);
    m_content->SetBackgroundColour(theme.bgSidebar);
    m_newChatButton->SetBackgroundColour(theme.modelPillBg);
    m_newChatButton->SetForegroundColour(theme.textPrimary);
    m_listWindow->SetBackgroundColour(theme.bgSidebar);
    m_border->SetBackgroundColour(theme.borderSubtle);
}

// ═══════════════════════════════════════════════════════════════════
//  Row background logic
// ═══════════════════════════════════════════════════════════════════

wxColour ConversationSidebar::GetRowBackground(const std::string& filePath) const
{
    // Priority: active > selected > normal
    if (filePath == m_activeFilePath)
        return m_theme->modelPillBg;
    if (IsSelected(filePath))
        return m_theme->sidebarSelected;
    return m_theme->bgSidebar;
}

void ConversationSidebar::RefreshAllRowBackgrounds()
{
    // Walk through every row panel in the sizer and update its color
    for (size_t i = 0; i < m_listSizer->GetItemCount(); ++i) {
        wxSizerItem* item = m_listSizer->GetItem(i);
        if (!item) continue;
        wxWindow* win = item->GetWindow();
        if (!win) continue;

        std::string path = win->GetName().ToStdString();
        if (!path.empty()) {
            win->SetBackgroundColour(GetRowBackground(path));
            win->Refresh();
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Static helper — resolve a child widget click to a file path
// ═══════════════════════════════════════════════════════════════════

std::string ConversationSidebar::PathFromWidget(wxWindow* win, wxWindow* stop)
{
    while (win && win != stop) {
        wxString name = win->GetName();
        if (name.EndsWith(".json"))
            return name.ToStdString();
        win = win->GetParent();
    }
    return {};
}

// ═══════════════════════════════════════════════════════════════════
//  Internal — scan conversation files from disk
// ═══════════════════════════════════════════════════════════════════

std::vector<ConversationSidebar::ConversationEntry>
ConversationSidebar::ScanConversations() const
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
        entry.filePath = fullPath.ToStdString();

        // Read the title from the JSON file
        try {
            std::ifstream file(entry.filePath);
            if (file.is_open()) {
                std::string content((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
                file.close();

                Poco::JSON::Parser parser;
                auto result = parser.parse(content);
                auto obj = result.extract<Poco::JSON::Object::Ptr>();

                if (obj->has("title")) {
                    entry.title = obj->getValue<std::string>("title");
                }
            }
        }
        catch (...) {
            // Skip files we can't parse
        }

        if (entry.title.empty()) {
            entry.title = filename.ToStdString();
        }

        // Get file modification time
        wxFileName fn(fullPath);
        fn.GetTimes(nullptr, &entry.modTime, nullptr);

        entries.push_back(entry);
        found = dir.GetNext(&filename);
    }

    // Sort by modification time, newest first
    std::sort(entries.begin(), entries.end(),
        [](const ConversationEntry& a, const ConversationEntry& b) {
            return a.modTime.IsLaterThan(b.modTime);
        });

    return entries;
}

// ═══════════════════════════════════════════════════════════════════
//  Internal — build a single conversation row in the list
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::BuildListEntry(const ConversationEntry& entry)
{
    auto* panel = new wxPanel(m_listWindow, wxID_ANY);
    panel->SetBackgroundColour(GetRowBackground(entry.filePath));

    auto* panelSizer = new wxBoxSizer(wxVERTICAL);

    // Title (truncated)
    std::string displayTitle = entry.title;
    if (displayTitle.size() > 35) {
        displayTitle = displayTitle.substr(0, 32) + "...";
    }
    auto* titleLabel = new wxStaticText(panel, wxID_ANY,
        wxString::FromUTF8(displayTitle));
    titleLabel->SetForegroundColour(m_theme->textMuted);
    wxFont titleFont = titleLabel->GetFont();
    titleFont.SetPointSize(11);
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    titleLabel->SetFont(titleFont);
    panelSizer->Add(titleLabel, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    // Relative time
    std::string timeStr = RelativeTimeString(entry.modTime);
    auto* timeLabel = new wxStaticText(panel, wxID_ANY,
        wxString::FromUTF8(timeStr));
    timeLabel->SetForegroundColour(m_theme->textMuted);
    wxFont timeFont = timeLabel->GetFont();
    timeFont.SetPointSize(9);
    timeLabel->SetFont(timeFont);
    panelSizer->Add(timeLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    panel->SetSizer(panelSizer);

    // Store file path in the panel name for click handling
    panel->SetName(wxString::FromUTF8(entry.filePath));

    // ── Left-click handler (selection + optional load) ───────────
    auto clickHandler = [this](wxMouseEvent& evt) {
        std::string path = PathFromWidget(
            dynamic_cast<wxWindow*>(evt.GetEventObject()), m_listWindow);
        if (path.empty()) return;

        if (evt.ControlDown()) {
            // Ctrl+Click: toggle this item in/out of selection, no load
            if (IsSelected(path))
                m_selected.erase(path);
            else
                m_selected.insert(path);
            m_anchorPath = path;
            RefreshAllRowBackgrounds();
        }
        else if (evt.ShiftDown() && !m_anchorPath.empty()) {
            // Shift+Click: range-select from anchor to here, no load
            SelectRange(m_anchorPath, path);
            RefreshAllRowBackgrounds();
        }
        else {
            // Plain click: clear selection, select + load this one
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
    };

    panel->Bind(wxEVT_LEFT_UP, clickHandler);
    titleLabel->Bind(wxEVT_LEFT_UP, clickHandler);
    timeLabel->Bind(wxEVT_LEFT_UP, clickHandler);

    // ── Right-click handler (context menu) ───────────────────────
    auto rightClickHandler = [this](wxMouseEvent& evt) {
        std::string path = PathFromWidget(
            dynamic_cast<wxWindow*>(evt.GetEventObject()), m_listWindow);
        if (path.empty()) return;

        if (!IsSelected(path)) {
            // Right-clicked an unselected item: replace selection
            m_selected.clear();
            m_selected.insert(path);
            m_anchorPath = path;
            RefreshAllRowBackgrounds();
        }
        // If already selected, keep the current multi-selection

        m_listWindow->CallAfter([this, path]() {
            ShowContextMenu(path);
        });
    };

    panel->Bind(wxEVT_RIGHT_UP, rightClickHandler);
    titleLabel->Bind(wxEVT_RIGHT_UP, rightClickHandler);
    timeLabel->Bind(wxEVT_RIGHT_UP, rightClickHandler);

    // ── Hover effect ─────────────────────────────────────────────
    // Only apply hover to rows that are not active or selected.
    auto enterHandler = [panel, this](wxMouseEvent&) {
        std::string p = panel->GetName().ToStdString();
        if (p != m_activeFilePath && !IsSelected(p)) {
            panel->SetBackgroundColour(m_theme->sidebarHover);
            panel->Refresh();
        }
    };
    auto leaveHandler = [panel, this](wxMouseEvent&) {
        std::string p = panel->GetName().ToStdString();
        panel->SetBackgroundColour(GetRowBackground(p));
        panel->Refresh();
    };
    panel->Bind(wxEVT_ENTER_WINDOW, enterHandler);
    panel->Bind(wxEVT_LEAVE_WINDOW, leaveHandler);

    m_listSizer->Add(panel, 0, wxEXPAND);
}

// ═══════════════════════════════════════════════════════════════════
//  Internal — context menu for selected conversation(s)
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::ShowContextMenu(const std::string& /*filePath*/)
{
    wxMenu menu;

    size_t count = m_selected.size();
    if (count <= 1) {
        menu.Append(wxID_DELETE, "Delete conversation");
    }
    else {
        wxString label = wxString::Format("Delete %zu conversations", count);
        menu.Append(wxID_DELETE, label);
    }

    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (m_callbacks.onDeleteRequested && !m_selected.empty()) {
            // Snapshot the selection — the callback may trigger Refresh()
            // which would clear UI, so capture paths first.
            std::vector<std::string> paths(m_selected.begin(), m_selected.end());
            m_callbacks.onDeleteRequested(paths);
        }
    }, wxID_DELETE);

    m_panel->PopupMenu(&menu);
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

    return dt.Format("%b %d").ToStdString();
}
