// pch.h
// Precompiled header for LlamaBoss.
//
// Contains only HEAVY, STABLE headers — wxWidgets, Poco, and the C++ standard
// library. Deliberately excludes LlamaBoss's own headers (agent_controller.h,
// tool_router.h, etc.): those change often, and anything that changes in the
// PCH forces a full rebuild, defeating the purpose.
//
// _CRT_SECURE_NO_WARNINGS lives here (before any include) so it is in effect
// before the CRT headers are pulled in by wx/Poco. Because the PCH is the
// first thing every translation unit sees, this define now applies project-
// wide — the per-file "#define _CRT_SECURE_NO_WARNINGS" on line 1 of
// LlamaBoss.cpp is redundant and should be deleted.
#pragma once

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

// ── wxWidgets ─────────────────────────────────────────────────────
// NOTE: we include the wx headers directly rather than <windows.h>. wx
// manages the Win32 inclusion carefully (NOMINMAX etc.); pulling raw
// <windows.h> into the PCH risks min/max macro pollution.
#include <wx/wx.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/artprov.h>
#include <wx/textdlg.h>
#include <wx/log.h>
#include <wx/utils.h>
#include <wx/thread.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/clipbrd.h>
#include <wx/mstream.h>
#include <wx/dir.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <wx/statline.h>

// ── Poco ──────────────────────────────────────────────────────────
#include <Poco/Base64Encoder.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>

// ── C++ standard library ──────────────────────────────────────────
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <functional>
#include <utility>
#include <optional>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <cstdint>
#include <cctype>
