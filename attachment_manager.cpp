#define _CRT_SECURE_NO_WARNINGS

// attachment_manager.cpp
// Manages pending file and image attachments for chat messages.
// Supports multiple simultaneous attachments.

#include "attachment_manager.h"
#include "path_safety.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <initializer_list>

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


namespace {

std::string LowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return s;
}

bool ContainsAny(const std::string& haystack,
                 std::initializer_list<const char*> needles)
{
    for (const char* n : needles) {
        if (haystack.find(n) != std::string::npos)
            return true;
    }
    return false;
}

bool LooksLikePdfFormFillRequest(const std::string& userText)
{
    const std::string t = LowerAscii(userText);

    // High-confidence form-fill verbs. These should route attached PDFs to
    // pdf_inspect_form -> pdf_fill_form, not to text extraction / drafting.
    if (ContainsAny(t, {
            "fill", "filled", "fill in", "complete this form",
            "complete the form", "populate", "put this into the pdf",
            "put it into the pdf", "pdf form", "form template"})) {
        return true;
    }

    // Common disciplinary-template language Cesar uses with blank Allied forms.
    // With an attached PDF, these usually mean "create the completed notice in
    // the PDF", not "stop after a text draft".
    if (ContainsAny(t, {
            "write up", "write-up", "disciplinary notice", "disciplinary form",
            "coaching", "counseling", "final warning", "written warning",
            "facts", "expectation", "corrective action"})) {
        return true;
    }

    return false;
}

} // namespace

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

    std::ifstream ifs(path_safety::Utf8ToWide(filePath), std::ios::in);
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

bool AttachmentManager::AttachPdfFile(const std::string& filePath,
                                      const std::string& toolRelativePath)
{
    if (m_pending.size() >= kMaxAttachments) return false;

    wxFileName fname(wxString::FromUTF8(filePath));
    if (!fname.FileExists()) return false;
    if (fname.GetExt().Lower() != "pdf") return false;

    wxULongLong fileSize = fname.GetSize();

    PendingAttachment item;
    item.type         = PendingAttachment::Type::PdfFile;
    // For PDFs, data is not raw file text. It is the safe relative tool arg
    // for pdf_extract_text, prepared by MyFrame after importing to the cwd.
    item.data         = toolRelativePath.empty()
                      ? std::string(fname.GetFullName().ToUTF8().data())
                      : toolRelativePath;
    item.name         = std::string(fname.GetFullName().ToUTF8().data());
    item.originalSize = (fileSize != wxInvalidSize)
                      ? static_cast<size_t>(fileSize.GetValue()) : 0;
    m_pending.push_back(std::move(item));

    if (m_logger)
        m_logger->information("PDF attached: " + m_pending.back().name +
            " -> pdf_extract_text arg: " + m_pending.back().data +
            " [" + std::to_string(m_pending.size()) + " pending]");

    NotifyChanged();
    return true;
}

bool AttachmentManager::AttachSpreadsheetFile(const std::string& filePath,
                                               const std::string& toolRelativePath)
{
    if (m_pending.size() >= kMaxAttachments) return false;

    wxFileName fname(wxString::FromUTF8(filePath));
    if (!fname.FileExists()) return false;
    if (fname.GetExt().Lower() != "xlsx") return false;

    wxULongLong fileSize = fname.GetSize();

    PendingAttachment item;
    item.type         = PendingAttachment::Type::SpreadsheetFile;
    // For spreadsheets, data is the safe relative tool arg for xlsx_inspect
    // and xlsx_report, prepared by MyFrame after importing to the cwd.
    item.data         = toolRelativePath.empty()
                      ? std::string(fname.GetFullName().ToUTF8().data())
                      : toolRelativePath;
    item.name         = std::string(fname.GetFullName().ToUTF8().data());
    item.originalSize = (fileSize != wxInvalidSize)
                      ? static_cast<size_t>(fileSize.GetValue()) : 0;
    m_pending.push_back(std::move(item));

    if (m_logger)
        m_logger->information("Spreadsheet attached: " + m_pending.back().name +
            " -> xlsx tool arg: " + m_pending.back().data +
            " [" + std::to_string(m_pending.size()) + " pending]");

    NotifyChanged();
    return true;
}

