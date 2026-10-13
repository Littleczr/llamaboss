// lb_mac_native.mm — see lb_mac_native.h.
#include "lb_mac_native.h"
#include "lb_mac_confirm.h"

#import <Cocoa/Cocoa.h>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/eventfilter.h>
#include <wx/nonownedwnd.h>
#include <wx/time.h>
#include <wx/toplevel.h>

#include <cmath>
#include <unordered_map>

#include "app.h"
#include "app_state.h"
#include "theme.h"

namespace {

enum class Role { Leave, Plain, Primary, Destructive };

int Distance(const wxColour& a, const wxColour& b)
{
    if (!a.IsOk() || !b.IsOk()) return 1 << 20;
    const int dr = int(a.Red()) - int(b.Red());
    const int dg = int(a.Green()) - int(b.Green());
    const int db = int(a.Blue()) - int(b.Blue());
    return int(std::sqrt(double(dr * dr + dg * dg + db * db)));
}

bool IsAffirmative(wxButton* btn)
{
    switch (btn->GetId()) {
    case wxID_OK: case wxID_YES: case wxID_SAVE: case wxID_APPLY: return true;
    default: break;
    }
    if (auto* tlw = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(btn)))
        return tlw->GetDefaultItem() == btn;
    return false;
}

Role RoleFor(wxButton* btn, const ThemeData& theme)
{
    const wxString label = btn->GetLabel();
    if (label.empty() || btn->GetBitmap().IsOk()) return Role::Leave;
    if (!btn->UseBackgroundColour()) return Role::Plain;

    const wxColour bg = btn->GetBackgroundColour();
    // A fill that matches the parent with a one- or two-character label is
    // a flat glyph button (close, overflow) by design.
    if (wxWindow* parent = btn->GetParent())
        if (label.length() <= 2 && Distance(bg, parent->GetBackgroundColour()) < 12)
            return Role::Leave;

    const wxColour neutrals[] = {
        theme.bgMain, theme.bgToolbar, theme.bgSidebar, theme.bgInputField,
        theme.bgInputArea, theme.bgDialogSurface,
        theme.sidebarHover, theme.sidebarSelected,
        // not modelPillBg: several themes make it a deep accent blue
    };
    int dNeutral = 1 << 20;
    for (const wxColour& c : neutrals) dNeutral = std::min(dNeutral, Distance(bg, c));
    const int dAccent = Distance(bg, theme.accentButton);
    const int dStop = Distance(bg, theme.stopButton);

    if (dStop < dAccent && dStop < dNeutral) return Role::Destructive;
    if (dAccent < dNeutral || dNeutral > 60) {
        // macOS shows one accent button per window: the dialog's default /
        // affirmative action.  Other accent-filled buttons in dialogs are
        // ordinary push buttons; the main window keeps Send and New Chat.
        const bool inDialog = dynamic_cast<wxDialog*>(wxGetTopLevelParent(btn)) != nullptr;
        return (!inDialog || IsAffirmative(btn)) ? Role::Primary : Role::Plain;
    }
    return Role::Plain;
}

// Push bezels need more room than the flat Windows buttons the shared
// layout was sized for; grow the minimum width so labels are not cut off.
void FitWidth(wxButton* btn, NSButton* b)
{
    const int need = int(std::ceil([[b cell] cellSize].width)) + 6;
    const wxSize cur = btn->GetSize();
    if (cur.x >= need) return;
    const wxSize min = btn->GetMinSize();
    btn->SetMinSize(wxSize(need, min.y > 0 ? min.y : cur.y));
    if (wxWindow* parent = btn->GetParent()) parent->Layout();
}

bool TitleHasColour(NSButton* b)
{
    NSAttributedString* t = [b attributedTitle];
    if ([t length] == 0) return false;
    return [t attribute:NSForegroundColorAttributeName atIndex:0 effectiveRange:nullptr] != nil;
}

void SetTitleColour(NSButton* b, NSColor* colour)
{
    NSMutableAttributedString* s =
        [[NSMutableAttributedString alloc] initWithString:[b title]];
    const NSRange all = NSMakeRange(0, [s length]);
    NSMutableParagraphStyle* para = [[NSMutableParagraphStyle alloc] init];
    [para setAlignment:NSTextAlignmentCenter];
    [s addAttribute:NSParagraphStyleAttributeName value:para range:all];
    if (NSFont* f = [b font]) [s addAttribute:NSFontAttributeName value:f range:all];
    [s addAttribute:NSForegroundColorAttributeName value:colour range:all];
    [b setAttributedTitle:s];
    [para release];
    [s release];
}

