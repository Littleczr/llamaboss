#pragma once

// ─── drop_import_controller.h ──────────────────────────────────────
// Drag-and-drop import coordinator for PDF, spreadsheet, DOCX, and
// CSV attachments.  LlamaBoss.cpp still owns the frame and drop
// target; this helper owns the shared "copy into cwd if needed,
// attach a chip, and show import status" workflow.
//
// The controller stays UI-light through callbacks so it does not need
// to know about MyFrame, ChatDisplay, or AttachmentManager directly.

#include <functional>
#include <string>

struct DropImportControllerCallbacks {
    std::function<bool()> isBusy;
    std::function<void(const std::string& message)> displaySystemMessage;
    std::function<std::string()> resolveCurrentCwd;
    std::function<void()> restoreComposerFocusDeferred;

    std::function<bool(const std::string& absPath,
                       const std::string& relPath)> attachPdfFile;
    std::function<bool(const std::string& absPath,
                       const std::string& relPath)> attachSpreadsheetFile;
    std::function<bool(const std::string& absPath,
                       const std::string& relPath)> attachDocxFile;
    std::function<bool(const std::string& absPath,
                       const std::string& relPath)> attachCsvFile;
    std::function<bool(const std::string& absPath,
                       const std::string& relPath)> attachZipFile;
};

class DropImportController
{
public:
    explicit DropImportController(DropImportControllerCallbacks callbacks);

    bool QueuePdfAttachmentFromDrop(const std::string& filePath) const;
    bool QueueSpreadsheetAttachmentFromDrop(const std::string& filePath) const;
    bool QueueDocxAttachmentFromDrop(const std::string& filePath) const;
    bool QueueCsvAttachmentFromDrop(const std::string& filePath) const;
    bool QueueZipAttachmentFromDrop(const std::string& filePath) const;

    void NotifyDocmDropRejected(const std::string& filePath) const;

private:
    struct DroppedFileSpec {
        std::string extLower;
        std::string displayLabel;
        std::string iconUtf8;
        unsigned long long byteCap = 0;
        std::function<bool(const std::string& absPath,
                           const std::string& relPath)> attach;
    };

    bool QueueDroppedFileImport(const std::string& filePath,
                                const DroppedFileSpec& spec) const;

    void DisplaySystemMessage(const std::string& message) const;

    DropImportControllerCallbacks m_callbacks;
};
