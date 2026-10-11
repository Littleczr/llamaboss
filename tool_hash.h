// tool_hash.h
//
// hash: read-only SHA-256 of one file, optionally checked against an
// expected digest or against a second file.
//
// Why a tool and not a script: release and verification work kept
// generating throwaway python_create_script/python_run_script pairs just
// to print a digest (seven in one website-deploy chat, three of which
// crashed), and the model then RETYPED 64-character digests into its
// reports, where one lost six characters.  This tool prints the digest and,
// when given something to compare against, a literal "Match: True/False"
// line the model can quote instead of transcribing hex.
//
// Args (legacy/XML and the native projection share this shape):
//   line 1: path of the file to hash
//   line 2: optional  - a 64-hex SHA-256 to compare against, or
//                     - a second file path (byte-for-byte digest compare)
// A one-line form "path <64-hex>" is also accepted.
//
// Paths resolve like read: cwd/project relative, absolute local paths
// outside the cwd, and conversation lanes (Scripts\, ToolOutputs\, ...).
// Files are streamed, so installer-sized inputs are fine.
//
#pragma once

#include "tool_context.h"

#include <cstdint>
#include <string>
#include <vector>

struct HashResult {
    //   ["528 B", "match", "0.00s"] / ["4.2 MB", "MISMATCH", ...] /
    //   ["failed", "0.00s"]
    std::vector<std::string> chips;
    std::string body;
    std::string errorBody;
    std::string bodyLang;   // always empty
};

namespace tool_hash {

struct Args {
    std::string path;
    std::string expected;   // empty, a digest, or a second path
};

// Pure parser (unit-tested).  Never throws.
Args ParseArgs(const std::string& args);

// Streams the file through SHA-256.  Returns false with `error` set when
// the file cannot be opened or read.  `absPath` must already be resolved.
bool HashFileAt(const std::string& absPath,
                std::string& hexOut,
                std::uint64_t& sizeOut,
                std::string& error);

} // namespace tool_hash

HashResult HashFile(const std::string& args, const ToolContext& ctx);
