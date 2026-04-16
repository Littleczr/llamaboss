// conversation_sidebar.cpp
// Implementation of the collapsible conversation sidebar.
// Supports multi-select (Ctrl+Click, Shift+Click) and batch delete.

#include "conversation_sidebar.h"
#include "chat_history.h"
#include "theme.h"

#include <wx/dir.h>
#include <wx/filename.h>

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

    // Search box
    m_searchBox = new wxTextCtrl(m_content, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, 32),
        wxTE_PROCESS_ENTER | wxBORDER_SIMPLE);
    m_searchBox->SetHint("Search conversations...");
    m_searchBox->SetBackgroundColour(theme.bgInputField);
    m_searchBox->SetForegroundColour(theme.textPrimary);
    wxFont searchFont = m_searchBox->GetFont();
    searchFont.SetPointSize(10);
    m_searchBox->SetFont(searchFont);
    contentSizer->Add(m_searchBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

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
        int screenX = m_border->ClientToScreen(e.GetPosition()).x;
        int delta = screenX - m_dragStartX;
        int newW = std::clamp(m_dragStartWidth + delta, MIN_WIDTH, MAX_WIDTH);
        m_panel->SetMinSize(wxSize(newW, -1));
        m_panel->GetParent()->Layout();
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

    // ── Search box events ────────────────────────────────────────
    m_searchBox->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        FilterRows();
    });
    m_searchBox->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            ClearSearch();
        }
        else {
            e.Skip();
        }
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

    m_activeFilePath = activeFilePath;

    // Scan and sort
    auto entries = ScanConversations();

    // Rebuild ordered path list for Shift+Click range logic
    m_orderedPaths.clear();
    m_orderedPaths.reserve(entries.size());

    // Prune selection for files that no longer exist
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

    // Remove rows that disappeared
    for (auto it = m_rows.begin(); it != m_rows.end(); ) {
        if (validPaths.find(it->first) == validPaths.end()) {
            RemoveRow(it->first);
            it = m_rows.erase(it);
        }
        else {
            ++it;
        }
    }

    // Create/update/reorder rows without rebuilding them all
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        m_orderedPaths.push_back(entry.filePath);

        auto found = m_rows.find(entry.filePath);
        if (found == m_rows.end()) {
            RowWidgets row = CreateRow(entry);
            auto [insertedIt, _] = m_rows.emplace(entry.filePath, std::move(row));
            found = insertedIt;
        }
        else {
            UpdateRow(found->second, entry);
        }

        wxPanel* panel = found->second.panel;
        if (!panel) continue;

        wxSizerItem* currentItem =
            (i < m_listSizer->GetItemCount()) ? m_listSizer->GetItem(i) : nullptr;
        wxWindow* currentWindow = currentItem ? currentItem->GetWindow() : nullptr;

        if (currentWindow != panel) {
            m_listSizer->Detach(panel);
            m_listSizer->Insert(i, panel, 0, wxEXPAND);
        }

        panel->SetBackgroundColour(GetRowBackground(entry.filePath));
        panel->Refresh();
    }

    m_listWindow->FitInside();
    m_listWindow->Layout();
    m_listWindow->Thaw();

    // Reapply search filter if active
    if (!m_searchFilter.empty())
        FilterRows();
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
    m_searchBox->SetBackgroundColour(theme.bgInputField);
    m_searchBox->SetForegroundColour(theme.textPrimary);
    m_listWindow->SetBackgroundColour(theme.bgSidebar);
    m_border->SetBackgroundColour(theme.borderSubtle);

    // Cached rows do not get rebuilt on theme changes, so recolor them here.
    for (auto& [path, row] : m_rows) {
        if (row.titleLabel)
            row.titleLabel->SetForegroundColour(theme.chatAssistant);
        if (row.timeLabel)
            row.timeLabel->SetForegroundColour(theme.textMuted);
        if (row.deleteBtn)
            row.deleteBtn->SetForegroundColour(theme.textMuted);
        if (row.panel)
            row.panel->SetBackgroundColour(GetRowBackground(path));
    }

    RefreshAllRowBackgrounds();
    m_listWindow->Refresh();
}

// ═══════════════════════════════════════════════════════════════════
//  Row background logic
// ═══════════════════════════════════════════════════════════════════

wxColour ConversationSidebar::GetRowBackground(const std::string& filePath) const
{
    if (filePath == m_activeFilePath)
        return m_theme->modelPillBg;
    if (IsSelected(filePath))
        return m_theme->sidebarSelected;
    return m_theme->bgSidebar;
}

