// endpoints_dialog.h
//
// Modal sub-dialog opened from Settings -> Remote Endpoints -> [ Manage ].
// Lists every configured remote inference endpoint from EndpointStore,
// lets the user add a new endpoint, edit an existing one (URL, auth,
// which SecretsStore key it uses, protocol, and its model list), or
// delete one.
//
// All mutations write through to the in-memory EndpointStore; the caller
// (SettingsDialog) is responsible for calling Save() when the user
// accepts. The API key itself lives in SecretsStore and is managed in
// the Connections dialog — this dialog only references it by name.
//
#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>

#include <string>
#include <vector>

class EndpointStore;
struct ThemeData;

class EndpointsDialog : public wxDialog
{
public:
    EndpointsDialog(wxWindow* parent,
                    EndpointStore* store,
                    const ThemeData& theme);

private:
    void OnAdd(wxCommandEvent& evt);
    void OnEdit(wxCommandEvent& evt);
    void OnDelete(wxCommandEvent& evt);
    void OnClose(wxCommandEvent& evt);
    void OnItemActivated(wxListEvent& evt);
    void OnSelectionChanged(wxListEvent& evt);

    void RebuildList(const wxString& selectId = wxEmptyString);
    void UpdateButtonState();
    void ApplyTheme();

    EndpointStore*   m_store = nullptr;
    const ThemeData* m_theme = nullptr;

    wxListCtrl* m_list     = nullptr;
    wxButton*   m_addBtn   = nullptr;
    wxButton*   m_editBtn  = nullptr;
    wxButton*   m_delBtn   = nullptr;
    wxButton*   m_closeBtn = nullptr;

    // Row index -> endpoint id, kept parallel to the list so Edit/Delete
    // resolve the selection without relying on the (display-name) column.
    std::vector<std::string> m_rowIds;

    wxDECLARE_EVENT_TABLE();
};
