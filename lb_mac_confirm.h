// lb_mac_confirm.h
//
// macOS only.  Mac alerts name the action on their buttons ("Delete" /
// "Cancel"), not "Yes" / "No" (Human Interface Guidelines).  A drop-in for
// the shared code's wxMessageBox(..., wxYES_NO ...) confirmations: same
// style flags, same return values (wxYES or wxNO).  With wxNO_DEFAULT the
// cancel button is the default (rightmost, Return) and the action is shown
// as destructive.  Implemented with NSAlert in lb_mac_native.mm: wx's
// SetYesNoLabels drops wxNO_DEFAULT, leaving no default button at all.
#pragma once

#ifdef __WXMAC__

#include <wx/string.h>

class wxWindow;

int LbMacConfirm(const wxString& message, const wxString& caption,
                 const wxString& action, long style, wxWindow* parent,
                 const wxString& cancel = "Cancel");

#endif // __WXMAC__
