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
// first thing every translation unit sees, this define applies project-wide,
// so .cpp files must NOT define it themselves.
//
// Because pch.h is force-included (/FI) into every TU of LlamaBoss.vcxproj,
// .cpp files in that project don't need to re-include anything listed
// here. Exceptions: .cpp files that are also built WITHOUT the PCH
// (LlamaBossTests: path_safety, tool_path, tool_read, var_store; the
// standalone harnesses: command_policy, tool_call_parser) keep their
// includes, and headers stay self-contained.
#pragma once

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

// ── wxWidgets ─────────────────────────────────────────────────────
// <windows.h> is never included raw: it comes in once, via lb_windows.h
// (NOMINMAX + WIN32_LEAN_AND_MEAN), just below this block.
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
#include <wx/stdpaths.h>

// ── Win32 ─────────────────────────────────────────────────────────
// Same macros Poco/UnWindows.h uses, at the same point it would pull
// <windows.h> in anyway -- this just makes it explicit and project-owned.
#include "lb_windows.h"

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
#include <Poco/Dynamic/Var.h>
#include <Poco/Exception.h>

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
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <set>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