void ConversationSidebar::RefreshAllRowBackgrounds()
{
    for (auto& [path, row] : m_rows) {
        if (!row.panel) continue;
        row.panel->SetBackgroundColour(GetRowBackground(path));
        row.panel->Refresh();
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Search / Filter
// ═══════════════════════════════════════════════════════════════════

void ConversationSidebar::FilterRows()
{
    wxString raw = m_searchBox->GetValue();
    m_searchFilter = raw.Lower().ToUTF8().data();

    m_listWindow->Freeze();

    for (auto& [path, row] : m_rows) {
        if (!row.panel) continue;

        if (m_searchFilter.empty()) {
            row.panel->Show();
        }
        else {
            // Case-insensitive substring match on the displayed title
            std::string titleLower = row.displayedTitle;
            std::transform(titleLower.begin(), titleLower.end(),
                           titleLower.begin(), ::tolower);

            bool match = (titleLower.find(m_searchFilter) != std::string::npos);
            row.panel->Show(match);
        }
    }

    m_listWindow->FitInside();
    m_listWindow->Layout();
    m_listWindow->Thaw();
}

void ConversationSidebar::ClearSearch()
{
    m_searchBox->Clear();
    // wxEVT_TEXT fires automatically from Clear(), which calls FilterRows()
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
        entry.filePath = fullPath.ToUTF8().data();

        try {
            std::ifstream file(entry.filePath, std::ios::in);
            if (file.is_open()) {
                std::string line;
                while (std::getline(file, line)) {
                    size_t keyPos = line.find("\"title\"");
                    if (keyPos == std::string::npos) continue;

                    size_t colonPos = line.find(':', keyPos + 7);
                    if (colonPos == std::string::npos) break;
                    size_t openQuote = line.find('"', colonPos + 1);
                    if (openQuote == std::string::npos) break;

                    size_t end = openQuote + 1;
                    while (end < line.size()) {
                        if (line[end] == '"' && line[end - 1] != '\\') break;
                        ++end;
                    }
                    if (end < line.size()) {
                        entry.title = line.substr(openQuote + 1, end - openQuote - 1);
                    }
                    break;
                }
            }
        }
        catch (...) {
            // Skip files we can't read
        }

        if (entry.title.empty()) {
            entry.title = filename.ToUTF8().data();
        }

        wxFileName fn(fullPath);
        fn.GetTimes(nullptr, &entry.modTime, nullptr);

        entries.push_back(entry);
        found = dir.GetNext(&filename);
    }

    std::sort(entries.begin(), entries.end(),
        [](const ConversationEntry& a, const ConversationEntry& b) {
            return a.modTime.IsLaterThan(b.modTime);
        });

    return entries;
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

    row.displayedTitle = entry.title;
    if (row.displayedTitle.size() > 35)
        row.displayedTitle = row.displayedTitle.substr(0, 32) + "...";

    row.displayedTime = RelativeTimeString(entry.modTime);

    row.panel = new wxPanel(m_listWindow, wxID_ANY);
    row.panel->SetBackgroundColour(GetRowBackground(entry.filePath));
    row.panel->SetName(wxString::FromUTF8(entry.filePath));

    auto* panelSizer = new wxBoxSizer(wxVERTICAL);

    // ── Top row: title + trash icon ──────────────────────────────
    auto* topSizer = new wxBoxSizer(wxHORIZONTAL);

    row.titleLabel = new wxStaticText(row.panel, wxID_ANY,
        wxString::FromUTF8(row.displayedTitle));
    row.titleLabel->SetForegroundColour(m_theme->chatAssistant);
    wxFont titleFont = row.titleLabel->GetFont();
    titleFont.SetPointSize(11);
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    row.titleLabel->SetFont(titleFont);
    topSizer->Add(row.titleLabel, 1, wxLEFT | wxTOP, 8);

    // Trash icon — hidden by default, shown on hover
    row.deleteBtn = new wxStaticText(row.panel, wxID_ANY,
        wxString::FromUTF8("\xF0\x9F\x97\x91"));  // 🗑
    row.deleteBtn->SetForegroundColour(m_theme->textMuted);
    row.deleteBtn->SetCursor(wxCursor(wxCURSOR_HAND));
    wxFont delFont = row.deleteBtn->GetFont();
    delFont.SetPointSize(11);
    row.deleteBtn->SetFont(delFont);
    row.deleteBtn->Hide();
    topSizer->Add(row.deleteBtn, 0, wxTOP | wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);

    panelSizer->Add(topSizer, 0, wxEXPAND);

    row.timeLabel = new wxStaticText(row.panel, wxID_ANY,
        wxString::FromUTF8(row.displayedTime));
    row.timeLabel->SetForegroundColour(m_theme->textMuted);
    wxFont timeFont = row.timeLabel->GetFont();
    timeFont.SetPointSize(9);
    row.timeLabel->SetFont(timeFont);
    panelSizer->Add(row.timeLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    row.panel->SetSizer(panelSizer);

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
    };

    row.panel->Bind(wxEVT_LEFT_UP, clickHandler);
    row.titleLabel->Bind(wxEVT_LEFT_UP, clickHandler);
    row.timeLabel->Bind(wxEVT_LEFT_UP, clickHandler);

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

        m_listWindow->CallAfter([this, path]() {
            ShowContextMenu(path);
        });
    };

    row.panel->Bind(wxEVT_RIGHT_UP, rightClickHandler);
    row.titleLabel->Bind(wxEVT_RIGHT_UP, rightClickHandler);
    row.timeLabel->Bind(wxEVT_RIGHT_UP, rightClickHandler);

    // ── Trash icon click → delete this conversation ────────────
    row.deleteBtn->Bind(wxEVT_LEFT_UP, [this, panel = row.panel](wxMouseEvent&) {
        if (m_callbacks.isBusy && m_callbacks.isBusy()) {
            wxBell();
            return;
        }

        std::string path = panel->GetName().ToUTF8().data();
        if (path.empty()) return;
        panel->CallAfter([this, path]() {
            if (m_callbacks.onDeleteRequested)
                m_callbacks.onDeleteRequested({ path });
        });
    });

    // ── Hover: show trash icon + highlight background ────────────
    // Uses CallAfter on leave to prevent flicker when the mouse
    // moves between the panel and its child widgets (title, time, trash).
    auto enterHandler = [panel = row.panel, delBtn = row.deleteBtn, this](wxMouseEvent&) {
        std::string p = panel->GetName().ToUTF8().data();
        if (p != m_activeFilePath && !IsSelected(p)) {
            panel->SetBackgroundColour(m_theme->sidebarHover);
            panel->Refresh();
        }
        if (delBtn && !delBtn->IsShown()) {
            delBtn->Show();
            panel->Layout();
        }
    };

    auto leaveHandler = [panel = row.panel, delBtn = row.deleteBtn, this](wxMouseEvent&) {
        // Defer the hide check so that child-enter events fire first.
        // If the mouse just moved to a child widget, it's still inside
        // the panel and we should keep the hover state.
        panel->CallAfter([panel, delBtn, this]() {
            if (!panel) return;
            wxPoint mouseScreen = wxGetMousePosition();
            wxRect panelScreen = panel->GetScreenRect();
            if (!panelScreen.Contains(mouseScreen)) {
                std::string p = panel->GetName().ToUTF8().data();
                panel->SetBackgroundColour(GetRowBackground(p));
                panel->Refresh();
                if (delBtn && delBtn->IsShown()) {
                    delBtn->Hide();
                    panel->Layout();
                }
            }
        });
    };

    row.panel->Bind(wxEVT_ENTER_WINDOW, enterHandler);
    row.panel->Bind(wxEVT_LEAVE_WINDOW, leaveHandler);
    row.titleLabel->Bind(wxEVT_ENTER_WINDOW, enterHandler);
    row.titleLabel->Bind(wxEVT_LEAVE_WINDOW, leaveHandler);
    row.timeLabel->Bind(wxEVT_ENTER_WINDOW, enterHandler);
    row.timeLabel->Bind(wxEVT_LEAVE_WINDOW, leaveHandler);
    row.deleteBtn->Bind(wxEVT_ENTER_WINDOW, enterHandler);
    row.deleteBtn->Bind(wxEVT_LEAVE_WINDOW, leaveHandler);

    return row;
}

