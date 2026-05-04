// conversation_sidebar.h
// Collapsible sidebar that lists saved conversations, grouped by their
// attached project.  Owns the sidebar UI panel, conversation list, and
// selection state.  Communicates back to the host frame via std::function
// callbacks.
//
// Layout model:
//   ▼ <Project Name>
//       chat row
//       chat row
//   ▶ <Other Project>   (5)
//   ▼ Unassigned
//       chat row
//
// Selection model (file-explorer style):
//   Click          = select one, load it
//   Ctrl+Click     = toggle one in/out of selection (no load)
//   Shift+Click    = range-select from anchor to clicked item
//   Right-click    = context menu; preserves multi-selection if target is selected
//   Delete menu    = deletes all selected conversations
//
// Project headers are inert with respect to selection.  They participate
// only in their own click-to-collapse and right-click → popup paths.

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
class HeaderDropTarget;  // Defined in conversation_sidebar.cpp; friend below.

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

        // Fired when the user toggles a project section's collapsed state.
        // The host frame persists the list to AppState.  IDs are project
        // IDs, plus the sentinel "__unassigned__" for the Unassigned group.
        std::function<void(const std::vector<std::string>& collapsedIds)>
            onCollapsedProjectsChanged;

        // Right-click on a chat row.  The frame builds the popup
        // (Move to project ▸ / Delete / etc.) so the sidebar stays
        // free of project-data dependencies.  |paths| is the current
        // multi-selection (or just the right-clicked path if no
        // multi-selection includes it); |anchor| is the window the
        // popup should be parented to.
        std::function<void(const std::vector<std::string>& paths,
                           wxWindow* anchor)>
            onChatContextMenuRequested;

        // Right-click on a project header.  |groupId| is the real
        // project ID, or empty when the header is the Unassigned
        // section (the frame may choose to show no menu in that case).
        std::function<void(const std::string& groupId, wxWindow* anchor)>
            onProjectHeaderContextMenuRequested;

        // Native drag-and-drop: one or more chat rows were dragged
        // and dropped onto a project header.  |targetProjectId| is
        // empty when the drop target was the Unassigned section.
        // Frame typically forwards into MoveChatsToProject.
        std::function<void(const std::vector<std::string>& paths,
                           const std::string& targetProjectId)>
            onChatsDroppedOnProject;
    };

    ConversationSidebar(wxWindow* parent, const ThemeData& theme,
                        const Callbacks& callbacks,
                        const std::vector<std::string>& initialCollapsed = {});
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
    // The drop target installed on each project header lives in the
    // sidebar's .cpp anonymous namespace and needs to call back into
    // OnDragHoverHeader / OnChatsDroppedOnHeader.  Friending keeps
    // those routing methods private to the rest of the world while
    // still letting the drop target reach them.
    friend class ::HeaderDropTarget;

    // Sentinel project id used for the Unassigned group.  Must not collide
    // with any real project id (sanitized to lowercase alphanumeric +
    // hyphen, never starts/ends with double underscore).
    static constexpr const char* kUnassignedId = "__unassigned__";

    // ── Internal data ────────────────────────────────────────────
    struct ConversationEntry {
        std::string filePath;
        std::string title;
        wxDateTime  modTime;
        std::string projectId;     // Empty → Unassigned group
        std::string projectName;   // Display name; mirrors what's in the JSON
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

        // The group this row currently belongs to.  Used so a collapse
        // toggle can find every chat row under a header in O(rows).
        std::string groupId;        // Real project id, or kUnassignedId
    };

    struct HeaderWidgets {
        wxPanel*      panel = nullptr;
        wxStaticText* triangle = nullptr;   // ▼ / ▶
        wxStaticText* nameLabel = nullptr;
        wxStaticText* countLabel = nullptr; // " (N)" while collapsed; empty otherwise

        std::string groupId;        // Real project id, or kUnassignedId
        std::string displayName;
        int         chatCount = 0;
        bool        collapsed = false;
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

    // Cached project header widgets keyed by group id (real project id
    // or kUnassignedId).  Same incremental-refresh philosophy as m_rows:
    // create on first sighting, update display on subsequent refreshes,
    // destroy when the group disappears.
    std::unordered_map<std::string, HeaderWidgets> m_projectHeaders;

    // Project ids whose section is currently collapsed.  Mirrors what
    // the host frame has persisted in AppState; mutated locally on click,
    // host is notified through onCollapsedProjectsChanged.
    std::set<std::string> m_collapsedGroups;

    // Sections force-expanded for this Refresh because they contain the
    // active chat or a search match.  These do NOT mutate
    // m_collapsedGroups, so the user's persisted intent is preserved.
    // Recomputed at the start of every Refresh().
    std::set<std::string> m_overrideExpandedGroups;

    // ── Metadata cache keyed by file path ─────────────────────────
    // ScanConversations() used to read the first ~2KB of every JSON on
    // every refresh to pluck out the "title" field. With a few hundred
    // conversations that's a visible UI hitch every time a message
    // completes (AutoSaveConversation → sidebar Refresh).
    //
    // Title and project association only change when the file is
    // rewritten, which bumps mtime.  So we cache them per path and skip
    // the file read when mtime hasn't moved.  Stat calls remain, but
    // they're microseconds.
    struct MetaCacheEntry {
        std::string title;
        std::string projectId;
        std::string projectName;
        time_t      mtime = 0;   // Unix seconds, from wxDateTime::GetTicks()
    };
    std::unordered_map<std::string, MetaCacheEntry> m_metaCache;

    // ── Helpers ──────────────────────────────────────────────────
    std::vector<ConversationEntry> ScanConversations();
    RowWidgets CreateRow(const ConversationEntry& entry);
    void UpdateRow(RowWidgets& row, const ConversationEntry& entry);
    void RemoveRow(const std::string& filePath);
    void ShowContextMenu(const std::string& filePath);

    // Project header CRUD
    HeaderWidgets CreateProjectHeader(const std::string& groupId,
                                      const std::string& displayName,
                                      int chatCount,
                                      bool collapsed);
    void UpdateProjectHeader(HeaderWidgets& header,
                             const std::string& displayName,
                             int chatCount,
                             bool collapsed);
    void RemoveProjectHeader(const std::string& groupId);
    void OnProjectHeaderClicked(const std::string& groupId);
    bool IsGroupCollapsed(const std::string& groupId) const;
    std::vector<std::string> CollapsedGroupsAsVector() const;

    // ── Drag-and-drop (chat rows → project headers) ─────────────
    //
    // Drag is initiated from chat row LEFT_DOWN + MOTION-past-threshold;
    // the existing LEFT_UP click handler still fires for plain clicks
    // because we don't consume the down event.  Drop targets installed
    // on header panels (including Unassigned) translate the dropped
    // payload back into a chat-paths list and surface it to the frame
    // via onChatsDroppedOnProject.
    struct DragState {
        bool        pressActive = false;  // LEFT_DOWN seen, no drag started yet
        wxPoint     pressPoint;
        std::string pressPath;            // The row that received LEFT_DOWN
    };
    DragState m_dragState;

    void MaybeStartDragFrom(const std::string& path, wxWindow* origin);
    std::vector<std::string> PathsForDragFrom(const std::string& path) const;

    // Called by the header drop target during drag — visual feedback only.
    void OnDragHoverHeader(const std::string& groupId, bool hovering);
    // Called by the header drop target when a drop completes.
    void OnChatsDroppedOnHeader(const std::vector<std::string>& paths,
                                const std::string& groupId);

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