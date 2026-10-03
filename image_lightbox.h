// image_lightbox.h
// Shared full-size image viewer ("lightbox") used by:
//   • ChatDisplay       — clicking a thumbnail in the transcript
//   • AttachmentChip    — clicking a pending (not yet sent) image card
//
// Lifted out of ChatDisplay::ShowImageViewer so both surfaces get the
// same viewer.  Header-only on purpose: no .vcxproj change needed.
//
// Behavior:
//   • Frame dims under the modal scrim; the image sits centred on the
//     chat background with a thin padding ring.
//   • Dismiss: Esc / Enter / Space, the ✕ badge, clicking the image,
//     or clicking anywhere on the dimmed backdrop (like Claude).
//   • Optional caption line under the image: "name · W × H".
//   • Optional right-click actions (the transcript passes Save as /
//     Show in folder; pending attachments have no file yet, so none).

#pragma once

#include <wx/wx.h>
#include <wx/dialog.h>
#include <wx/statbmp.h>
#include <wx/menu.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "lb_modal_scrim.h"

struct LbLightboxAction {
    wxString              label;
    std::function<void()> run;
};

// `owner` is the top-level frame: it parents the dialog and is the area
// the scrim dims.  `full` is taken by value because it is rescaled here.
inline void LbShowImageLightbox(wxWindow& owner,
                                wxImage full,
                                const wxColour& bg,
                                const wxColour& fg,
                                const wxString& caption = wxString(),
                                std::vector<LbLightboxAction> actions = {})
{
    if (!full.IsOk()) { wxBell(); return; }

    const int nativeW = full.GetWidth();
    const int nativeH = full.GetHeight();

    // Fit within ~88% of the frame client area, minus a reservation for
    // the close row and caption.  Downscale only — upscaling past native
    // resolution trades sharpness for size.
    const bool hasCaption = !caption.empty();
    const wxSize avail = owner.GetClientSize();
    const int reserveH = owner.FromDIP(hasCaption ? 76 : 48);
    const int maxW = std::max(320, (int)(avail.GetWidth()  * 0.88));
    const int maxH = std::max(240, (int)(avail.GetHeight() * 0.88) - reserveH);

    int w = nativeW, h = nativeH;
    if (w > maxW || h > maxH) {
        const double scale = std::min((double)maxW / (double)w,
                                      (double)maxH / (double)h);
        w = std::max(1, (int)(w * scale));
        h = std::max(1, (int)(h * scale));
        full.Rescale(w, h, wxIMAGE_QUALITY_HIGH);
    }

    wxDialog dlg(&owner, wxID_ANY, wxEmptyString,
                 wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(bg);

    // Muted at rest, full foreground on hover — derived from the live
    // palette so it works on every theme.
    const wxColour mutedFg((fg.Red()   + bg.Red())   / 2,
                           (fg.Green() + bg.Green()) / 2,
                           (fg.Blue()  + bg.Blue())  / 2);

    auto* closeX = new wxStaticText(&dlg, wxID_ANY, wxString(L"\u2715"));
    closeX->SetForegroundColour(mutedFg);
    closeX->SetBackgroundColour(bg);
    {
        wxFont f = closeX->GetFont();
        f.SetPointSize(f.GetPointSize() + 3);
        closeX->SetFont(f);
    }
    closeX->SetCursor(wxCursor(wxCURSOR_HAND));
    closeX->SetToolTip("Close (Esc)");
    closeX->Bind(wxEVT_ENTER_WINDOW, [closeX, fg](wxMouseEvent& e) {
        closeX->SetForegroundColour(fg);
        closeX->Refresh();
        e.Skip();
    });
    closeX->Bind(wxEVT_LEAVE_WINDOW, [closeX, mutedFg](wxMouseEvent& e) {
        closeX->SetForegroundColour(mutedFg);
        closeX->Refresh();
        e.Skip();
    });

    auto* bitmap = new wxStaticBitmap(&dlg, wxID_ANY, wxBitmap(full));

    auto* topRow = new wxBoxSizer(wxHORIZONTAL);
    topRow->AddStretchSpacer(1);
    topRow->Add(closeX, 0, wxTOP | wxRIGHT, owner.FromDIP(10));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(topRow, 0, wxEXPAND);

    wxStaticText* captionText = nullptr;
    if (hasCaption) {
        sizer->Add(bitmap, 0, wxLEFT | wxRIGHT, owner.FromDIP(12));

        wxString line = caption;
        line += wxString::FromUTF8("  \xC2\xB7  ");
        line += wxString::Format("%d \u00D7 %d", nativeW, nativeH);

        captionText = new wxStaticText(&dlg, wxID_ANY, line,
                                       wxDefaultPosition, wxDefaultSize,
                                       wxALIGN_CENTRE_HORIZONTAL |
                                       wxST_ELLIPSIZE_MIDDLE);
        captionText->SetForegroundColour(mutedFg);
        captionText->SetBackgroundColour(bg);
        {
            wxFont f = captionText->GetFont();
            f.SetPointSize(std::max(8, f.GetPointSize() - 1));
            captionText->SetFont(f);
        }
        // Keep a long filename from widening the dialog past the image.
        captionText->SetMaxSize(wxSize(w, -1));
        sizer->Add(captionText, 0,
                   wxALIGN_CENTRE_HORIZONTAL | wxALL, owner.FromDIP(10));
    } else {
        sizer->Add(bitmap, 0, wxLEFT | wxRIGHT | wxBOTTOM, owner.FromDIP(12));
    }

    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();

    const auto dismiss = [&dlg](wxMouseEvent&) {
        if (dlg.IsModal()) dlg.EndModal(wxID_OK);
    };
    closeX->Bind(wxEVT_LEFT_UP, dismiss);
    bitmap->Bind(wxEVT_LEFT_UP, dismiss);
    dlg.Bind(wxEVT_LEFT_UP, dismiss);
    if (captionText) captionText->Bind(wxEVT_LEFT_UP, dismiss);

    // A click on the dimmed backdrop reaches us as WM_CLOSE (posted by
    // the scrim — see LbShowModalWithScrim's dismissOnScrimClick).
    dlg.Bind(wxEVT_CLOSE_WINDOW, [&dlg](wxCloseEvent&) {
        if (dlg.IsModal()) dlg.EndModal(wxID_CANCEL);
    });

    dlg.Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& e) {
        const int key = e.GetKeyCode();
        if (key == WXK_ESCAPE || key == WXK_RETURN || key == WXK_SPACE ||
            key == WXK_NUMPAD_ENTER) {
            dlg.EndModal(wxID_CANCEL);
        } else {
            e.Skip();
        }
    });

    // Right-click actions.  Bound on both the bitmap and the dialog;
    // wxContextMenuEvent propagates and the handled event is not
    // Skip()ed, so it fires once.  Actions are copied into the lambda so
    // nothing depends on the caller's storage across the nested loop.
    if (!actions.empty()) {
        const auto contextMenu = [&dlg, actions](wxContextMenuEvent&) {
            wxMenu menu;
            for (size_t i = 0; i < actions.size(); ++i)
                menu.Append((int)i + 1, actions[i].label);
            const int sel = dlg.GetPopupMenuSelectionFromUser(menu);
            if (sel >= 1 && sel <= (int)actions.size() && actions[sel - 1].run)
                actions[sel - 1].run();
        };
        bitmap->Bind(wxEVT_CONTEXT_MENU, contextMenu);
        dlg.Bind(wxEVT_CONTEXT_MENU, contextMenu);
    }

    LbShowModalWithScrim(owner, dlg, /*dismissOnScrimClick*/ true);
}
