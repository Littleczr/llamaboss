// model_manager.h
#pragma once

#include <wx/wx.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <string>
#include <vector>

// Forward declarations
struct ThemeData;

// ── Model Manager dialog ──────────────────────────────────────────
// Lists .gguf files from the models directory.
// Supports delete (removes file) and opening the models folder.
// Pull/Create are deferred to Phase 2.
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

    wxListCtrl*   m_modelList;
    wxButton*     m_deleteButton;
    wxButton*     m_refreshButton;
    wxStaticText* m_statusText;

    const ThemeData* m_theme;

    // Parallel to list rows: full GGUF paths
    std::vector<std::string> m_modelPaths;

    wxDECLARE_EVENT_TABLE();
};
