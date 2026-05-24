#define _CRT_SECURE_NO_WARNINGS

// ─── drop_import_controller.cpp ────────────────────────────────────

#include "drop_import_controller.h"

#include <wx/wx.h>
#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <utility>

namespace {

std::string WxToUtf8Drop(const wxString& value)
{
    const wxScopedCharBuffer utf8 = value.ToUTF8();
    return utf8.data() ? std::string(utf8.data()) : std::string();
}

std::string DropImport_HumanBytes(wxULongLong bytes)
{
    const double value = static_cast<double>(bytes.GetValue());
    if (value < 1024.0) {
        return WxToUtf8Drop(wxString::Format("%.0f B", value));
    }
    if (value < 1024.0 * 1024.0) {
        return WxToUtf8Drop(wxString::Format("%.1f KB", value / 1024.0));
    }
    if (value < 1024.0 * 1024.0 * 1024.0) {
        return WxToUtf8Drop(wxString::Format("%.1f MB", value / (1024.0 * 1024.0)));
    }
    return WxToUtf8Drop(wxString::Format("%.1f GB",
                                         value / (1024.0 * 1024.0 * 1024.0)));
}

// Compute a path for `file` relative to `baseDir`.  Returns true and
// fills relPathOut iff the result stays inside baseDir (no leading
// "..", no drive letter).  False if the source lives outside baseDir
// — the caller then falls back to copying the file into baseDir.
bool DropImport_MakeRelativePathIfInsideCwd(const wxFileName& file,
                                            const wxString&   baseDir,
                                            wxString&         relPathOut)
{
    wxFileName rel(file);
    if (!rel.MakeRelativeTo(baseDir)) return false;

    wxString relPath = rel.GetFullPath();
    wxString relCheck = relPath;
    relCheck.Replace("\\", "/");

    if (relCheck == ".." || relCheck.StartsWith("../") ||
        relPath.Find(':') != wxNOT_FOUND) {
        return false;
    }

    relPathOut = relPath;
    return true;
}

// Pick a destination filename inside `dir` that doesn't collide with
// an existing file.  If <name>.<ext> exists, try <name> (1).<ext>,
// <name> (2).<ext>, etc.  Final fallback: <name> - imported.<ext>.
wxFileName DropImport_MakeUniqueDestination(const wxString&   dir,
                                            const wxFileName& src)
{
    wxFileName dest(dir, src.GetFullName());
    if (!dest.FileExists()) return dest;

    const wxString baseName = src.GetName();
    const wxString ext      = src.GetExt();

    for (int i = 1; i < 10000; ++i) {
        wxString candidateName = baseName +
            " (" + wxString::Format("%d", i) + ")";
        if (!ext.empty()) candidateName += "." + ext;

        wxFileName candidate(dir, candidateName);
        if (!candidate.FileExists()) return candidate;
    }

    wxString fallbackName = baseName + " - imported";
    if (!ext.empty()) fallbackName += "." + ext;
    return wxFileName(dir, fallbackName);
}

} // anonymous namespace

DropImportController::DropImportController(DropImportControllerCallbacks callbacks)
    : m_callbacks(std::move(callbacks))
{
}

void DropImportController::DisplaySystemMessage(const std::string& message) const
{
    if (m_callbacks.displaySystemMessage)
        m_callbacks.displaySystemMessage(message);
}

