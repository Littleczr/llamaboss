// ui_builder.cpp
#include "ui_builder.h"
#include "widgets.h"
#include "chat_input_ctrl.h"
#include "theme.h"

namespace UIBuilder {

TopBarWidgets BuildTopBar(wxWindow* parent, wxBoxSizer* mainSizer,
                          const ThemeData& theme)
{
    TopBarWidgets w{};

    w.toolbarPanel = new wxPanel(parent, wxID_ANY);
    w.toolbarPanel->SetBackgroundColour(theme.bgToolbar);
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    // ── Left: Sidebar toggle + App title ──
    wxString hamburger = wxString::FromUTF8("\xE2\x98\xB0"); // ☰
    w.sidebarToggle = new wxButton(w.toolbarPanel, wxID_ANY, hamburger,
        wxDefaultPosition, wxSize(52, 44), wxBORDER_NONE);
    w.sidebarToggle->SetBackgroundColour(theme.bgToolbar);
    w.sidebarToggle->SetForegroundColour(theme.textMuted);
    w.sidebarToggle->SetToolTip("Toggle sidebar");
    wxFont hamburgerFont = w.sidebarToggle->GetFont();
    hamburgerFont.SetPointSize(18);
    w.sidebarToggle->SetFont(hamburgerFont);
    w.sidebarToggle->SetCursor(wxCURSOR_HAND);
    sizer->Add(w.sidebarToggle, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

    w.titleLabel = new wxStaticText(w.toolbarPanel, wxID_ANY, "LlamaBoss");
    w.titleLabel->SetForegroundColour(theme.textPrimary);
    wxFont titleFont = w.titleLabel->GetFont();
    titleFont.SetPointSize(15);
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    w.titleLabel->SetFont(titleFont);
    sizer->Add(w.titleLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

    sizer->AddStretchSpacer(1);

    // ── Center: Model pill [dot + model name] (clickable) ──
    w.modelPill = new wxPanel(w.toolbarPanel, wxID_ANY);
    w.modelPill->SetBackgroundColour(theme.bgToolbar);
    w.modelPill->SetCursor(wxCURSOR_HAND);
    auto* pillSizer = new wxBoxSizer(wxHORIZONTAL);

    w.statusDot = new StatusDot(w.modelPill, 10);
    w.statusDot->SetCursor(wxCURSOR_HAND);
    pillSizer->Add(w.statusDot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

    w.modelLabel = new wxStaticText(w.modelPill, wxID_ANY, "loading...");
    w.modelLabel->SetForegroundColour(theme.textPrimary);
    w.modelLabel->SetCursor(wxCURSOR_HAND);
    wxFont modelFont = w.modelLabel->GetFont();
    modelFont.SetPointSize(13);
    modelFont.SetWeight(wxFONTWEIGHT_BOLD);
    w.modelLabel->SetFont(modelFont);
    pillSizer->Add(w.modelLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    pillSizer->AddSpacer(10);

    w.modelPill->SetSizer(pillSizer);
    sizer->Add(w.modelPill, 0, wxALIGN_CENTER_VERTICAL);

    sizer->AddStretchSpacer(1);

    // ── Right: New Chat button ──
    w.newChatButton = new wxButton(w.toolbarPanel, wxID_ANY, "+",
        wxDefaultPosition, wxSize(48, 44), wxBORDER_NONE);
    w.newChatButton->SetBackgroundColour(theme.bgToolbar);
    w.newChatButton->SetForegroundColour(theme.textMuted);
    w.newChatButton->SetToolTip("New Chat (Ctrl+N)");
    wxFont newChatFont = w.newChatButton->GetFont();
    newChatFont.SetPointSize(22);
    w.newChatButton->SetFont(newChatFont);
    w.newChatButton->SetCursor(wxCURSOR_HAND);
    sizer->Add(w.newChatButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

    // ── Right: Settings gear ──
    wxString gear = wxString::FromUTF8("\xE2\x9A\x99\xEF\xB8\x8F");
    w.settingsButton = new wxButton(w.toolbarPanel, wxID_ANY, gear,
        wxDefaultPosition, wxSize(52, 44), wxBORDER_NONE);
    w.settingsButton->SetBackgroundColour(theme.bgToolbar);
    w.settingsButton->SetForegroundColour(theme.textMuted);
    w.settingsButton->SetToolTip("Settings");
    wxFont gearFont = w.settingsButton->GetFont();
    gearFont.SetPointSize(18);
    w.settingsButton->SetFont(gearFont);
    w.settingsButton->SetCursor(wxCURSOR_HAND);
    sizer->Add(w.settingsButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);

    // ── Right: About info ──
    wxString infoChar = wxString::FromUTF8("\xE2\x93\x98"); // ⓘ
    w.aboutButton = new wxButton(w.toolbarPanel, wxID_ANY, infoChar,
        wxDefaultPosition, wxSize(48, 44), wxBORDER_NONE);
    w.aboutButton->SetBackgroundColour(theme.bgToolbar);
    w.aboutButton->SetForegroundColour(theme.textMuted);
    w.aboutButton->SetToolTip("About LlamaBoss");
    wxFont aboutFont = w.aboutButton->GetFont();
    aboutFont.SetPointSize(18);
    w.aboutButton->SetFont(aboutFont);
    w.aboutButton->SetCursor(wxCURSOR_HAND);
    sizer->Add(w.aboutButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    w.toolbarPanel->SetSizer(sizer);
    mainSizer->Add(w.toolbarPanel, 0, wxEXPAND);

    // Separator line
    w.topSeparator = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    w.topSeparator->SetBackgroundColour(theme.borderSubtle);
    mainSizer->Add(w.topSeparator, 0, wxEXPAND);

    return w;
}

InputAreaWidgets BuildInputArea(wxWindow* parent, wxBoxSizer* parentSizer,
                                const ThemeData& theme)
{
    InputAreaWidgets w{};

    w.inputContainer = new wxPanel(parent, wxID_ANY);
    w.inputContainer->SetBackgroundColour(theme.bgInputArea);
    auto* outerSizer = new wxBoxSizer(wxVERTICAL);

    // Separator line above input
    w.inputSeparator = new wxPanel(w.inputContainer, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 1));
    w.inputSeparator->SetBackgroundColour(theme.borderSubtle);
    outerSizer->Add(w.inputSeparator, 0, wxEXPAND);

    // Input row: [📎] [TextInput] [Send/Stop]
    w.inputSizer = new wxBoxSizer(wxHORIZONTAL);

    // Attach button
    wxString clip = wxString::FromUTF8("\xF0\x9F\x93\x8E");
    w.attachButton = new wxButton(w.inputContainer, wxID_ANY, clip,
        wxDefaultPosition, wxSize(52, 42), wxBORDER_NONE);
    w.attachButton->SetBackgroundColour(theme.bgInputArea);
    w.attachButton->SetForegroundColour(theme.textMuted);
    w.attachButton->SetToolTip("Attach files");
    wxFont clipFont = w.attachButton->GetFont();
    clipFont.SetPointSize(18);
    w.attachButton->SetFont(clipFont);
    w.attachButton->SetCursor(wxCURSOR_HAND);
    w.inputSizer->Add(w.attachButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

    // Text input field (ChatInputCtrl intercepts WM_PASTE for image clipboard)
    w.userInputCtrl = new ChatInputCtrl(w.inputContainer, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize,
        wxTE_PROCESS_ENTER | wxTE_MULTILINE | wxBORDER_NONE);
    w.userInputCtrl->SetBackgroundColour(theme.bgInputField);
    w.userInputCtrl->SetForegroundColour(theme.textPrimary);
    w.userInputCtrl->SetHint("Message...");
    w.inputSizer->Add(w.userInputCtrl, 1, wxEXPAND | wxTOP | wxBOTTOM, 6);

    // Send button — the primary action
    w.sendButton = new wxButton(w.inputContainer, wxID_ANY, "Send",
        wxDefaultPosition, wxSize(76, 36), wxBORDER_NONE);
    w.sendButton->SetBackgroundColour(theme.accentButton);
    w.sendButton->SetForegroundColour(theme.accentButtonText);
    wxFont btnFont = w.sendButton->GetFont();
    btnFont.SetPointSize(11);
    btnFont.SetWeight(wxFONTWEIGHT_MEDIUM);
    w.sendButton->SetFont(btnFont);
    w.inputSizer->Add(w.sendButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);

    // Stop button — red, hidden by default, replaces Send
    w.stopButton = new wxButton(w.inputContainer, wxID_ANY, "Stop",
        wxDefaultPosition, wxSize(76, 36), wxBORDER_NONE);
    w.stopButton->SetBackgroundColour(theme.stopButton);
    w.stopButton->SetForegroundColour(theme.stopButtonText);
    w.stopButton->SetFont(btnFont);
    w.stopButton->Hide();
    w.inputSizer->Add(w.stopButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    outerSizer->Add(w.inputSizer, 0, wxEXPAND | wxALL, 4);
    w.inputContainer->SetSizer(outerSizer);
    parentSizer->Add(w.inputContainer, 0, wxEXPAND);

    return w;
}

} // namespace UIBuilder
