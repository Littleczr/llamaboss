// lb_mac_native.h
//
// macOS only: native look for standard controls.
//
// The shared UI code paints wxButton with theme colours (SetBackgroundColour /
// SetForegroundColour).  On Windows that produces the flat themed buttons; on
// macOS NSButton ignores the background and only half-applies the text
// colour, so the buttons come out washed out.  Following the Mac identity
// principle (MAC_PORT_STATUS.md), the Mac build does not imitate the Windows
// styling: every text wxButton becomes a native push button, and its theme
// role is kept by mapping the colour the shared code chose:
//
//   theme accent fill      -> push button tinted with the system accent colour,
//                             but in dialogs only the default/OK button (one
//                             accent button per window, as on macOS)
//   theme stop (red) fill  -> push button tinted system red
//   anything else          -> plain push button
//   1-2 char flat glyphs   -> left alone (close / overflow buttons)
//
// Push bezels need more padding than the flat Windows buttons, so a button
// narrower than its label is widened.
//
// The app appearance (dark/light Aqua) follows the LlamaBoss theme so native
// controls match the window colours whatever the system setting is.
//
// LbMacInstallNativeControls() installs an app-wide event filter that applies
// this when a window is shown and again shortly after the UI goes idle, so
// theme switches and relabelled buttons are covered without touching the
// shared code.
#pragma once

#ifdef __WXMAC__
void LbMacInstallNativeControls();
#endif
