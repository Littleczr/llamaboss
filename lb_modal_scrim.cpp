// lb_modal_scrim.cpp
// Implementation of the Telegram-style modal scrim.  See lb_modal_scrim.h.
//
// The first version used a translucent wxFrame.  On wxMSW that can be
// capability-dependent and, in practice here, it did not reliably appear above
// the owning frame before the modal dialog opened.  We use a small native
// layered popup instead: it is owned by the LlamaBoss frame, paints solid
// black, applies per-window alpha, and is shown before the dialog enters its
// modal loop.  The dialog is created/shown afterward, so it remains above the
// scrim.
#include "lb_modal_scrim.h"

#include <wx/wx.h>
#include <wx/dialog.h>

#ifdef __WXMSW__
#include <windows.h>
#include <memory>
#endif

namespace {

#ifdef __WXMSW__

LRESULT CALLBACK ModalScrimWndProc(HWND hwnd,
                                   UINT msg,
                                   WPARAM wParam,
                                   LPARAM lParam)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // The scrim is decorative modal chrome, not an interactive window.
        // Do not let an outside click activate it or leak into focus changes.
        return MA_NOACTIVATEANDEAT;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_CONTEXTMENU:
        // Outside clicks should simply be swallowed while the dialog is modal.
        // This preserves the dimming effect without disturbing
        // wxDialog::ShowModal().
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

HWND ShowModalScrim(wxWindow& parent)
{
    const wxSize clientSize = parent.GetClientSize();
    if (clientSize.GetWidth() <= 0 || clientSize.GetHeight() <= 0)
        return nullptr;

    HWND owner = reinterpret_cast<HWND>(parent.GetHandle());
    if (!owner) return nullptr;

    static const wchar_t* kScrimClassName = L"LlamaBossModalScrim";
    static ATOM s_scrimClassAtom = 0;
    static bool s_scrimClassRegistered = false;
    static HBRUSH s_scrimBrush = nullptr;

    if (!s_scrimClassRegistered) {
        if (!s_scrimBrush)
            s_scrimBrush = CreateSolidBrush(RGB(0, 0, 0));

        WNDCLASSW wc{};
        wc.lpfnWndProc   = ModalScrimWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = kScrimClassName;
        wc.hbrBackground = s_scrimBrush;

        s_scrimClassAtom = RegisterClassW(&wc);
        if (s_scrimClassAtom == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
        s_scrimClassRegistered = true;
    }

    const wxPoint screenPos = parent.ClientToScreen(wxPoint(0, 0));

    HWND scrim = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kScrimClassName,
        L"",
        WS_POPUP,
        screenPos.x,
        screenPos.y,
        clientSize.GetWidth(),
        clientSize.GetHeight(),
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);

    if (!scrim) return nullptr;

    // 128/255 ≈ 50% black. On the Telegram-like #0E1621 app surface,
    // this visually lands close to the sampled #070B11 backdrop.
    SetLayeredWindowAttributes(scrim, 0, 128, LWA_ALPHA);

    ShowWindow(scrim, SW_SHOWNOACTIVATE);
    SetWindowPos(
        scrim,
        HWND_TOP,
        screenPos.x,
        screenPos.y,
        clientSize.GetWidth(),
        clientSize.GetHeight(),
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    return scrim;
}

struct ModalScrimGuard
{
    explicit ModalScrimGuard(HWND hwnd)
        : scrim(std::make_shared<HWND>(hwnd))
    {}

    ~ModalScrimGuard() { Reset(); }

    HWND Get() const { return scrim ? *scrim : nullptr; }

    void Reset()
    {
        if (!scrim) return;
        HWND hwnd = *scrim;
        if (hwnd && ::IsWindow(hwnd))
            ::DestroyWindow(hwnd);
        *scrim = nullptr;
    }

    std::shared_ptr<HWND> scrim;
};

#else  // !__WXMSW__

wxFrame* ShowModalScrim(wxWindow& /*parent*/)
{
    return nullptr;
}

void HideModalScrim(wxFrame*& scrim)
{
    if (scrim) scrim->Destroy();
    scrim = nullptr;
}

#endif // __WXMSW__

} // namespace

int LbShowModalWithScrim(wxWindow& parent, wxDialog& dlg)
{
#ifdef __WXMSW__
    ModalScrimGuard modalScrim(ShowModalScrim(parent));
    if (modalScrim.Get()) {
        std::shared_ptr<HWND> scrimState = modalScrim.scrim;
        dlg.Bind(wxEVT_SHOW, [scrimState, &dlg](wxShowEvent& event) {
            event.Skip();
            if (!event.IsShown()) return;

            HWND scrimHwnd = scrimState ? *scrimState : nullptr;
            if (!scrimHwnd || !::IsWindow(scrimHwnd)) return;

            HWND dialogHwnd = reinterpret_cast<HWND>(dlg.GetHandle());
            if (!dialogHwnd) return;

            // Some wxMSW custom dialogs create/focus their child controls
            // before ShowModal() enters the native modal loop.  Make the
            // intended z-order explicit once the dialog is visible: scrim
            // under the dialog, dialog on top and active.
            ::SetWindowPos(
                scrimHwnd,
                dialogHwnd,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ::SetWindowPos(
                dialogHwnd,
                HWND_TOP,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        });
    }
    const int dialogResult = dlg.ShowModal();
    modalScrim.Reset();
    return dialogResult;
#else
    wxFrame* modalScrim = ShowModalScrim(parent);
    const int dialogResult = dlg.ShowModal();
    HideModalScrim(modalScrim);
    return dialogResult;
#endif
}
