// presented_file.h
// A file presented in the chat — a clickable chip that can save content to disk.
//
// Designed to be producer-agnostic so that different sources (MarkdownRenderer
// code-block detection, future tool handlers like a PowerShell tool, an
// image-gen tool, a PDF extractor, etc.) can all funnel through the same
// ChatDisplay::PresentFile() rendering path.
//
// Content source is one of:
//   - inlineContent (bytes held in memory, e.g. a code block we just parsed)
//   - diskPath      (absolute path to a file already on disk, e.g. tool output)
// Exactly one should be populated per PresentedFile.

#pragma once

#include <string>
#include <cstddef>

struct PresentedFile {
    std::string displayName;      // Label shown on the chip, e.g. "block_1.cpp"
    std::string language;         // Optional language/type hint: "cpp", "powershell", ...

    // Content source — populate exactly one:
    std::string inlineContent;    // In-memory bytes.  Empty if diskPath is used.
    std::string diskPath;         // Absolute path on disk.  Empty if inlineContent is used.

    // Optional metadata rendered into the chip label.
    std::size_t sizeBytes = 0;
    int         lineCount = 0;
};
