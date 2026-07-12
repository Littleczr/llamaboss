// lb_modal_scrim.h
// Telegram-style modal scrim: raised dialogs stay visually focused while the
// parent client area dims beneath them.  Extracted from MyFrame to keep the
// frame thin; the Win32 layered-popup machinery lives in lb_modal_scrim.cpp.
//
// Public surface is a single helper that shows a dialog modally with the scrim
// applied underneath it.  On non-Windows builds the scrim is a no-op and this
// simply forwards to wxDialog::ShowModal().
#pragma once

class wxWindow;
class wxDialog;

// Shows `dlg` modally with a dimming scrim drawn over `parent`'s client area
// (Windows only; elsewhere this is a plain ShowModal()).  Returns the dialog's
// ShowModal() result (e.g. wxID_OK / wxID_CANCEL).
int LbShowModalWithScrim(wxWindow& parent, wxDialog& dlg);
