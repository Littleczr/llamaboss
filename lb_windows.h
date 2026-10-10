// lb_windows.h
//
// The ONE way LlamaBoss code includes <windows.h>.
//
// Always defines NOMINMAX (no min/max macros fighting std::min/std::max)
// and WIN32_LEAN_AND_MEAN (skip winsock 1, ole2, shellapi, wincrypt,
// mmsystem, ...). Files that need one of the excluded headers include it
// explicitly AFTER this one: <shellapi.h>, <wincrypt.h>, <shlobj.h>,
// <winhttp.h>, <bcrypt.h>, <winsock2.h>, ...
//
// These are exactly the macros Poco/UnWindows.h sets before it includes
// <windows.h>. In LlamaBoss.vcxproj, pch.h includes this header, so every
// translation unit already has it; the per-file includes matter for
// builds WITHOUT the PCH (LlamaBossTests and the standalone *_tests.cpp
// harnesses), which is why headers and those .cpp files keep including
// it directly.
//
// Safe to include on non-Windows builds: it does nothing there.
#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