bool AttachmentManager::AttachDocxFile(const std::string& filePath,
                                       const std::string& toolRelativePath)
{
    if (m_pending.size() >= kMaxAttachments) return false;

    wxFileName fname(wxString::FromUTF8(filePath));
    if (!fname.FileExists()) return false;
    wxString ext = fname.GetExt().Lower();
    if (ext != "docx" && ext != "docm") return false;

    wxULongLong fileSize = fname.GetSize();

    PendingAttachment item;
    item.type         = PendingAttachment::Type::DocxFile;
    // For DOCX, data is the safe relative tool arg for docx_extract_text /
    // docx_inspect, prepared by MyFrame after importing to the cwd.
    item.data         = toolRelativePath.empty()
                      ? std::string(fname.GetFullName().ToUTF8().data())
                      : toolRelativePath;
    item.name         = std::string(fname.GetFullName().ToUTF8().data());
    item.originalSize = (fileSize != wxInvalidSize)
                      ? static_cast<size_t>(fileSize.GetValue()) : 0;
    m_pending.push_back(std::move(item));

    if (m_logger)
        m_logger->information("DOCX attached: " + m_pending.back().name +
            " -> docx tool arg: " + m_pending.back().data +
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

bool AttachmentManager::HasPdfFile() const
{
    for (const auto& item : m_pending)
        if (item.type == PendingAttachment::Type::PdfFile) return true;
    return false;
}

bool AttachmentManager::HasSpreadsheetFile() const
{
    for (const auto& item : m_pending)
        if (item.type == PendingAttachment::Type::SpreadsheetFile) return true;
    return false;
}

bool AttachmentManager::HasDocxFile() const
{
    for (const auto& item : m_pending)
        if (item.type == PendingAttachment::Type::DocxFile) return true;
    return false;
}

std::string AttachmentManager::GetDisplayLabel() const
{
    if (m_pending.empty()) return "";

    if (m_pending.size() == 1) {
        const auto& item = m_pending[0];
        if (item.type == PendingAttachment::Type::Image)
            return "  \xF0\x9F\x96\xBC  " + item.name;   // 🖼
        if (item.type == PendingAttachment::Type::SpreadsheetFile)
            return "  \xF0\x9F\x93\x8A  " + item.name;   // 📊
        if (item.type == PendingAttachment::Type::PdfFile)
            return "  \xF0\x9F\x93\x84  " + item.name;   // 📄
        if (item.type == PendingAttachment::Type::DocxFile)
            return "  \xF0\x9F\x93\x84  " + item.name;   // 📄
        return "  \xF0\x9F\x93\x84  " + item.name;       // 📄
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
        if (item.type == PendingAttachment::Type::Image)
            info.kind = AttachmentInfo::Kind::Image;
        else if (item.type == PendingAttachment::Type::PdfFile)
            info.kind = AttachmentInfo::Kind::PdfFile;
        else if (item.type == PendingAttachment::Type::SpreadsheetFile)
            info.kind = AttachmentInfo::Kind::SpreadsheetFile;
        else if (item.type == PendingAttachment::Type::DocxFile)
            info.kind = AttachmentInfo::Kind::DocxFile;
        else
            info.kind = AttachmentInfo::Kind::TextFile;
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

std::string AttachmentManager::BakePdfFilesIntoMessage(const std::string& userText,
                                                       bool agentModeEnabled) const
{
    // PDFs should feel like image attachments in the composer: the user sees
    // a small chip/filename, not a wall of extracted text.  The model still
    // needs a safe route to the contents, so we quietly add tool-routing
    // instructions to the request text.  Important polish: form-fill requests
    // should route to pdf_inspect_form -> pdf_fill_form, not stop at a review
    // text draft after pdf_extract_text.
    std::string baked;
    const bool likelyFormFill = LooksLikePdfFormFillRequest(userText);

    for (const auto& item : m_pending) {
        if (item.type != PendingAttachment::Type::PdfFile) continue;

        baked += "[Attached PDF: " + item.name + "]\n";
        baked += "Safe pdf_extract_text tool argument: " + item.data + "\n";
        baked += "Safe pdf_inspect_form tool argument: " + item.data + "\n";
        baked += "Safe pdf_fill_form first-line argument: " + item.data + "\n";

        if (agentModeEnabled) {
            if (likelyFormFill) {
                baked += "This appears to be a PDF form-fill request. First call "
                         "pdf_inspect_form with exactly this args value: " + item.data + "\n";
                baked += "After pdf_inspect_form returns exact field names, call "
                         "pdf_fill_form using this PDF path on the first line and a JSON "
                         "field map on the remaining lines. Do not create an intermediate "
                         ".txt draft, do not ask the user to review before filling, and do "
                         "not stop after pdf_extract_text unless the PDF has no fillable "
                         "AcroForm fields.\n";
            } else {
                baked += "When the user's request depends on this PDF's text contents, "
                         "call the pdf_extract_text tool with exactly this args value "
                         "before answering: " + item.data + "\n";
                baked += "Do not claim you have read this PDF until its text has been extracted.\n";
            }
        } else {
            if (likelyFormFill) {
                baked += "PDF form filling is not available in this non-agent request. Tell "
                         "the user to enable Agent mode or run /pdf_inspect_form first, then "
                         "/pdf_fill_form with the exact fields.\n";
            } else {
                baked += "PDF text is not embedded in this non-agent request. If the user "
                         "needs the contents, tell them to enable Agent mode or run: "
                         "/pdf_extract_text " + item.data + "\n";
            }
        }

        baked += "\n";
    }

    if (baked.empty()) return userText;
    return baked + userText;
}

std::string AttachmentManager::BakeSpreadsheetFilesIntoMessage(const std::string& userText,
                                                               bool agentModeEnabled) const
{
    // Spreadsheets should feel like image/PDF attachments in the composer: the
    // user sees a chip/filename, not raw workbook data. The model gets a safe
    // tool route to inspect or report on the workbook only when needed.
    std::string baked;

    for (const auto& item : m_pending) {
        if (item.type != PendingAttachment::Type::SpreadsheetFile) continue;

        baked += "[Attached spreadsheet: " + item.name + "]\n";
        baked += "Safe xlsx_inspect/xlsx_report tool argument: " + item.data + "\n";
        baked += "Do not claim you have read this workbook until it has been inspected.\n";

        if (agentModeEnabled) {
            baked += "When the user's request depends on this workbook's contents, "
                     "call the xlsx_inspect tool with exactly this args value before "
                     "answering: " + item.data + "\n";
            baked += "Only call xlsx_report if the user specifically asks for a Markdown "
                     "report/artifact; xlsx_report requires approval.\n";
        } else {
            baked += "Workbook data is not embedded in this non-agent request. If the user "
                     "needs the contents, tell them to enable Agent mode or run: "
                     "/xlsx_inspect " + item.data + "\n";
        }

        baked += "\n";
    }

    if (baked.empty()) return userText;
    return baked + userText;
}

std::string AttachmentManager::BakeDocxFilesIntoMessage(const std::string& userText,
                                                        bool agentModeEnabled) const
{
    // DOCX should feel like image/PDF attachments in the composer: the user
    // sees a chip/filename, not extracted document text. The model gets a
    // safe tool route to inspect or extract from the document only when
    // needed.
    std::string baked;

    for (const auto& item : m_pending) {
        if (item.type != PendingAttachment::Type::DocxFile) continue;

        baked += "[Attached Word document: " + item.name + "]\n";
        baked += "Safe docx_extract_text/docx_inspect tool argument: " + item.data + "\n";
        baked += "Do not claim you have read this document until it has been extracted or inspected.\n";

        if (agentModeEnabled) {
            baked += "When the user's request depends on this document's contents (summary, "
                     "what it says, who it's about, what action is listed), call the "
                     "docx_extract_text tool with exactly this args value before answering: "
                     + item.data + "\n";
            baked += "Use docx_inspect instead when the user only needs structure overview "
                     "(heading list, table count, section count); docx_inspect is read-only "
                     "and does not require approval.\n";
        } else {
            baked += "Document text is not embedded in this non-agent request. If the user "
                     "needs the contents, tell them to enable Agent mode or run: "
                     "/docx_extract_text " + item.data + "\n";
        }

        baked += "\n";
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
                    // Defensive: only inject if content is still a plain
                    // string. If something upstream already converted it
                    // to an array (e.g. a future caller), bail out with a
                    // log rather than throwing from getValue<string>.
                    if (!msg->has("content") || msg->isNull("content")) {
                        if (m_logger)
                            m_logger->warning("InjectImagesIntoRequest: user message has no content; skipping injection");
                        break;
                    }
                    auto contentVar = msg->get("content");
                    if (!contentVar.isString()) {
                        if (m_logger)
                            m_logger->warning("InjectImagesIntoRequest: user content is not a string (already structured?); skipping injection");
                        break;
                    }
                    std::string textContent = contentVar.convert<std::string>();

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

        // Sanitize the original filename before letting it flow into
        // the on-disk path.  item.name comes from a user-dropped or
        // user-pasted attachment, so the threat surface is narrower
        // than a model-emitted name — but the bug class is the same
        // (path separators, traversal tokens, Windows-invalid chars,
        // reserved device names, trailing dots) and routing every
        // disk-write through the same sanitizer keeps the rules
        // uniform across producers.
        std::string safeName = path_safety::SanitizeFilename(
            item.name, "image");

        // Build base filename: "{messageIndex}_{safeName}"
        std::string baseFilename = std::to_string(messageIndex) + "_" + safeName;

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

            std::ofstream outFile(path_safety::Utf8ToWide(fullPath), std::ios::binary);
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

bool AttachmentManager::IsPdfFile(const std::string& path)
{
    wxString ext = wxFileName(wxString::FromUTF8(path)).GetExt().Lower();
    return ext == "pdf";
}

bool AttachmentManager::IsSpreadsheetFile(const std::string& path)
{
    wxString ext = wxFileName(wxString::FromUTF8(path)).GetExt().Lower();
    return ext == "xlsx";
}

bool AttachmentManager::IsDocxFile(const std::string& path)
{
    wxString ext = wxFileName(wxString::FromUTF8(path)).GetExt().Lower();
    return ext == "docx" || ext == "docm";
}

bool AttachmentManager::IsTextFile(const std::string& path)
{
    wxFileName fn(wxString::FromUTF8(path));
    wxString ext = fn.GetExt().Lower();

    // ── Extension-based match ─────────────────────────────────────
    if (ext == "txt" || ext == "md"   || ext == "json" ||
        ext == "cpp" || ext == "h"    || ext == "hpp"  ||
        ext == "py"  || ext == "js"   || ext == "ts"   ||
        ext == "css" || ext == "html" || ext == "xml"  ||
        ext == "yaml"|| ext == "yml"  || ext == "toml" ||
        ext == "csv" || ext == "log"  || ext == "ini"  ||
        ext == "cfg" || ext == "sh"   || ext == "bat"  ||
        // Additional language / framework extensions
        ext == "tsx" || ext == "jsx"  ||
        ext == "rs"  || ext == "go"   || ext == "java" ||
        ext == "kt"  || ext == "swift"|| ext == "rb"   ||
        ext == "php" || ext == "sql"  || ext == "dockerfile")
        return true;

    // ── Dotfile / extensionless cases ─────────────────────────────
    // wxFileName splits ".env" differently across platforms (sometimes
    // name=".env" ext="" , sometimes name="" ext="env"), so the
    // extension path above *may* already have matched — this is the
    // belt-and-suspenders fallback that also catches "Dockerfile" and
    // similar extensionless conventions.
    wxString full = fn.GetFullName().Lower();
    if (full == ".env"       || full == ".gitignore" ||
        full == "dockerfile")
        return true;

    return false;
}

std::string AttachmentManager::GuessMimeType(const std::string& filename)
{
    // Extract extension
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = filename.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "pdf")                return "application/pdf";
    if (ext == "xlsx")               return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";

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
    if (ext == "js"  || ext == "jsx") return "text/javascript";
    if (ext == "ts"  || ext == "tsx") return "text/typescript";

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
    std::ifstream file(path_safety::Utf8ToWide(filePath), std::ios::binary);
    if (!file.is_open()) return "";

    std::ostringstream base64Stream;
    Poco::Base64Encoder encoder(base64Stream);
    // Disable Poco's default 72-char line breaks. With no newlines in the
    // output we can return the stream contents directly, saving an O(n)
    // whitespace-strip pass on top of the already-allocated base64 buffer
    // (noticeable on multi-MB images).
    encoder.rdbuf()->setLineLength(0);
    encoder << file.rdbuf();
    encoder.close();

    return base64Stream.str();
}
