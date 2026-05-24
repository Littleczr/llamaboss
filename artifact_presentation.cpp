#define _CRT_SECURE_NO_WARNINGS

// artifact_presentation.cpp
#include "artifact_presentation.h"

#include "lb_string_utils.h"
#include "presented_file.h"
#include "tool_dispatcher.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::string LbPresentedFileExtLower(const PresentedFile& f)
{
    std::string name = !f.displayName.empty() ? f.displayName : f.diskPath;
    std::replace(name.begin(), name.end(), '\\', '/');
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return std::string();
    return LbLowerAscii(name.substr(dot + 1));
}

struct ArtifactPresentation {
    std::string iconUtf8;
    std::string toolName;
};

ArtifactPresentation BuildArtifactPresentation(const std::vector<PresentedFile>& files)
{
    ArtifactPresentation p;
    if (files.empty()) return p;

    bool hasDocx = false;
    bool hasSheet = false;
    bool hasPdf = false;
    bool hasMarkdown = false;
    bool hasText = false;
    bool hasImage = false;
    bool hasOther = false;

    for (const auto& f : files) {
        const std::string ext = LbPresentedFileExtLower(f);
        const std::string lang = LbLowerAscii(f.language);

        if (ext == "docx") {
            hasDocx = true;
        } else if (ext == "xlsx" || ext == "csv") {
            hasSheet = true;
        } else if (ext == "pdf") {
            hasPdf = true;
        } else if (ext == "md" || ext == "markdown" || lang == "markdown" || lang == "md") {
            hasMarkdown = true;
        } else if (ext == "txt" || lang == "text") {
            hasText = true;
        } else if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp") {
            hasImage = true;
        } else {
            hasOther = true;
        }
    }

    const int kinds = (hasDocx ? 1 : 0) + (hasSheet ? 1 : 0) +
                      (hasPdf ? 1 : 0) + (hasMarkdown ? 1 : 0) +
                      (hasText ? 1 : 0) + (hasImage ? 1 : 0) +
                      (hasOther ? 1 : 0);

    if (files.size() == 1 && kinds == 1) {
        if (hasDocx)     return { "\xF0\x9F\x93\x84", "Create Word Document" };      // 📄
        if (hasSheet)    return { "\xF0\x9F\x93\x8A", "Create Spreadsheet" };        // 📊
        if (hasPdf)      return { "\xF0\x9F\x93\x84", "Create PDF" };                 // 📄
        if (hasMarkdown) return { "\xF0\x9F\x93\x9D", "Create Markdown Document" };  // 📝
        if (hasText)     return { "\xF0\x9F\x93\x84", "Create Text Document" };       // 📄
        if (hasImage)    return { "\xF0\x9F\x96\xBC", "Create Image" };               // 🖼
        return { "\xF0\x9F\x93\x8E", "Create File" };                                 // 📎
    }

    if (hasDocx && kinds == 1)      return { "\xF0\x9F\x93\x84", "Create Word Documents" };
    if (hasSheet && kinds == 1)     return { "\xF0\x9F\x93\x8A", "Create Spreadsheets" };
    if (hasPdf && kinds == 1)       return { "\xF0\x9F\x93\x84", "Create PDFs" };
    if (hasMarkdown && kinds == 1) return { "\xF0\x9F\x93\x9D", "Create Markdown Documents" };

    return { "\xF0\x9F\x93\xA6", "Create Files" };                                    // 📦
}

} // namespace

void ApplyArtifactPresentation(ToolInvocationResult& result)
{
    if (result.presentedFiles.empty()) return;
    if (!result.errorBody.empty()) return;

    ArtifactPresentation p = BuildArtifactPresentation(result.presentedFiles);
    if (p.toolName.empty()) return;

    result.iconUtf8 = p.iconUtf8;
    result.toolName = p.toolName;
}