bool DropImportController::QueueDroppedFileImport(
    const std::string& filePath,
    const DroppedFileSpec& spec) const
{
    const std::string& label = spec.displayLabel;
    const std::string& icon  = spec.iconUtf8;

    if (m_callbacks.isBusy && m_callbacks.isBusy()) {
        DisplaySystemMessage(
            icon + " " + label + " Drop\n"
            "LlamaBoss is busy right now. Drop the " + label +
            " again after the current task finishes.");
        return false;
    }

    wxFileName source(wxString::FromUTF8(filePath));
    if (!source.FileExists()) return false;
    if (WxToUtf8Drop(source.GetExt().Lower()) != spec.extLower)
        return false;

    source.Normalize(wxPATH_NORM_ABSOLUTE |
                     wxPATH_NORM_DOTS |
                     wxPATH_NORM_TILDE);

    wxULongLong sourceSize = source.GetSize();

    if (sourceSize != wxInvalidSize &&
        sourceSize.GetValue() > spec.byteCap) {
        DisplaySystemMessage(
            icon + " " + label + " Import  \xC2\xB7  blocked\n"
            "> " + WxToUtf8Drop(source.GetFullName()) + "\n\n"
            "That " + label + " is too large for drag-and-drop import.\n"
            "Limit: " + DropImport_HumanBytes(wxULongLong(spec.byteCap)) + "\n"
            "File size: " + DropImport_HumanBytes(sourceSize));
        return false;
    }

    const std::string cwdText =
        m_callbacks.resolveCurrentCwd ? m_callbacks.resolveCurrentCwd()
                                      : std::string();
    wxString cwd = wxString::FromUTF8(cwdText);
    wxFileName cwdDir(cwd, wxEmptyString);
    cwdDir.Normalize(wxPATH_NORM_ABSOLUTE |
                     wxPATH_NORM_DOTS |
                     wxPATH_NORM_TILDE);
    cwd = cwdDir.GetPath();

    if (cwd.empty() || !wxDirExists(cwd)) {
        DisplaySystemMessage(
            icon + " " + label + " Drop\n"
            "The current LlamaBoss working directory was not found.\n\n"
            "Current working directory:\n" + WxToUtf8Drop(cwd));
        return false;
    }

    wxString relPath;
    wxFileName fileForTool(source);

    if (DropImport_MakeRelativePathIfInsideCwd(source, cwd, relPath)) {
        DisplaySystemMessage(
            icon + " " + label +
            " Ready  \xC2\xB7  already in working directory\n"
            "> " + WxToUtf8Drop(source.GetFullName()));
    } else {
        fileForTool = DropImport_MakeUniqueDestination(cwd, source);

        if (!wxCopyFile(source.GetFullPath(), fileForTool.GetFullPath(), false)) {
            DisplaySystemMessage(
                icon + " " + label + " Import  \xC2\xB7  failed\n"
                "> " + WxToUtf8Drop(source.GetFullName()) + "\n\n"
                "LlamaBoss could not copy the " + label +
                " into the current working directory.\n\n"
                "Current working directory:\n" + WxToUtf8Drop(cwd));
            return false;
        }

        if (!DropImport_MakeRelativePathIfInsideCwd(fileForTool, cwd, relPath)) {
            DisplaySystemMessage(
                icon + " " + label + " Import  \xC2\xB7  failed\n"
                "The " + label +
                " was copied, but LlamaBoss could not prepare a safe "
                "relative path for the tool.");
            return false;
        }

        std::string sizeText = (sourceSize == wxInvalidSize)
            ? std::string("unknown size")
            : DropImport_HumanBytes(sourceSize);

        DisplaySystemMessage(
            icon + " " + label +
            " Imported  \xC2\xB7  copied  \xC2\xB7  " + sizeText + "\n"
            "> " + WxToUtf8Drop(fileForTool.GetFullName()) + "\n\n"
            "Saved to working directory:\n"
            + WxToUtf8Drop(fileForTool.GetFullPath()));
    }

    if (!spec.attach) return false;

    const bool ok = spec.attach(
        WxToUtf8Drop(fileForTool.GetFullPath()),
        WxToUtf8Drop(relPath));
    if (ok && m_callbacks.restoreComposerFocusDeferred)
        m_callbacks.restoreComposerFocusDeferred();
    return ok;
}

bool DropImportController::QueuePdfAttachmentFromDrop(
    const std::string& filePath) const
{
    DroppedFileSpec spec;
    spec.extLower     = "pdf";
    spec.displayLabel = "PDF";
    spec.iconUtf8     = "\xF0\x9F\x93\x84";   // 📄
    spec.byteCap      = 100ULL * 1024ULL * 1024ULL;
    spec.attach       = m_callbacks.attachPdfFile;
    return QueueDroppedFileImport(filePath, spec);
}

bool DropImportController::QueueSpreadsheetAttachmentFromDrop(
    const std::string& filePath) const
{
    DroppedFileSpec spec;
    spec.extLower     = "xlsx";
    spec.displayLabel = "Spreadsheet";
    spec.iconUtf8     = "\xF0\x9F\x93\x8A";   // 📊
    spec.byteCap      = 100ULL * 1024ULL * 1024ULL;
    spec.attach       = m_callbacks.attachSpreadsheetFile;
    return QueueDroppedFileImport(filePath, spec);
}

bool DropImportController::QueueDocxAttachmentFromDrop(
    const std::string& filePath) const
{
    DroppedFileSpec spec;
    spec.extLower     = "docx";
    spec.displayLabel = "Word Document";
    spec.iconUtf8     = "\xF0\x9F\x93\x84";   // 📄
    spec.byteCap      = 100ULL * 1024ULL * 1024ULL;
    spec.attach       = m_callbacks.attachDocxFile;
    return QueueDroppedFileImport(filePath, spec);
}

void DropImportController::NotifyDocmDropRejected(
    const std::string& filePath) const
{
    wxFileName fn(wxString::FromUTF8(filePath));
    const std::string name = WxToUtf8Drop(fn.GetFullName());

    DisplaySystemMessage(
        "\xF0\x9F\x93\x84 Word Document (.docm)  \xC2\xB7  not auto-attached\n"
        "> " + name + "\n\n"
        "Macro-enabled .docm files can contain executable VBA macros, "
        "so LlamaBoss does not auto-attach them via drag-and-drop. "
        "If the file does not need macros, save it as .docx and try "
        "again. Otherwise place it manually in this conversation's "
        "working directory and ask the assistant to inspect it from "
        "there.");
}
