// chat_input_ctrl.h
// Custom input control with clipboard image and large-text attachment support.
// On Windows, a wxTE_MULTILINE text control handles Ctrl+V natively
// at the WM_PASTE message level, before wxEVT_CHAR_HOOK can fire.
// This subclass intercepts WM_PASTE to check for clipboard images first.
#pragma once

#include <wx/wx.h>
#include <functional>

class ChatInputCtrl : public wxTextCtrl {
public:
    ChatInputCtrl(wxWindow* parent, wxWindowID id,
        const wxString& value = wxEmptyString,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = 0)
        : wxTextCtrl(parent, id, value, pos, size, style)
#ifdef __WXMSW__
    {}
#else
    {
        // macOS: there is no WM_PASTE, and wxOSX sends no wxEVT_TEXT_PASTE
        // for Cmd+V (the NSTextView pastes natively), so the shortcut is
        // caught as a key.  Same order as the Windows hook: image first,
        // then large-text attachment, else the native paste.  Cmd+Shift+V
        // and the Edit menu item keep the plain native paste.
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& evt) {
            if (evt.GetKeyCode() == 'V' && evt.GetModifiers() == wxMOD_CMD &&
                !m_pasteAsText && IsEnabled() && IsEditable()) {
                if (m_imagePasteHandler && m_imagePasteHandler()) return;
                if (m_textPasteHandler && m_textPasteHandler()) return;
            }
            evt.Skip();
        });
    }
#endif

    void SetImagePasteHandler(std::function<bool()> handler) {
        m_imagePasteHandler = handler;
    }

    void SetTextPasteHandler(std::function<bool()> handler) {
        m_textPasteHandler = handler;
    }

    // Explicit escape hatch for a long prompt that should stay in the editor.
    // The frame binds Ctrl+Shift+V to this; native paste keeps caret/undo rules.
    void PasteAsText() {
        const bool previous = m_pasteAsText;
        m_pasteAsText = true;
        wxTextCtrl::Paste();
        m_pasteAsText = previous;
    }

#ifdef __WXMSW__
protected:
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override {
        // Intercept WM_PASTE (0x0302) before the native edit control handles it.
        // Handle attachments before the native control inserts a large block.
        // A true result also covers a reported failure: keep the clipboard
        // and draft intact instead of silently falling back to a huge paste.
        if (nMsg == 0x0302 /* WM_PASTE */ && !m_pasteAsText &&
            IsEnabled() && IsEditable()) {
            if (m_imagePasteHandler && m_imagePasteHandler())
                return 0;
            if (m_textPasteHandler && m_textPasteHandler())
                return 0;
        }
        return wxTextCtrl::MSWWindowProc(nMsg, wParam, lParam);
    }
#endif

private:
    std::function<bool()> m_imagePasteHandler;
    std::function<bool()> m_textPasteHandler;
    bool m_pasteAsText = false;
};
