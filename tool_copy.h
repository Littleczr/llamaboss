// tool_copy.h
//
// copy: byte-exact file copy into a trusted write root, verified by
// SHA-256 after the write.
//
// Before this tool existed the harness told models "there is no native
// copy tool; read the file and write its contents".  For a request like
// "make staging version.json byte-identical to the live one" that meant
// retyping a JSON document containing a 64-character SHA-256 through
// overwrite_file -- exactly the transcription step that later dropped six
// characters from a reported hash.  copy moves bytes without the model
// touching them and reports the verified digest.
//
// Args (legacy/XML and the native projection share this shape):
//   line 1: source file (read-style resolution: cwd/project relative,
//           absolute local paths, conversation lanes such as ToolOutputs\)
//   line 2: destination file (write-style: must land in a trusted write
//           root; folder grants apply exactly as for overwrite_file)
//   line 3: optional "overwrite" to replace an existing destination
//
// Safety: the destination goes through the same gates as overwrite_file
// (containment, mutation guard, filename sanitization, executable /
// scriptable extension block, parent must exist, staged temp + atomic
// promote).  Sources are capped at kExactWriteMaxBytes (64 MiB).
//
#pragma once

#include "tool_context.h"

#include <cstddef>
#include <string>
#include <vector>

struct CopyResult {
    //   ["copied", "528 B", "verified", "0.01s"] / ["overwritten", ...] /
    //   ["exists", ...] / ["blocked", ...] / ["failed", ...]
    std::vector<std::string> chips;
    std::string body;
    std::string errorBody;
    std::string bodyLang;   // always empty

    // Success metadata for the artifact card (same shape as WriteResult).
    std::string createdPath;
    std::string displayName;
    size_t      sizeBytes = 0;
    int         lineCount = 0;
};

namespace tool_copy {

struct Args {
    std::string source;
    std::string destination;
    bool        overwrite = false;
    std::string error;      // non-empty when the shape is wrong
};

// Pure parser (unit-tested).  Never throws.
Args ParseArgs(const std::string& args);

} // namespace tool_copy

CopyResult CopyFileExact(const std::string& args, const ToolContext& ctx);
