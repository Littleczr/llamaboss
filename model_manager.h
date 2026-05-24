// model_manager.h
#pragma once

#include <wx/wx.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/button.h>
#include <string>
#include <vector>

// Forward declarations
struct ThemeData;

// ── Model Manager dialog ──────────────────────────────────────────
// Lists .gguf files from the models directory.
// Supports delete (removes file) and opening the models folder.
//
// Button styling matches the rest of the dialog family:
//   Refresh / Open folder → solid accent wxButtons
//   Close → flat borderless wxButton with muted text
//
// Delete lives on the list itself — Del key while a row is selected,
// or right-click context menu. Same pattern as ProjectAttachDialog,
// so destructive actions don't dominate the action row visually.
class ModelManagerDialog : public wxDialog
{
public:
    ModelManagerDialog(wxWindow* parent, const ThemeData* theme = nullptr);
    ~ModelManagerDialog() = default;

private:
    void CreateControls();
    void RefreshModelList();

    // Event handlers
    void OnDeleteClicked(wxCommandEvent& ev);
    void OnRefreshClicked(wxCommandEvent& ev);
    void OnOpenFolderClicked(wxCommandEvent& ev);
    void OnClose(wxCommandEvent& ev);
    void OnContextMenu(wxContextMenuEvent& ev);   // Right-click on list
    void OnCharHook(wxKeyEvent& ev);              // Del key catch

    wxListCtrl*   m_modelList     = nullptr;
    wxButton*     m_refreshButton = nullptr;  // solid accent
    wxStaticText* m_statusText    = nullptr;

    const ThemeData* m_theme;

    // Parallel to list rows: full GGUF paths
    std::vector<std::string> m_modelPaths;

    wxDECLARE_EVENT_TABLE();
};
