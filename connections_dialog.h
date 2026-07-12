// connections_dialog.h
//
// Modal sub-dialog opened from Settings → Connections → [ manage ].
// Lists every provider/key pair from SecretsStore, lets the user
// add a new connection, edit an existing one (replace the value or
// switch between raw / $env), or delete one outright.
//
// All mutations write through to the in-memory SecretsStore; the
// caller is responsible for calling Save() when the user accepts
// the parent Settings dialog.
//
#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>

class SecretsStore;
struct ThemeData;

class ConnectionsDialog : public wxDialog
{
public:
    ConnectionsDialog(wxWindow* parent,
                      SecretsStore* store,
                      const ThemeData& theme);

private:
    void OnAdd(wxCommandEvent& evt);
    void OnEdit(wxCommandEvent& evt);
    void OnDelete(wxCommandEvent& evt);
    void OnClose(wxCommandEvent& evt);
    void OnItemActivated(wxListEvent& evt);
    void OnSelectionChanged(wxListEvent& evt);

    void RebuildList(const wxString& selectProvider = wxEmptyString,
                     const wxString& selectKey = wxEmptyString);
    void UpdateButtonState();
    void ApplyTheme();

    // Opens a small modal to add or edit one connection.  Returns
    // true if the user confirmed; fills outProvider/outKey/outValue/
    // outIsEnvRef accordingly.  When editing, the values are seeded
    // from the current row.
    bool PromptForConnection(const wxString& title,
                             wxString& ioProvider,
                             wxString& ioKey,
                             wxString& ioValue,
                             bool&     ioIsEnvRef,
                             bool      allowEmptyDirectValue = false);

    SecretsStore*   m_store   = nullptr;
    const ThemeData* m_theme  = nullptr;

    wxListCtrl* m_list    = nullptr;
    wxButton*   m_addBtn  = nullptr;
    wxButton*   m_editBtn = nullptr;
    wxButton*   m_delBtn  = nullptr;
    wxButton*   m_closeBtn = nullptr;

    wxDECLARE_EVENT_TABLE();
};
