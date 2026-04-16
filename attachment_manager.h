// attachment_manager.h
// Manages pending file and image attachments for chat messages.
// Supports multiple simultaneous attachments (images + text files).
// Owns attachment data and serialization logic; UI updates are
// driven by a callback so this class has no wxWidgets dependency.

#pragma once

#include <string>
#include <functional>
#include <vector>

namespace Poco { class Logger; }

// ═══════════════════════════════════════════════════════════════════
//  AttachmentInfo — lightweight metadata that lives on a ChatHistory
//  message. No payload, no UI, just "what was attached".
//  This is the foundation for structured message parts.
// ═══════════════════════════════════════════════════════════════════

struct AttachmentInfo {
    enum class Kind { Image, TextFile };
    Kind        kind;
    std::string filename;     // "report.cpp", "photo.png"
    std::string mimeType;     // "text/x-c++src", "image/png"
    size_t      byteSize = 0; // original file size in bytes

    // ── Persistence (Phase 3) ────────────────────────────────
    std::string storagePath;   // Sidecar path relative to conversations dir
                               // e.g. "attachments/chat_abc12345/0_photo.png"
                               // Empty if not persisted to disk.
    // std::string displayHint;   // "code", "document", "photo"
};

// ═══════════════════════════════════════════════════════════════════
//  PendingAttachment — a single queued file waiting to be sent.
//  Lives only inside AttachmentManager; consumed and cleared on send.
// ═══════════════════════════════════════════════════════════════════

struct PendingAttachment {
    enum class Type { Image, TextFile };
    Type        type;
    std::string data;           // base64 for images, raw text for text files
    std::string name;           // display filename
    size_t      originalSize = 0;
};

// ═══════════════════════════════════════════════════════════════════

class AttachmentManager
{
public:
    // Callback fired whenever the pending list changes (add, remove, clear).
    // The frame responds by reading GetDisplayLabel() / GetCount() and
    // updating its indicator UI accordingly.
    using OnChangedCallback = std::function<void()>;

    explicit AttachmentManager(Poco::Logger* logger = nullptr);
    ~AttachmentManager() = default;

    void SetOnChanged(OnChangedCallback cb) { m_onChanged = std::move(cb); }
    void SetLogger(Poco::Logger* logger)    { m_logger = logger; }

    // ── Attach methods (additive — each appends to the pending list) ──
    // Returns false on I/O error or validation failure.

    bool AttachImageFromFile(const std::string& filePath);
    bool AttachImageFromBase64(const std::string& base64,
                               const std::string& displayName);
    bool AttachTextFile(const std::string& filePath);

    // ── Remove / clear ────────────────────────────────────────────

    void RemoveAt(size_t index);   // Remove one item by index
    void Clear();                  // Remove all pending items

    // ── Query ─────────────────────────────────────────────────────

    bool   HasPending()  const { return !m_pending.empty(); }
    bool   HasImage()    const;    // true if any pending item is an image
    bool   HasTextFile() const;    // true if any pending item is a text file
    size_t GetCount()    const { return m_pending.size(); }

    const PendingAttachment& GetAt(size_t index) const { return m_pending.at(index); }

    // Display label for the attachment indicator bar.
    //   1 item:   "  🖼  photo.png" / "  📄  main.cpp"
    //   N items:  "  📎  3 files: photo.png, main.cpp, utils.h"
    //   0 items:  ""
    std::string GetDisplayLabel() const;

    // All pending filenames (for building chat display prefixes).
    std::vector<std::string> GetFileNames() const;

    // Returns structured metadata for ALL pending attachments.
    std::vector<AttachmentInfo> GetAttachmentInfo() const;

    // ── Serialization for send flow ───────────────────────────────

    // Bakes ALL pending text files into the message as code-fenced blocks.
    // Images are skipped (they go through InjectImagesIntoRequest instead).
    std::string BakeTextFilesIntoMessage(const std::string& userText) const;

    // Injects ALL pending images into the Ollama /api/chat request JSON,
    // converting the last user message's "content" to a multimodal array.
    std::string InjectImagesIntoRequest(const std::string& requestJson) const;

    // ── Image persistence (Phase 3) ──────────────────────────────

    // Save all pending images to sidecar files on disk.
    // attachDir:       absolute path to the target directory (created if needed)
    // relativePrefix:  path prefix for storagePath (e.g. "attachments/chat_abc12345")
    // messageIndex:    message index used in filename (e.g. 2 → "2_photo.png")
    // infoVec:         AttachmentInfo vector — storagePath is set for each saved image
    // Returns true if all images saved successfully.
    bool SaveImagesToDisk(const std::string& attachDir,
                          const std::string& relativePrefix,
                          size_t messageIndex,
                          std::vector<AttachmentInfo>& infoVec) const;

    // ── File classification (static helpers) ──────────────────────

    static bool IsImageFile(const std::string& path);
    static bool IsTextFile(const std::string& path);
    static std::string GuessMimeType(const std::string& filename);

    static constexpr size_t kMaxTextFileBytes = 100 * 1024;  // 100 KB
    static constexpr size_t kMaxAttachments   = 10;           // Max pending files

private:
    std::vector<PendingAttachment> m_pending;

    Poco::Logger*     m_logger = nullptr;
    OnChangedCallback m_onChanged;

    void NotifyChanged();
    static std::string FileToBase64(const std::string& filePath);
};
