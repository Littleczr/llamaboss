#define _CRT_SECURE_NO_WARNINGS

// attachment_manager.cpp
// Manages pending file and image attachments for chat messages.
// Supports multiple simultaneous attachments.

#include "attachment_manager.h"

#include <fstream>
#include <sstream>
#include <algorithm>

#include <Poco/Base64Encoder.h>
#include <Poco/Base64Decoder.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Logger.h>

// Use wxFileName only for lightweight path queries (extension, existence, size).
// No other wx UI dependency.
#include <wx/filename.h>

AttachmentManager::AttachmentManager(Poco::Logger* logger)
    : m_logger(logger)
{
}

// ═══════════════════════════════════════════════════════════════════
//  Attach Methods — each APPENDS to the pending list
// ═══════════════════════════════════════════════════════════════════

bool AttachmentManager::AttachImageFromFile(const std::string& filePath)
{
    if (m_pending.size() >= kMaxAttachments) return false;

    wxFileName fname(wxString::FromUTF8(filePath));
    if (!fname.FileExists()) return false;

    wxULongLong fileSize = fname.GetSize();
    std::string base64 = FileToBase64(filePath);
    if (base64.empty()) return false;

    std::string name(fname.GetFullName().ToUTF8().data());
    size_t origSize = (fileSize != wxInvalidSize)
                      ? static_cast<size_t>(fileSize.GetValue()) : 0;

    PendingAttachment item;
    item.type         = PendingAttachment::Type::Image;
    item.data         = std::move(base64);
    item.name         = std::move(name);
    item.originalSize = origSize;
    m_pending.push_back(std::move(item));

    if (m_logger)
        m_logger->information("Image attached: " + m_pending.back().name +
            " (" + std::to_string(m_pending.back().data.size()) + " bytes base64)"
            " [" + std::to_string(m_pending.size()) + " pending]");

    NotifyChanged();
    return true;
}

bool AttachmentManager::AttachImageFromBase64(const std::string& base64,
                                               const std::string& displayName)
{
    if (m_pending.size() >= kMaxAttachments) return false;
    if (base64.empty()) return false;

    PendingAttachment item;
    item.type         = PendingAttachment::Type::Image;
    item.data         = base64;
    item.name         = displayName;
    item.originalSize = 0;  // Unknown for clipboard images
    m_pending.push_back(std::move(item));

    if (m_logger)
        m_logger->information("Image attached (clipboard): " + m_pending.back().name +
            " (" + std::to_string(m_pending.back().data.size()) + " bytes base64)"
            " [" + std::to_string(m_pending.size()) + " pending]");

    NotifyChanged();
    return true;
}