// The shared code sets the role colours once (and again on a theme change);
// they are read here, remembered, then cleared so wx does not paint the flat
// Windows fill over the native bezel.
std::unordered_map<wxButton*, Role>& Roles()
{
    static std::unordered_map<wxButton*, Role> roles;
    return roles;
}

Role TakeRole(wxButton* btn, const ThemeData& theme)
{
    auto& roles = Roles();
    if (!btn->UseBackgroundColour() && !btn->UseForegroundColour()) {
        auto it = roles.find(btn);
        if (it != roles.end()) return it->second;
    }
    const Role role = RoleFor(btn, theme);
    if (role == Role::Leave) return role;
    if (roles.find(btn) == roles.end())
        btn->Bind(wxEVT_DESTROY, [btn](wxWindowDestroyEvent& e) {
            if (e.GetEventObject() == btn) Roles().erase(btn);
            e.Skip();
        });
    roles[btn] = role;
    btn->SetBackgroundColour(wxNullColour);
    btn->SetForegroundColour(wxNullColour);
    btn->Refresh();
    return role;
}

// wx sets a borderless button's title colour when SetForegroundColour is
// called, but after the app appearance changes (a theme switch between dark
// and light) it no longer reaches the NSButton: the toolbar glyphs (☰ + ⓘ)
// kept the previous theme's colour, invisible on the new background, and
// their hover tint stopped showing.  Copy the colour wx holds onto the title.
void SyncFlatTitleColour(wxButton* btn)
{
    if (!btn->UseForegroundColour() || btn->GetLabel().empty() || btn->GetBitmap().IsOk())
        return;
    NSButton* b = (NSButton*)btn->GetHandle();
    if (![b isKindOfClass:[NSButton class]] || [b isBordered] || [[b title] length] == 0)
        return;
    NSColor* want = [btn->GetForegroundColour().OSXGetNSColor()
                        colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    NSAttributedString* t = [b attributedTitle];
    NSColor* have = [t length] ? [t attribute:NSForegroundColorAttributeName atIndex:0
                                    effectiveRange:nullptr] : nil;
    have = [have colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (have && want &&
        std::fabs([have redComponent] - [want redComponent]) < 0.004 &&
        std::fabs([have greenComponent] - [want greenComponent]) < 0.004 &&
        std::fabs([have blueComponent] - [want blueComponent]) < 0.004 &&
        std::fabs([have alphaComponent] - [want alphaComponent]) < 0.004)
        return;
    if (want) SetTitleColour(b, want);
}

void StyleButton(wxButton* btn, const ThemeData& theme)
{
    const Role role = TakeRole(btn, theme);
    if (role == Role::Leave) {
        SyncFlatTitleColour(btn);
        return;
    }
    NSButton* b = (NSButton*)btn->GetHandle();
    if (![b isKindOfClass:[NSButton class]]) return;

    if (![b isBordered]) [b setBordered:YES];
    if ([b bezelStyle] != NSBezelStylePush) [b setBezelStyle:NSBezelStylePush];

    NSColor* bezel = nil;
    if (role == Role::Primary) bezel = [NSColor controlAccentColor];
    else if (role == Role::Destructive) bezel = [NSColor systemRedColor];
    NSColor* current = [b bezelColor];
    if (bezel != current && ![bezel isEqual:current]) [b setBezelColor:bezel];

    // Tinted buttons use white text, as AppKit's default button does; plain
    // ones use the system label colour (no attributed colour).
    if (bezel) {
        NSColor* have = [[b attributedTitle] length]
            ? [[b attributedTitle] attribute:NSForegroundColorAttributeName
                                     atIndex:0 effectiveRange:nullptr]
            : nil;
        if (![have isEqual:[NSColor whiteColor]]) SetTitleColour(b, [NSColor whiteColor]);
    } else if (TitleHasColour(b)) {
        [b setTitle:[b title]];
    }
    FitWidth(btn, b);
}

void StyleTree(wxWindow* w, const ThemeData& theme)
{
    if (auto* btn = dynamic_cast<wxButton*>(w)) StyleButton(btn, theme);
    for (wxWindow* child : w->GetChildren()) StyleTree(child, theme);
}

// Changing NSApp.appearance makes wx send wxEVT_SYS_COLOUR_CHANGED, and
// some controls (the chat wxRichTextCtrl) then drop their theme colours for
// the system ones.  So the appearance is set once at startup, before any
// window exists, and later only when the theme moves between dark and light.
void MatchAppearance(const ThemeData& theme)
{
    const wxColour& bg = theme.bgMain;
    const bool dark = bg.IsOk() &&
        (0.299 * bg.Red() + 0.587 * bg.Green() + 0.114 * bg.Blue()) < 128.0;
    NSAppearanceName want = dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    NSAppearanceName have = [[NSApp effectiveAppearance]
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    if ([want isEqualToString:have]) return;
    [NSApp setAppearance:[NSAppearance appearanceNamed:want]];
}

void Apply(wxWindow* top)
{
    const ThemeData& theme = wxGetApp().GetAppState().GetTheme();
    MatchAppearance(theme);
    StyleTree(top, theme);
}

class NativeControlsFilter : public wxEventFilter
{
public:
    int FilterEvent(wxEvent& event) override
    {
        const wxEventType t = event.GetEventType();
        if (t == wxEVT_ENTER_WINDOW || t == wxEVT_LEAVE_WINDOW) {
            // The shared hover code recolours glyph buttons in its own
            // handler for this event; sync the title once it has run.
            if (auto* btn = wxDynamicCast(event.GetEventObject(), wxButton)) {
                wxWeakRef<wxButton> ref(btn);
                wxTheApp->CallAfter([ref] { if (ref) SyncFlatTitleColour(ref.get()); });
            }
            return Event_Skip;
        }
        if (t != wxEVT_SHOW && t != wxEVT_IDLE) return Event_Skip;
        auto* win = wxDynamicCast(event.GetEventObject(), wxWindow);
        if (!win || !win->IsTopLevel() || win->IsBeingDeleted()) return Event_Skip;

        // Idle passes are throttled; shows apply at once so a new dialog
        // never appears with the Windows styling.
        const wxLongLong now = wxGetLocalTimeMillis();
        wxLongLong& last = m_lastPass[win];
        if (t == wxEVT_IDLE && now - last < 250) return Event_Skip;
        last = now;
        Apply(win);
        return Event_Skip;
    }

private:
    std::unordered_map<wxWindow*, wxLongLong> m_lastPass;
};

} // namespace

void LbMacInstallNativeControls()
{
    // Lives for the whole process: a filter must not be destroyed while
    // still registered, and the app only removes its own at exit.
    static NativeControlsFilter* filter = nullptr;
    if (filter) return;
    filter = new NativeControlsFilter;
    wxEvtHandler::AddFilter(filter);
    MatchAppearance(wxGetApp().GetAppState().GetTheme());
}

// ── Confirmation alerts (lb_mac_confirm.h) ──────────────────────────
int LbMacConfirm(const wxString& message, const wxString& caption,
                 const wxString& action, long style, wxWindow* /*parent*/,
                 const wxString& cancel)
{
    auto ns = [](const wxString& s) { return [NSString stringWithUTF8String:s.utf8_str().data()]; };
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:ns(caption)];
    [alert setInformativeText:ns(message)];
    // Critical shows the caution triangle with the app icon badge, as the
    // wx alert did; informational shows the app icon.
    [alert setAlertStyle:(style & wxICON_WARNING) ? NSAlertStyleCritical
                                                  : NSAlertStyleInformational];
    const bool cancelDefault = (style & wxNO_DEFAULT) != 0;
    // The first button is the default (rightmost, Return); "Cancel" also
    // answers Esc.
    NSButton* actionButton = nil;
    if (cancelDefault) {
        // AppKit gives a "Cancel" button Esc instead of Return; make it
        // the default explicitly (Esc still cancels the alert).
        NSButton* cancelButton = [alert addButtonWithTitle:ns(cancel)];
        [cancelButton setKeyEquivalent:@"\r"];
        actionButton = [alert addButtonWithTitle:ns(action)];
        if (@available(macOS 11.0, *)) [actionButton setHasDestructiveAction:YES];
    } else {
        actionButton = [alert addButtonWithTitle:ns(action)];
        [alert addButtonWithTitle:ns(cancel)];
    }
    // Esc cancels too: the cancel button's own Esc equivalent was
    // replaced by Return above.
    id escMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                          handler:^NSEvent*(NSEvent* e) {
        if ([e keyCode] == 53 && [NSApp modalWindow] == [alert window]) {
            [NSApp stopModalWithCode:cancelDefault ? NSAlertFirstButtonReturn
                                                   : NSAlertSecondButtonReturn];
            return nil;
        }
        return e;
    }];
    const NSModalResponse r = [alert runModal];
    [NSEvent removeMonitor:escMonitor];
    [alert release];
    const NSModalResponse actionResponse =
        cancelDefault ? NSAlertSecondButtonReturn : NSAlertFirstButtonReturn;
    return r == actionResponse ? wxYES : wxNO;
}
