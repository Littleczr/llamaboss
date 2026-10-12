// lb_ui_font.h
//
// Fixed UI font sizes are written for Windows, where wx maps one point to
// 96/72 pixels at 100% scaling.  macOS maps one point to one logical pixel,
// so the same SetPointSize(10) draws about 25% smaller there.  LbUiPt()
// scales a Windows point size to the same physical size on macOS and is the
// identity on Windows.  LbMonoFaceName() is the platform's built-in
// monospace face for the terminal-style chrome (Consolas is Windows-only).
#pragma once

inline int LbUiPt(int windowsPoints)
{
#ifdef __WXMAC__
    return (windowsPoints * 4 + 1) / 3;
#else
    return windowsPoints;
#endif
}

inline const char* LbMonoFaceName()
{
#ifdef __WXMAC__
    return "Menlo";
#else
    return "Consolas";
#endif
}