bool AttachmentManager::AttachTextFile(const std::string& filePath)
{
    if (m_pending.size() >= kMaxAttachments) return false;

    wxFileName fname(wxString::FromUTF8(filePath));
    if (!fname.FileExists()) return false;

    // Size guard
    wxULongLong fileSize = fname.GetSize();
    if (fileSize == wxInvalidSize || fileSize.GetValue() > kMaxTextFileBytes)
        return false;

    std::ifstream ifs(filePath, std::ios::in);
    if (!ifs.is_open()) return false;

    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string content = oss.str();
    if (content.empty()) return false;

    PendingAttachment item;
    item.type         = PendingAttachment::Type::TextFile;
    item.data         = std::move(content);
    item.name         = std::string(fname.GetFullName().ToUTF8().data());
    item.originalSize = static_cast<size_t>(fileSize.GetValue());
    m_pending.push_back(std::move(item));

    if (m_logger)
        m_logger->information("Text file attached: " + m_pending.back().name +
            " (" + std::to_string(m_pending.back().data.size()) + " bytes)"
            " [" + std::to_string(m_pending.size()) + " pending]");

    NotifyChanged();
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  Remove / Clear
// ═══════════════════════════════════════════════════════════════════

void AttachmentManager::RemoveAt(size_t index)
{
    if (index >= m_pending.size()) return;

    if (m_logger)
        m_logger->information("Attachment removed: " + m_pending[index].name +
            " [" + std::to_string(m_pending.size() - 1) + " remaining]");

    m_pending.erase(m_pending.begin() + static_cast<ptrdiff_t>(index));
    NotifyChanged();
}

void AttachmentManager::Clear()
{
    if (m_pending.empty()) return;  // avoid spurious callbacks

    if (m_logger)
        m_logger->information("All attachments cleared (" +
            std::to_string(m_pending.size()) + " removed)");

    m_pending.clear();
    NotifyChanged();
}

// ═══════════════════════════════════════════════════════════════════
//  Query
// ═══════════════════════════════════════════════════════════════════

bool AttachmentManager::HasImage() const
{
    for (const auto& item : m_pending)
        if (item.type == PendingAttachment::Type::Image) return true;
    return false;
}

bool AttachmentManager::HasTextFile() const
{
    for (const auto& item : m_pending)
        if (item.type == PendingAttachment::Type::TextFile) return true;
    return false;
}

std::string AttachmentManager::GetDisplayLabel() const
{
    if (m_pending.empty()) return "";

    if (m_pending.size() == 1) {
        const auto& item = m_pending[0];
        if (item.type == PendingAttachment::Type::Image)
            return "  \xF0\x9F\x96\xBC  " + item.name;   // 🖼
        else
            return "  \xF0\x9F\x93\x84  " + item.name;   // 📄
    }

    // Multiple items — show paperclip + count + abbreviated file list
    // 📎 U+1F4CE
    std::string label = "  \xF0\x9F\x93\x8E  "
        + std::to_string(m_pending.size()) + " files: ";
    for (size_t i = 0; i < m_pending.size(); ++i) {
        if (i > 0) label += ", ";
        label += m_pending[i].name;
    }
    return label;
}

std::vector<std::string> AttachmentManager::GetFileNames() const
{
    std::vector<std::string> names;
    names.reserve(m_pending.size());
    for (const auto& item : m_pending)
        names.push_back(item.name);
    return names;
}

std::vector<AttachmentInfo> AttachmentManager::GetAttachmentInfo() const
{
    std::vector<AttachmentInfo> result;
    result.reserve(m_pending.size());

    for (const auto& item : m_pending) {
        AttachmentInfo info;
        info.kind     = (item.type == PendingAttachment::Type::Image)
                        ? AttachmentInfo::Kind::Image
                        : AttachmentInfo::Kind::TextFile;
        info.filename = item.name;
        info.mimeType = GuessMimeType(item.name);
        info.byteSize = item.originalSize;
        result.push_back(std::move(info));
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  Serialization
// ═══════════════════════════════════════════════════════════════════

std::string AttachmentManager::BakeTextFilesIntoMessage(const std::string& userText) const
{
    // Prepend each text file as a code-fenced block before the user's text.
    // Images are not baked — they go through InjectImagesIntoRequest.
    std::string baked;

    for (const auto& item : m_pending) {
        if (item.type != PendingAttachment::Type::TextFile) continue;
        baked += "[File: " + item.name + "]\n"
                 "```\n" + item.data + "\n```\n\n";
    }

    if (baked.empty()) return userText;
    return baked + userText;
}

std::string AttachmentManager::InjectImagesIntoRequest(const std::string& requestJson) const
{
    // Count pending images — bail early if none
    bool anyImages = false;
    for (const auto& item : m_pending)
        if (item.type == PendingAttachment::Type::Image) { anyImages = true; break; }
    if (!anyImages) return requestJson;

    try {
        Poco::JSON::Parser parser;
        auto root = parser.parse(requestJson).extract<Poco::JSON::Object::Ptr>();
        auto messages = root->getArray("messages");

        if (messages && messages->size() > 0) {
            // Find the last user message
            for (int i = static_cast<int>(messages->size()) - 1; i >= 0; --i) {
                auto msg = messages->getObject(i);
                if (msg->getValue<std::string>("role") == "user") {
                    std::string textContent = msg->getValue<std::string>("content");

                    // Build OpenAI-style content array with all images + text.
                    Poco::JSON::Array::Ptr contentArray = new Poco::JSON::Array;

                    for (const auto& item : m_pending) {
                        if (item.type != PendingAttachment::Type::Image) continue;

                        std::string mime = GuessMimeType(item.name);
                        Poco::JSON::Object::Ptr imagePart = new Poco::JSON::Object;
                        imagePart->set("type", "image_url");
                        Poco::JSON::Object::Ptr imageUrl = new Poco::JSON::Object;
                        imageUrl->set("url", "data:" + mime + ";base64," + item.data);
                        imagePart->set("image_url", imageUrl);
                        contentArray->add(imagePart);
                    }

                    Poco::JSON::Object::Ptr textPart = new Poco::JSON::Object;
                    textPart->set("type", "text");
                    textPart->set("text", textContent);
                    contentArray->add(textPart);

                    msg->set("content", contentArray);
                    break;
                }
            }
        }

        std::ostringstream oss;
        Poco::JSON::Stringifier::stringify(root, oss);
        return oss.str();
    }
    catch (const Poco::Exception& ex) {
        if (m_logger)
            m_logger->error("Failed to inject images: " + ex.displayText());
        return requestJson;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Image Persistence (Phase 3)
// ═══════════════════════════════════════════════════════════════════

bool AttachmentManager::SaveImagesToDisk(const std::string& attachDir,
                                          const std::string& relativePrefix,
                                          size_t messageIndex,
                                          std::vector<AttachmentInfo>& infoVec) const
{
    // Create the sidecar directory if it doesn't exist
    wxFileName dirPath(wxString::FromUTF8(attachDir) + wxFileName::GetPathSeparator());
    if (!dirPath.DirExists())
        dirPath.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    bool allOk = true;

    // Track used filenames within this batch to avoid collisions
    // (e.g. multiple clipboard_image.png pastes, or same-named files
    // dragged from different folders).
    std::vector<std::string> usedFilenames;

    for (size_t i = 0; i < m_pending.size() && i < infoVec.size(); ++i) {
        const auto& item = m_pending[i];
        if (item.type != PendingAttachment::Type::Image) continue;

        // Build base filename: "{messageIndex}_{originalName}"
        std::string baseFilename = std::to_string(messageIndex) + "_" + item.name;

        // Deduplicate: if this name is already used, insert a suffix.
        // "2_photo.png" → "2_photo_2.png", "2_photo_3.png", ...
        std::string filename = baseFilename;
        if (std::find(usedFilenames.begin(), usedFilenames.end(), filename) != usedFilenames.end()) {
            // Split into stem + extension for clean suffix insertion
            size_t dot = baseFilename.rfind('.');
            std::string stem = (dot != std::string::npos) ? baseFilename.substr(0, dot) : baseFilename;
            std::string ext  = (dot != std::string::npos) ? baseFilename.substr(dot)    : "";

            int suffix = 2;
            do {
                filename = stem + "_" + std::to_string(suffix) + ext;
                ++suffix;
            } while (std::find(usedFilenames.begin(), usedFilenames.end(), filename) != usedFilenames.end());
        }
        usedFilenames.push_back(filename);

        std::string fullPath = attachDir + "/" + filename;

        try {
            // Decode base64 back to raw binary
            std::istringstream base64Stream(item.data);
            Poco::Base64Decoder decoder(base64Stream);

            std::ofstream outFile(fullPath, std::ios::binary);
            if (!outFile.is_open()) {
                allOk = false;
                if (m_logger)
                    m_logger->error("Cannot open for writing: " + fullPath);
                continue;
            }

            std::copy(std::istreambuf_iterator<char>(decoder),
                      std::istreambuf_iterator<char>(),
                      std::ostreambuf_iterator<char>(outFile));
            outFile.close();

            // Store relative path (forward slashes for portability)
            infoVec[i].storagePath = relativePrefix + "/" + filename;

            if (m_logger)
                m_logger->information("Image persisted: " + fullPath +
                    " (" + std::to_string(item.originalSize) + " bytes)");
        }
        catch (const std::exception& ex) {
            allOk = false;
            if (m_logger)
                m_logger->error("Failed to persist image: " + std::string(ex.what()));
        }
    }

    return allOk;
}

// ═══════════════════════════════════════════════════════════════════
//  File Classification
// ═══════════════════════════════════════════════════════════════════

bool AttachmentManager::IsImageFile(const std::string& path)
{
    wxString ext = wxFileName(wxString::FromUTF8(path)).GetExt().Lower();
    return ext == "png" || ext == "jpg" || ext == "jpeg" ||
           ext == "gif" || ext == "bmp" || ext == "webp";
}

bool AttachmentManager::IsTextFile(const std::string& path)
{
    wxString ext = wxFileName(wxString::FromUTF8(path)).GetExt().Lower();
    return ext == "txt" || ext == "md"   || ext == "json" ||
           ext == "cpp" || ext == "h"    || ext == "hpp"  ||
           ext == "py"  || ext == "js"   || ext == "ts"   ||
           ext == "css" || ext == "html" || ext == "xml"  ||
           ext == "yaml"|| ext == "yml"  || ext == "toml" ||
           ext == "csv" || ext == "log"  || ext == "ini"  ||
           ext == "cfg" || ext == "sh"   || ext == "bat";
}

std::string AttachmentManager::GuessMimeType(const std::string& filename)
{
    // Extract extension
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = filename.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Images
    if (ext == "png")                return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")                return "image/gif";
    if (ext == "bmp")                return "image/bmp";
    if (ext == "webp")               return "image/webp";

    // Text / code
    if (ext == "txt" || ext == "log") return "text/plain";
    if (ext == "md")                 return "text/markdown";
    if (ext == "json")               return "application/json";
    if (ext == "html")               return "text/html";
    if (ext == "xml")                return "text/xml";
    if (ext == "css")                return "text/css";
    if (ext == "csv")                return "text/csv";
    if (ext == "yaml" || ext == "yml") return "text/yaml";
    if (ext == "toml")               return "text/plain";
    if (ext == "ini" || ext == "cfg") return "text/plain";
    if (ext == "sh" || ext == "bat") return "text/plain";
    if (ext == "cpp" || ext == "h" || ext == "hpp") return "text/x-c++src";
    if (ext == "py")                 return "text/x-python";
    if (ext == "js")                 return "text/javascript";
    if (ext == "ts")                 return "text/typescript";

    return "application/octet-stream";
}

// ═══════════════════════════════════════════════════════════════════
//  Private Helpers
// ═══════════════════════════════════════════════════════════════════

void AttachmentManager::NotifyChanged()
{
    if (m_onChanged) m_onChanged();
}

std::string AttachmentManager::FileToBase64(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return "";

    std::ostringstream base64Stream;
    Poco::Base64Encoder encoder(base64Stream);
    encoder << file.rdbuf();
    encoder.close();

    std::string result = base64Stream.str();
    result.erase(std::remove_if(result.begin(), result.end(), ::isspace),
                 result.end());
    return result;
}
