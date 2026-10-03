#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>

#include <string>
#include <vector>

class EndpointStore;
class SecretsStore;
struct ThemeData;

class EndpointsDialog : public wxDialog
{
public:
    EndpointsDialog(wxWindow* parent,
                    EndpointStore* store,
                    SecretsStore* secretsStore,
                    const ThemeData& theme);

    const std::string& GetModelToUse() const { return m_modelToUse; }

private:
    std::string m_modelToUse;
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
    SecretsStore*    m_secretsStore = nullptr;
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

// Call before opening any editor that can save connections or keys.  When
// either settings file failed to load, explains that saving is blocked to
// protect it and offers Retry (reload from disk), Start Fresh (explicitly
// allow replacing it; a copy was kept when possible) or Cancel.  Returns
// false on Cancel -- the caller must not open the editor.
bool LbEnsureConnectionStoresWritable(wxWindow* parent, EndpointStore* endpoints,
                                      SecretsStore* secrets);

// Shared guided setup. Keys are entered in the dialog; never pass them through chat.
// Connect checks credentials; Save and use model persists and returns the selection.
// Cancel leaves stores unchanged.
bool LbShowAIConnectionSetup(wxWindow* parent, EndpointStore* store, SecretsStore* secrets,
    const ThemeData& theme, const std::string& provider = "openrouter", const std::string& modelSearch = "", std::string* modelToUse = nullptr);
