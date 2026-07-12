#pragma once

#include "theme.h"

#include <wx/wx.h>
#include <functional>

// Small themed replacement for wxTextEntryDialog. Native wxTextEntryDialog
// ignores the app palette on Windows, which makes simple prompts look
// detached from themed modal flows such as Attach / Manage Project.
class LbThemedTextEntryDialog final : public wxDialog
{
public:
    LbThemedTextEntryDialog(wxWindow* parent,
                            const ThemeData& theme,
                            const wxString& title,
                            const wxString& prompt,
                            const wxString& actionLabel);

    wxString GetValue() const;

private:
    void UpdateOkButton();

    ThemeData   m_theme;
    wxTextCtrl* m_input = nullptr;
    wxButton*   m_okButton = nullptr;
};

// Small themed replacement for wxSingleChoiceDialog. Native
// wxSingleChoiceDialog also ignores the app palette on Windows, which makes
// list-picking dialogs look detached from the rest of LlamaBoss.
class LbThemedSingleChoiceDialog final : public wxDialog
{
public:
    using DeleteHandler = std::function<bool(int selection, const wxString& label)>;

    LbThemedSingleChoiceDialog(wxWindow* parent,
                               const ThemeData& theme,
                               const wxString& title,
                               const wxString& prompt,
                               const wxArrayString& choices,
                               const wxString& actionLabel);

    int GetSelection() const;
    void SetDeleteHandler(DeleteHandler handler);

private:
    void UpdateOkButton();
    void OnCharHook(wxKeyEvent& event);
    bool TryDeleteSelection();

    ThemeData     m_theme;
    wxListBox*    m_list = nullptr;
    wxButton*     m_okButton = nullptr;
    DeleteHandler m_deleteHandler;
};
