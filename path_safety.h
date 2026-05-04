// path_safety.h
//
// Centralized filename sanitization for any code path that lands a
// model-emitted or user-supplied name on disk.  Building tools that
// write files (file chips from streamed code blocks, image attachment
// persistence, future Create / Edit / git-apply tools, etc.) -- call
// SanitizeFilename at the boundary; do NOT trust callers to sanitize
// on the way in.
//
// Threat model: the input string may have come from a model response
// shaped by indirect prompt injection (e.g. a poisoned README that the
// agent read into context), or from a user-controlled filename in a
// dropped attachment.  The sanitizer must:
//
//   - Strip path separators ('/' and '\\') so output is always a
//     single filename component, never a path.
//   - Neutralize leading ".." runs that would form a traversal token.
//     Single leading dots are preserved so files like ".env" survive.
//   - Replace Windows-invalid chars ('< > : " | ? *') and ASCII
//     control characters.
//   - Strip trailing dots and spaces (Windows silently drops them on
//     file creation, which would desync the on-disk name from the UI).
//   - Reject Windows reserved device names (CON, PRN, AUX, NUL,
//     COM1-9, LPT1-9), case-insensitive, with or without extension.
//   - Cap length so the caller's prefix + sanitized name fits MAX_PATH
//     comfortably.  The extension is preserved when truncating.
//   - Never return an empty string -- fall back to the caller's
//     supplied default if the input sanitizes away to nothing.
//
// Output is always a single filename component.  It does NOT contain
// any directory separator.  Callers join it with their own absolute
// or relative directory prefix.

#pragma once

#include <string>

namespace path_safety {

// Cap on the sanitized filename component (before any prefix the
// caller adds).  Conservative: Windows MAX_PATH is 260, and callers
// typically prepend an absolute directory plus a numeric stem like
// "{msgIdx}_{chipSeq}_".  Leaves headroom for both.
constexpr size_t kMaxFilenameLen = 200;

// Returns a filesystem-safe single-component filename derived from
// `raw`.  If `raw` sanitizes to empty (or is itself empty), returns
// `fallback`.  `fallback` should already be safe -- it is NOT
// re-sanitized by this call.
std::string SanitizeFilename(const std::string& raw,
                             const std::string& fallback = "file");

// ── UTF-8 <-> wide conversion ────────────────────────────────────────
//
// Windows file APIs (and MSVC's std::ifstream/ofstream extension)
// accept std::wstring paths interpreted as UTF-16.  std::string paths
// passed to those same APIs are interpreted as the active ANSI
// codepage, NOT UTF-8 -- so a path containing non-ASCII characters
// (e.g. a Windows account named "Muller", or a filename with CJK)
// silently fails to open.  Use Utf8ToWide() at every fstream / Win32
// call boundary where the source path is UTF-8.
//
// Empty input or conversion failure returns an empty wstring/string.
std::wstring Utf8ToWide(const std::string& in);
std::string  WideToUtf8(const std::wstring& in);

} // namespace path_safety