void ConversationSidebar::UpdateRow(RowWidgets& row,
                                    const ConversationEntry& entry)
{
    row.filePath = entry.filePath;
    row.modTime = entry.modTime;

    std::string newTitle = entry.title;
    if (newTitle.size() > 35)
        newTitle = newTitle.substr(0, 32) + "...";

    std::string newTime = RelativeTimeString(entry.modTime);

    if (newTitle != row.displayedTitle) {
        row.displayedTitle = newTitle;
        if (row.titleLabel)
            row.titleLabel->SetLabel(wxString::FromUTF8(newTitle));
    }

    if (newTime != row.displayedTime) {
        row.displayedTime = newTime;
        if (row.timeLabel)
            row.timeLabel->SetLabel(wxString::FromUTF8(newTime));
    }

    if (row.panel)
        row.panel->SetBackgroundColour(GetRowBackground(entry.filePath));
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

void ConversationSidebar::ShowContextMenu(const std::string& /*filePath*/)
{
    wxMenu menu;

    const bool isBusy = m_callbacks.isBusy && m_callbacks.isBusy();

    wxMenuItem* deleteItem = nullptr;
    size_t count = m_selected.size();
    if (count <= 1) {
        deleteItem = menu.Append(wxID_DELETE, "Delete conversation");
    }
    else {
        wxString label = wxString::Format("Delete %zu conversations", count);
        deleteItem = menu.Append(wxID_DELETE, label);
    }

    if (deleteItem && isBusy) {
        deleteItem->Enable(false);
    }

    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (m_callbacks.onDeleteRequested && !m_selected.empty()) {
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

    return dt.Format("%b %d").ToUTF8().data();
}