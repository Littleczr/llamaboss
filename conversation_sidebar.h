// conversation_sidebar.h
// Collapsible sidebar that lists saved conversations.
// Owns the sidebar UI panel, conversation list, and selection state.
// Communicates back to the host frame via std::function callbacks.
//
// Selection model (file-explorer style):
//   Click          = select one, load it
//   Ctrl+Click     = toggle one in/out of selection (no load)
//   Shift+Click    = range-select from anchor to clicked item
//   Right-click    = context menu; preserves multi-selection if target is selected
//   Delete menu    = deletes all selected conversations

#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <functional>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>

// Forward declarations
struct ThemeData;

class ConversationSidebar
{
public:
    // Callbacks the host frame provides
    struct Callbacks {
        std::function<void(const std::string& path)>              onConversationClicked;
        std::function<void()>                                      onNewChatClicked;
        std::function<void(const std::vector<std::string>& paths)> onDeleteRequested;
        std::function<bool()>                                      isBusy;
        std::function<void(int width)>                             onResized;  // sidebar width changed
    };

    ConversationSidebar(wxWindow* parent, const ThemeData& theme,
                        const Callbacks& callbacks);
    ~ConversationSidebar() = default;

    // ── Visibility ───────────────────────────────────────────────
    void Show();
    void Hide();
    bool IsVisible() const;
    void Toggle();

    // ── Resizing ──────────────────────────────────────────────────
    int  GetWidth() const;
    void SetWidth(int w);

    // ── Content ──────────────────────────────────────────────────
    // Rebuild the conversation list from disk.
    // activeFilePath: the currently loaded conversation (highlighted).
    void Refresh(const std::string& activeFilePath);

    // ── Selection ────────────────────────────────────────────────
    void ClearSelection();

    // ── Theming ──────────────────────────────────────────────────
    void ApplyTheme(const ThemeData& theme);

    // ── Layout access ────────────────────────────────────────────
    // Returns the top-level panel so the host frame can add it to a sizer.
    wxPanel* GetPanel() const { return m_panel; }

private:
    // ── Internal data ────────────────────────────────────────────
    struct ConversationEntry {
        std::string filePath;
        std::string title;
        wxDateTime  modTime;
    };

    struct RowWidgets {
        wxPanel*      panel = nullptr;
        wxStaticText* titleLabel = nullptr;
        wxStaticText* timeLabel = nullptr;
        wxStaticText* deleteBtn = nullptr;   // Trash icon — visible on hover

        std::string filePath;
        std::string displayedTitle;
        std::string displayedTime;
        wxDateTime  modTime;
    };

    // ── UI widgets ───────────────────────────────────────────────
    wxPanel*          m_panel;            // Outer panel (contains content + border)
    wxPanel*          m_content;          // Content area (button + list)
    wxPanel*          m_border;           // Drag handle / vertical border on right edge

    // ── Drag-resize state ─────────────────────────────────────────
    bool m_dragging = false;
    int  m_dragStartX = 0;
    int  m_dragStartWidth = 0;
    static constexpr int BORDER_WIDTH = 5;
    static constexpr int MIN_WIDTH = 180;
    static constexpr int MAX_WIDTH = 600;
    wxButton*         m_newChatButton;    // "+ New Chat" button
    wxTextCtrl*       m_searchBox;        // Search/filter conversations
    wxScrolledWindow* m_listWindow;       // Scrollable conversation list
    wxBoxSizer*       m_listSizer;        // Sizer inside m_listWindow

    // ── State ────────────────────────────────────────────────────
    Callbacks    m_callbacks;
    const ThemeData* m_theme;             // Current theme (not owned)
    std::string  m_activeFilePath;        // Currently loaded conversation
    std::string  m_searchFilter;          // Current search text (lowercase)

    // ── Multi-select state ───────────────────────────────────────
    std::set<std::string>  m_selected;    // Set of selected file paths
    std::string            m_anchorPath;  // Shift-click anchor
    // Ordered list of file paths matching the current visual order,
    // rebuilt on every Refresh(). Needed for Shift+Click range logic.
    std::vector<std::string> m_orderedPaths;

    // Cached row widgets keyed by full conversation file path.
    // Lets Refresh() update only rows that changed instead of rebuilding
    // the entire widget tree on every refresh.
    std::unordered_map<std::string, RowWidgets> m_rows;

    // ── Helpers ──────────────────────────────────────────────────
    std::vector<ConversationEntry> ScanConversations() const;
    RowWidgets CreateRow(const ConversationEntry& entry);
    void UpdateRow(RowWidgets& row, const ConversationEntry& entry);
    void RemoveRow(const std::string& filePath);
    void ShowContextMenu(const std::string& filePath);

    // Selection helpers
    void SelectRange(const std::string& from, const std::string& to);
    bool IsSelected(const std::string& path) const;
    wxColour GetRowBackground(const std::string& filePath) const;
    void RefreshAllRowBackgrounds();

    // Search helpers
    void FilterRows();
    void ClearSearch();

    // Walk up from a child widget to find the panel holding the .json path
    static std::string PathFromWidget(wxWindow* win, wxWindow* stop);

    static std::string RelativeTimeString(const wxDateTime& dt);
};