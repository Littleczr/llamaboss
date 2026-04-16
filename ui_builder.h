// ui_builder.h
// Pure UI construction helpers extracted from MyFrame.
// These functions create and lay out widgets, returning
// structs of pointers that the caller stores in its members.
#pragma once

#include <wx/wx.h>

class StatusDot;
class ChatInputCtrl;
struct ThemeData;

namespace UIBuilder {

// ─── Top bar result ──────────────────────────────────────────────
struct TopBarWidgets {
    wxPanel*       toolbarPanel;
    wxStaticText*  titleLabel;
    wxPanel*       modelPill;
    wxStaticText*  modelLabel;
    StatusDot*     statusDot;
    wxButton*      sidebarToggle;
    wxButton*      newChatButton;
    wxButton*      settingsButton;
    wxButton*      aboutButton;
    wxPanel*       topSeparator;
};

// Creates the toolbar panel with sidebar toggle, title, model pill,
// new-chat / settings / about buttons, and a bottom separator.
// Appends everything to |mainSizer|.
TopBarWidgets BuildTopBar(wxWindow* parent, wxBoxSizer* mainSizer,
                          const ThemeData& theme);

// ─── Input area result ───────────────────────────────────────────
struct InputAreaWidgets {
    wxPanel*       inputContainer;
    wxPanel*       inputSeparator;
    ChatInputCtrl* userInputCtrl;
    wxButton*      sendButton;
    wxButton*      stopButton;
    wxButton*      attachButton;
    wxBoxSizer*    inputSizer;
};

// Creates the input row: [📎] [TextInput] [Send/Stop].
// Appends everything to |parentSizer| (the right-panel sizer).
InputAreaWidgets BuildInputArea(wxWindow* parent, wxBoxSizer* parentSizer,
                                const ThemeData& theme);

} // namespace UIBuilder
