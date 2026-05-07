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

#include <fstream>
#include <algorithm>

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

    // ── Sidebar-wide Delete-key shortcut ─────────────────────────
    // CHAR_HOOK fires on every level of the widget hierarchy from the
    // top-level window down to the focused window, so this handler
    // catches Del whenever focus is anywhere inside the sidebar — the
    // scrolled list, a row panel, the search box, or the New Chat
    // button.  KEY_DOWN bound on m_listWindow alone wouldn't reliably
    // catch the event because wxPanel::SetFocus() redirects focus to
    // the first focusable child, so the scrolled window rarely actually
    // owns focus after a click.
    //
    // The Del key is intentionally not consumed when focus is on a
    // wxTextCtrl (the search box, primarily) so editing keystrokes
    // continue to work normally there.
    m_panel->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() != WXK_DELETE) {
            e.Skip();
            return;
        }

        // Don't intercept when typing in any text input within the sidebar.
        wxWindow* focused = wxWindow::FindFocus();
        if (dynamic_cast<wxTextCtrl*>(focused)) {
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

    m_activeFilePath = activeFilePath;

    // Scan and sort
    auto entries = ScanConversations();

    // ── Group by project ─────────────────────────────────────────
    // Walk the (mod-time-sorted) entries once and bucket them by group
    // id.  An empty projectId in the entry maps to kUnassignedId so the
    // sentinel flows through the rest of the layout code unchanged.
    //
    // Group display order:
    //   1. Real projects, alphabetical by display name (case-insensitive).
    //   2. Unassigned, always last.
    //
    // Within each group, entries keep ScanConversations()'s descending
    // mod-time order — the most recently touched chat is at the top of
    // its own group, which matches the expected "what was I just
    // working on" navigation pattern.
    struct Group {
        std::string id;
        std::string displayName;
        std::vector<const ConversationEntry*> entries;
    };
    std::unordered_map<std::string, Group> groupsById;

    for (const auto& e : entries) {
        const std::string id = e.projectId.empty() ? kUnassignedId : e.projectId;
        auto& g = groupsById[id];
        if (g.id.empty()) {
            g.id = id;
            g.displayName = (id == kUnassignedId)
                ? std::string("Unassigned")
                : (e.projectName.empty() ? std::string("(unnamed project)")
                                         : e.projectName);
        }
        g.entries.push_back(&e);
    }

    std::vector<Group*> orderedGroups;
    orderedGroups.reserve(groupsById.size());
    for (auto& [id, g] : groupsById) {
        if (id != kUnassignedId) orderedGroups.push_back(&g);
    }
    std::sort(orderedGroups.begin(), orderedGroups.end(),
        [](const Group* a, const Group* b) {
            std::string an = a->displayName;
            std::string bn = b->displayName;
            std::transform(an.begin(), an.end(), an.begin(), ::tolower);
            std::transform(bn.begin(), bn.end(), bn.begin(), ::tolower);
            return an < bn;
        });
    if (auto it = groupsById.find(kUnassignedId); it != groupsById.end()) {
        orderedGroups.push_back(&it->second);
    }

    // ── Recompute override-expanded set for this render ──────────
    // A collapsed section is force-expanded for display only when it
    // contains the active chat or (later) a search match.  This does not
    // touch m_collapsedGroups, so the user's persisted intent survives.
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

    // Rebuild ordered path list for Shift+Click range logic.  Headers
    // are NOT included — selection still operates on chat rows only.
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

    // Remove headers for groups that disappeared
    std::set<std::string> validGroupIds;
    for (const auto* g : orderedGroups) validGroupIds.insert(g->id);
    for (auto it = m_projectHeaders.begin(); it != m_projectHeaders.end(); ) {
        if (validGroupIds.find(it->first) == validGroupIds.end()) {
            RemoveProjectHeader(it->first);
            it = m_projectHeaders.erase(it);
        }
        else {
            ++it;
        }
    }

    // ── Lay out: header → its chats → next header → ... ──────────
    size_t sizerIdx = 0;
    auto placeAtSizerIndex = [&](wxPanel* panel) {
        wxSizerItem* currentItem =
            (sizerIdx < m_listSizer->GetItemCount())
                ? m_listSizer->GetItem(sizerIdx)
                : nullptr;
        wxWindow* currentWindow = currentItem ? currentItem->GetWindow() : nullptr;
        if (currentWindow != panel) {
            m_listSizer->Detach(panel);
            m_listSizer->Insert(sizerIdx, panel, 0, wxEXPAND);
        }
        ++sizerIdx;
    };

    for (const auto* g : orderedGroups) {
        const bool collapsed = IsGroupCollapsed(g->id);
        const int  chatCount = static_cast<int>(g->entries.size());

        // Create-or-update the header for this group
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
            placeAtSizerIndex(headerIt->second.panel);
        }

        // Chat rows for this group.  Hidden rows are not added to
        // m_orderedPaths so Shift+Click range selection can't span
        // across a collapsed section invisibly.
        for (const auto* e : g->entries) {
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

            wxPanel* panel = rowIt->second.panel;
            if (!panel) continue;

            if (collapsed) {
                panel->Hide();
                // Detach so collapsed rows don't take any vertical
                // space in the sizer at all.
                if (m_listSizer->GetItem(panel)) {
                    m_listSizer->Detach(panel);
                }
                continue;
            }

            panel->Show();
            placeAtSizerIndex(panel);
            m_orderedPaths.push_back(entry.filePath);

            panel->SetBackgroundColour(GetRowBackground(entry.filePath));
            panel->Refresh();
        }
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

    // Recolor cached project headers — same lifetime story as rows.
    for (auto& [id, header] : m_projectHeaders) {
        if (header.panel)
            header.panel->SetBackgroundColour(theme.bgSidebar);
        if (header.triangle)
            header.triangle->SetForegroundColour(theme.textMuted);
        if (header.nameLabel)
            header.nameLabel->SetForegroundColour(theme.textPrimary);
        if (header.countLabel)
            header.countLabel->SetForegroundColour(theme.textMuted);
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
    std::string newFilter = raw.Lower().ToUTF8().data();
    const bool wasFiltered  = !m_searchFilter.empty();
    const bool nowFiltered  = !newFilter.empty();
    const bool transition   = (wasFiltered != nowFiltered);

    m_searchFilter = newFilter;

    // Crossing the empty/non-empty boundary changes how IsGroupCollapsed
    // behaves and therefore changes the sizer layout.  A full Refresh
    // is the cleanest way to re-emit headers and chat rows in the right
    // expanded/collapsed state.  Once back inside the filtered or
    // unfiltered regime, per-row visibility tweaks are enough.
    if (transition) {
        Refresh(m_activeFilePath);
        if (!nowFiltered) return;  // Cleared filter — Refresh did the work
    }

    m_listWindow->Freeze();

    // Track which groups have at least one visible chat under the
    // current filter, so we can hide headers for groups that match
    // nothing.
    std::set<std::string> groupsWithMatches;

    for (auto& [path, row] : m_rows) {
        if (!row.panel) continue;

        bool show = true;
        if (nowFiltered) {
            std::string titleLower = row.displayedTitle;
            std::transform(titleLower.begin(), titleLower.end(),
                           titleLower.begin(), ::tolower);
            show = (titleLower.find(m_searchFilter) != std::string::npos);
        }

        row.panel->Show(show);
        if (show && !row.groupId.empty())
            groupsWithMatches.insert(row.groupId);
    }

    // Hide headers whose groups have no surviving chat rows.  When the
    // filter is empty, every header that exists in m_projectHeaders
    // should be visible (Refresh already removed dead ones).
    for (auto& [id, header] : m_projectHeaders) {
        if (!header.panel) continue;
        if (!nowFiltered) {
            header.panel->Show();
        }
        else {
            header.panel->Show(groupsWithMatches.count(id) != 0);
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

        wxFileName fn(fullPath);
        fn.GetTimes(nullptr, &entry.modTime, nullptr);
        time_t thisMtime = entry.modTime.IsValid() ? entry.modTime.GetTicks() : 0;

        // ── Cache lookup ─────────────────────────────────────────
        // If we've seen this path before and its mtime hasn't moved,
        // reuse the cached metadata.  The JSON file hasn't been
        // rewritten so title and project association are unchanged.
        auto cachedIt = m_metaCache.find(entry.filePath);
        bool cacheHit = (cachedIt != m_metaCache.end() &&
                         cachedIt->second.mtime == thisMtime &&
                         thisMtime != 0);

        if (cacheHit) {
            entry.title       = cachedIt->second.title;
            entry.projectId   = cachedIt->second.projectId;
            entry.projectName = cachedIt->second.projectName;
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
                    auto extractStringField = [](const std::string& line,
                                                 const std::string& key,
                                                 std::string& out) -> bool {
                        std::string needle = "\"" + key + "\"";
                        size_t keyPos = line.find(needle);
                        if (keyPos == std::string::npos) return false;
                        size_t colonPos = line.find(':', keyPos + needle.size());
                        if (colonPos == std::string::npos) return false;
                        size_t openQuote = line.find('"', colonPos + 1);
                        if (openQuote == std::string::npos) return false;
                        size_t end = openQuote + 1;
                        while (end < line.size()) {
                            if (line[end] == '"' && line[end - 1] != '\\') break;
                            ++end;
                        }
                        if (end >= line.size()) return false;
                        out = line.substr(openQuote + 1, end - openQuote - 1);
                        return true;
                    };

                    std::string line;
                    while (std::getline(file, line)) {
                        if (line.find("\"messages\"") != std::string::npos) break;

                        if (!sawTitle && extractStringField(line, "title", entry.title))
                            sawTitle = true;
                        if (!sawProjectId && extractStringField(line, "project_id", entry.projectId))
                            sawProjectId = true;
                        if (!sawProjectName && extractStringField(line, "project_name", entry.projectName))
                            sawProjectName = true;

                        if (sawTitle && sawProjectId && sawProjectName) break;
                    }
                }
            }
            catch (...) {
                // Skip files we can't read
            }

            if (entry.title.empty()) {
                entry.title = filename.ToUTF8().data();
            }

            // Populate cache for next refresh
            m_metaCache[entry.filePath] = {
                entry.title, entry.projectId, entry.projectName, thisMtime
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

    // Monospace font matches the project status strip + the box-drawing
    // sidebar idiom.  Bold weight keeps the section break visually
    // distinct from chat titles.
    wxFont monoFont(11, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                    wxFONTWEIGHT_BOLD, false, "Consolas");

    header.triangle = new wxStaticText(header.panel, wxID_ANY,
        wxString::FromUTF8(collapsed ? kTriangleCollapsed : kTriangleExpanded));
    header.triangle->SetForegroundColour(m_theme->textMuted);
    header.triangle->SetFont(monoFont);
    header.triangle->SetCursor(wxCursor(wxCURSOR_HAND));
    sizer->Add(header.triangle, 0,
               wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 8);

    header.nameLabel = new wxStaticText(header.panel, wxID_ANY,
        wxString::FromUTF8(displayName));
    header.nameLabel->SetForegroundColour(m_theme->textPrimary);
    header.nameLabel->SetFont(monoFont);
    header.nameLabel->SetCursor(wxCursor(wxCURSOR_HAND));
    sizer->Add(header.nameLabel, 0,
               wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 6);

    // Count label is only populated when collapsed, but we always
    // create the widget so toggling doesn't allocate/destroy.
    header.countLabel = new wxStaticText(header.panel, wxID_ANY, "");
    header.countLabel->SetForegroundColour(m_theme->textMuted);
    header.countLabel->SetFont(monoFont);
    header.countLabel->SetCursor(wxCursor(wxCURSOR_HAND));
    sizer->Add(header.countLabel, 0,
               wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 4);

    sizer->AddStretchSpacer(1);

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
    header.countLabel->Bind(wxEVT_LEFT_UP, clickToggle);

    // ── Right-click → header context menu ──────────────────────
    // Frame builds the popup (Attach this chat / open folders /
    // delete project).  Unassigned sections pass an empty groupId
    // up so the frame can either show a slim menu or nothing.
    const std::string contextId =
        (groupId == kUnassignedId) ? std::string() : groupId;
    auto rightClickMenu =
        [this, contextId, panel = header.panel](wxMouseEvent&) {
            if (m_callbacks.onProjectHeaderContextMenuRequested) {
                m_callbacks.onProjectHeaderContextMenuRequested(contextId, panel);
            }
        };

    header.panel->Bind(wxEVT_RIGHT_UP, rightClickMenu);
    header.triangle->Bind(wxEVT_RIGHT_UP, rightClickMenu);
    header.nameLabel->Bind(wxEVT_RIGHT_UP, rightClickMenu);
    header.countLabel->Bind(wxEVT_RIGHT_UP, rightClickMenu);

    // ── Drop target ─────────────────────────────────────────────
    // Owned by the panel (wxWindow::SetDropTarget takes ownership).
    // Unassigned sections receive drops too — that's how chats leave
    // a project via DnD.  The OS routes drops over child static-text
    // widgets up to the parent panel since the children have no drop
    // target of their own.
    header.panel->SetDropTarget(new HeaderDropTarget(this, groupId));

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

    // Show "(N)" only while collapsed — when expanded, the rows
    // themselves convey the count and the trailing number is noise.
    if (header.countLabel) {
        if (collapsed && chatCount > 0) {
            std::string label = "  (" + std::to_string(chatCount) + ")";
            header.countLabel->SetLabel(wxString::FromUTF8(label));
        }
        else {
            header.countLabel->SetLabel("");
        }
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

void ConversationSidebar::OnProjectHeaderClicked(const std::string& groupId)
{
    if (groupId.empty()) return;

    if (m_collapsedGroups.count(groupId)) {
        m_collapsedGroups.erase(groupId);
    }
    else {
        m_collapsedGroups.insert(groupId);
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
    wxColour bg = hovering ? m_theme->sidebarHover : m_theme->bgSidebar;
    it->second.panel->SetBackgroundColour(bg);
    it->second.panel->Refresh();
}

void ConversationSidebar::OnChatsDroppedOnHeader(
    const std::vector<std::string>& paths,
    const std::string& groupId)
{
    if (!m_callbacks.onChatsDroppedOnProject) return;
    if (paths.empty()) return;

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
    // Indented past the header triangle so chats sit visually under
    // the project name.  The leading indent is intentionally larger
    // than the header's triangle padding (8px) — chat titles align
    // roughly under the project name's first character.
    topSizer->Add(row.titleLabel, 1, wxLEFT | wxTOP, 24);

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
    panelSizer->Add(row.timeLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 24);

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

        // Park focus on the scrolled list itself.  Plain SetFocus() would
        // redirect to the first focusable child (per wxPanel docs);
        // SetFocusIgnoringChildren forces focus to actually land on
        // m_listWindow.  This isn't strictly required for the Del-key
        // shortcut (CHAR_HOOK on m_panel catches keys from any sidebar
        // descendant) but it ensures focus stays inside the sidebar
        // hierarchy after a click rather than wherever it was before.
        m_listWindow->SetFocusIgnoringChildren();
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

        // See clickHandler above — park focus on the list so the Del-key
        // CHAR_HOOK on m_panel will fire after the context menu closes.
        m_listWindow->SetFocusIgnoringChildren();

        m_listWindow->CallAfter([this, path]() {
            ShowContextMenu(path);
        });
    };

    row.panel->Bind(wxEVT_RIGHT_UP, rightClickHandler);
    row.titleLabel->Bind(wxEVT_RIGHT_UP, rightClickHandler);
    row.timeLabel->Bind(wxEVT_RIGHT_UP, rightClickHandler);

    // ── Drag initiation ─────────────────────────────────────────
    // LEFT_DOWN records the press (and lets evt.Skip() carry through
    // so LEFT_UP still fires for plain clicks).  MOTION compares
    // distance against the threshold and kicks off DoDragDrop.
    //
    // We only stash the path here — actual drag payload (which paths
    // travel) is decided at drag-start time so multi-selection
    // changes between press and threshold-crossing are honored.
    auto pressDown = [this, path = entry.filePath](wxMouseEvent& evt) {
        m_dragState.pressActive = true;
        m_dragState.pressPath   = path;
        m_dragState.pressPoint  = evt.GetPosition();
        evt.Skip();  // Don't suppress LEFT_UP click handling
    };

    auto pressMotion = [this](wxMouseEvent& evt) {
        if (!m_dragState.pressActive) {
            evt.Skip();
            return;
        }
        if (!evt.LeftIsDown()) {
            // Button released without us ever crossing the threshold
            // (or a focus loss interrupted the press).  Bail out.
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

        // Past threshold — start drag.  Clear pressActive first so
        // we don't re-enter from any motion events that fire while
        // DoDragDrop is running.
        const std::string fromPath = m_dragState.pressPath;
        m_dragState.pressActive = false;

        wxWindow* origin = dynamic_cast<wxWindow*>(evt.GetEventObject());
        MaybeStartDragFrom(fromPath, origin);
    };

    auto pressEnd = [this](wxMouseEvent& evt) {
        m_dragState.pressActive = false;
        evt.Skip();
    };

    row.panel->Bind(wxEVT_LEFT_DOWN,  pressDown);
    row.panel->Bind(wxEVT_MOTION,     pressMotion);
    row.panel->Bind(wxEVT_LEFT_UP,    pressEnd);
    row.titleLabel->Bind(wxEVT_LEFT_DOWN, pressDown);
    row.titleLabel->Bind(wxEVT_MOTION,    pressMotion);
    row.titleLabel->Bind(wxEVT_LEFT_UP,   pressEnd);
    row.timeLabel->Bind(wxEVT_LEFT_DOWN,  pressDown);
    row.timeLabel->Bind(wxEVT_MOTION,     pressMotion);
    row.timeLabel->Bind(wxEVT_LEFT_UP,    pressEnd);

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