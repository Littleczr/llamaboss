// lb_mac_clipboard.h
//
// macOS only: read a picture from the general pasteboard as PNG bytes.
//
// wxWidgets on macOS does not report screenshots or copied images as
// wxDF_BITMAP (the pasteboard holds public.png / public.tiff), so the
// composer's image paste reads NSPasteboard directly.  Returns false when
// the pasteboard holds no image; file URLs (Finder copies) are left to the
// normal paste so their names paste as text, as before.
#pragma once

#ifdef __WXMAC__
#include <vector>

bool LbMacClipboardImagePng(std::vector<unsigned char>& png);
#endif
