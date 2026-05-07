// chat_input_ctrl.h
// Custom input control with clipboard image paste support.
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
    {}

    void SetImagePasteHandler(std::function<bool()> handler) {
        m_imagePasteHandler = handler;
    }

#ifdef __WXMSW__
protected:
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override {
        // Intercept WM_PASTE (0x0302) before the native edit control handles it.
        // If the clipboard contains an image, handle it and suppress text paste.
        if (nMsg == 0x0302 /* WM_PASTE */) {
            if (m_imagePasteHandler && m_imagePasteHandler())
                return 0;
        }
        return wxTextCtrl::MSWWindowProc(nMsg, wParam, lParam);
    }
#endif

private:
    std::function<bool()> m_imagePasteHandler;
};
