// chat_display.cpp
#include "chat_display.h"
#include "markdown_renderer.h"
#include "ascii_animation.h"
#include "theme.h"
#include "path_safety.h"
#include <wx/clipbrd.h>
#include <wx/filedlg.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

int DisplayCharCount(const std::string& s)
{
    return static_cast<int>(wxString::FromUTF8(s).length());
}

std::string ToLowerAscii(std::string s)
{
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string GetBestFileName(const PresentedFile& file)
{
    if (!file.displayName.empty()) return file.displayName;
    if (!file.diskPath.empty()) {
        wxFileName fn(wxString::FromUTF8(file.diskPath));
        return fn.GetFullName().ToStdString();
    }
    return "file";
}

std::string GetFileExtensionLower(const PresentedFile& file)
{
    wxFileName fn(wxString::FromUTF8(GetBestFileName(file)));
    return ToLowerAscii(fn.GetExt().ToStdString());
}

bool IsLikelyCodeFile(const PresentedFile& file)
{
    std::string lang = ToLowerAscii(file.language);
    if (lang == "cpp" || lang == "c++" || lang == "c" || lang == "h" ||
        lang == "hpp" || lang == "python" || lang == "py" || lang == "js" ||
        lang == "javascript" || lang == "ts" || lang == "typescript" ||
        lang == "json" || lang == "xml" || lang == "html" || lang == "css" ||
        lang == "powershell" || lang == "ps1" || lang == "bash" || lang == "sh" ||
        lang == "sql" || lang == "yaml" || lang == "yml" || lang == "markdown" || lang == "md") {
        return true;
    }

    const std::string ext = GetFileExtensionLower(file);
    static const std::vector<std::string> codeExts = {
        "c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx",
        "py", "js", "ts", "tsx", "jsx", "java", "cs", "go", "rs",
        "ps1", "psm1", "bat", "cmd", "sh", "sql", "json", "xml",
        "html", "htm", "css", "scss", "md", "yaml", "yml", "ini", "toml"
    };
    return std::find(codeExts.begin(), codeExts.end(), ext) != codeExts.end();
}

bool IsMarkdownDocumentFile(const PresentedFile& file)
{
    const std::string lang = ToLowerAscii(file.language);
    if (lang == "markdown" || lang == "md") return true;

    const std::string ext = GetFileExtensionLower(file);
    return ext == "md" || ext == "markdown";
}

std::string HumanFileSize(std::size_t bytes)
{
    std::ostringstream oss;
    if (bytes >= 1024ull * 1024ull) {
        oss.setf(std::ios::fixed);
        oss.precision(1);
        oss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    }
    else if (bytes >= 1024ull) {
        oss.setf(std::ios::fixed);
        oss.precision(bytes >= 10ull * 1024ull ? 0 : 1);
        oss << (static_cast<double>(bytes) / 1024.0) << " KB";
    }
    else {
        oss << bytes << " B";
    }
    return oss.str();
}

std::string DescribePresentedFileKind(const PresentedFile& file)
{
    const std::string ext = GetFileExtensionLower(file);
    const std::string lang = ToLowerAscii(file.language);

    if (ext == "cpp" || ext == "cc" || ext == "cxx" || lang == "cpp" || lang == "c++") return "C++ source";
    if (ext == "h" || ext == "hpp" || ext == "hh" || ext == "hxx") return "C/C++ header";
    if (ext == "py" || lang == "python" || lang == "py") return "Python script";
    if (ext == "ps1" || lang == "powershell" || lang == "ps1") return "PowerShell script";
    if (ext == "js" || lang == "javascript" || lang == "js") return "JavaScript file";
    if (ext == "ts" || ext == "tsx" || lang == "typescript" || lang == "ts") return "TypeScript file";
    if (ext == "json") return "JSON file";
    if (ext == "md" || lang == "markdown" || lang == "md") return "Markdown document";
    if (ext == "txt") return "Text document";
    if (ext == "pdf") return "PDF document";
    if (ext == "docx") return "Word document";
    if (ext == "xlsx") return "Excel workbook";
    if (ext == "csv") return "CSV spreadsheet";
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp") return "Image file";
    if (!file.language.empty()) return file.language + " file";
    if (!ext.empty()) return ext + " file";
    return "File";
}

std::string DescribePresentedFileMeta(const PresentedFile& file)
{
    std::string meta = DescribePresentedFileKind(file);
    if (IsLikelyCodeFile(file) && file.lineCount > 0) {
        meta += "  •  " + std::to_string(file.lineCount);
        meta += (file.lineCount == 1 ? " line" : " lines");
    }
    else if (file.sizeBytes > 0) {
        meta += "  •  " + HumanFileSize(file.sizeBytes);
    }
    else if (file.lineCount > 0) {
        meta += "  •  " + std::to_string(file.lineCount);
        meta += (file.lineCount == 1 ? " line" : " lines");
    }
    return meta;
}

std::string DescribePresentedFileCardLabel(const PresentedFile& file)
{
    const std::string ext = GetFileExtensionLower(file);
    const std::string lang = ToLowerAscii(file.language);

    if (ext == "docx") return "Word Document";
    if (ext == "xlsx") return "Spreadsheet";
    if (ext == "csv") return "Spreadsheet";
    if (ext == "pdf") return "PDF";
    if (ext == "md" || ext == "markdown" || lang == "markdown" || lang == "md") return "Markdown Document";
    if (ext == "txt" || lang == "text") return "Text Document";
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp") return "Image";
    if (ext == "py" || lang == "python" || lang == "py") return "Python Script";
    if (IsLikelyCodeFile(file)) return "Code File";
    return "File";
}

std::string PadRight(const std::string& s, int width)
{
    const int chars = DisplayCharCount(s);
    if (chars >= width) return s;
    return s + std::string(static_cast<size_t>(width - chars), ' ');
}

std::string RepeatText(const std::string& token, int count)
{
    std::string out;
    if (count <= 0 || token.empty()) return out;
    out.reserve(token.size() * static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) out += token;
    return out;
}

} // namespace
ChatDisplay::ChatDisplay(wxRichTextCtrl* displayCtrl)
    : m_displayCtrl(displayCtrl)
    , m_markdownRenderer(std::make_unique<MarkdownRenderer>(displayCtrl))
    , m_userColor(108, 180, 238)       // Soft blue (#6CB4EE)
    , m_assistantColor(125, 212, 160)  // Mint green (#7DD4A0)
    , m_systemColor(136, 136, 136)     // Medium gray (#888888)
    , m_thoughtColor(154, 154, 154)    // Light gray (#9A9A9A)
    , m_stdoutColor(245, 245, 245)     // Near-white (#F5F5F5) — overridden by ApplyTheme
    , m_isInThoughtBlock(false)
    , m_isFirstAssistantDelta(true)
    , m_hasRenderedAssistantContent(false)
    , m_activeAssistantColor(125, 212, 160)
{
    // Configure markdown renderer colors to match theme
    m_markdownRenderer->SetCodeColor(wxColour(232, 184, 77));      // Warm amber (#E8B84D)
    m_markdownRenderer->SetHeadingColor(wxColour(232, 232, 232));  // Near-white (#E8E8E8)

    // ── File chip callback ───────────────────────────────────────
    // Whenever a fenced code block closes, the renderer hands us a
    // PresentedFile; we drop a chip into the chat at its current
    // insertion point (which is right after the "📋 Copy" link).
    m_markdownRenderer->SetFileCallback([this](const PresentedFile& f) {
        PresentFile(f);
    });

    // ── Code block copy: click handler ────────────────────────────
    m_displayCtrl->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        long pos = 0;
        auto hit = m_displayCtrl->HitTest(event.GetPosition(), &pos);
        if (hit == wxTE_HT_ON_TEXT || hit == wxTE_HT_BEFORE) {
            // [Copy] link — existing behavior
            int blockIdx = m_markdownRenderer->HitTestCopyLink(pos);
            if (blockIdx >= 0) {
                const std::string& code = m_markdownRenderer->GetCodeBlock(static_cast<size_t>(blockIdx));
                if (wxTheClipboard->Open()) {
                    wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(code)));
                    wxTheClipboard->Close();
                }
                return;
            }
            // File chip — new behavior
            int chipIdx = HitTestFileChip(pos);
            if (chipIdx >= 0) {
                HandleFileChipClick(static_cast<size_t>(chipIdx));
                return;
            }
            // Tool block "[details]" affordance.  While a turn is still
            // appending text, don't let clicks mutate wxRichTextCtrl ranges.
            int tbIdx = HitTestToolBlockAffordance(pos);
            if (tbIdx >= 0) {
                if (!m_toolBlockInteractionEnabled) {
                    wxBell();
                    return;
                }
                HandleToolBlockAffordanceClick(static_cast<size_t>(tbIdx));
                return;
            }
        }

        // Nothing clickable was hit, so let wxRichTextCtrl keep its
        // normal text-selection/caret behavior.  Do not Skip() after
        // handling a chip/tool click, or the click may continue through
        // parent/default handlers.
        event.Skip();
    });

    // ── Code block copy: hand cursor on hover ─────────────────────
    m_displayCtrl->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        long pos = 0;
        auto hit = m_displayCtrl->HitTest(event.GetPosition(), &pos);
        bool overLink = false;
        if (hit == wxTE_HT_ON_TEXT || hit == wxTE_HT_BEFORE) {
            overLink = (m_markdownRenderer->HitTestCopyLink(pos) >= 0)
                    || (HitTestFileChip(pos) >= 0)
                    || (m_toolBlockInteractionEnabled &&
                        HitTestToolBlockAffordance(pos) >= 0);
        }
        if (overLink) {
            m_displayCtrl->SetCursor(wxCursor(wxCURSOR_HAND));
            // Don't Skip — prevents wxRichTextCtrl from resetting cursor
        } else {
            m_displayCtrl->SetCursor(wxCursor(wxCURSOR_IBEAM));
            event.Skip();
        }
    });
}

// Destructor defined here (not in header) because unique_ptr<MarkdownRenderer>
// needs the complete type, and the header only forward-declares it.
ChatDisplay::~ChatDisplay() = default;

void ChatDisplay::SetToolBlockInteractionEnabled(bool enabled)
{
    m_toolBlockInteractionEnabled = enabled;
}

// ── Thinking indicator ───────────────────────────────────────────
// Dispatches timer ticks back to the owning ChatDisplay.
void ChatDisplay::ThinkingTimer::Notify()
{
    if (m_owner) m_owner->OnThinkingTick();
}

void ChatDisplay::StartThinkingIndicator()
{
    if (m_thinkingActive || !m_displayCtrl) return;

    m_thinkingActive    = true;
    m_thinkingDotsFrame = 0;

    SetInsertionPointToEnd();
    m_thinkingDotsStartPos = m_displayCtrl->GetInsertionPoint();

    // Write the first frame: a single dot in the thought color.
    wxRichTextAttr attr;
    attr.SetTextColour(m_thoughtColor);
    attr.SetFontWeight(wxFONTWEIGHT_NORMAL);
    attr.SetFontStyle(wxFONTSTYLE_NORMAL);
    m_displayCtrl->BeginStyle(attr);
    m_displayCtrl->WriteText(".");
    m_displayCtrl->EndStyle();
    m_thinkingDotsEndPos = m_displayCtrl->GetInsertionPoint();
    EnsureVisibleAtEnd();

    if (!m_thinkingTimer)
        m_thinkingTimer = std::make_unique<ThinkingTimer>(this);
    m_thinkingTimer->Start(400);  // 400 ms per frame — smooth but not jittery
}

void ChatDisplay::OnThinkingTick()
{
    if (!m_thinkingActive || !m_displayCtrl) return;

    // Cycle through 1, 2, 3 dots.
    m_thinkingDotsFrame = (m_thinkingDotsFrame + 1) % 3;
    const int dotCount = m_thinkingDotsFrame + 1;

    // Swap old dots for new by removing the existing range and writing
    // fresh dots at the same start position.  Using Remove + WriteText
    // keeps styling/position bookkeeping simple.
    if (m_thinkingDotsEndPos > m_thinkingDotsStartPos) {
        m_displayCtrl->Remove(m_thinkingDotsStartPos, m_thinkingDotsEndPos);
    }
    m_displayCtrl->SetInsertionPoint(m_thinkingDotsStartPos);

    wxRichTextAttr attr;
    attr.SetTextColour(m_thoughtColor);
    attr.SetFontWeight(wxFONTWEIGHT_NORMAL);
    attr.SetFontStyle(wxFONTSTYLE_NORMAL);
    m_displayCtrl->BeginStyle(attr);
    m_displayCtrl->WriteText(wxString::FromUTF8(std::string(dotCount, '.')));
    m_displayCtrl->EndStyle();
    m_thinkingDotsEndPos = m_displayCtrl->GetInsertionPoint();
    // Don't scroll on every tick — dots sit at a fixed position, and
    // scrolling here would fight the user if they've scrolled up to read.
}

void ChatDisplay::ClearThinkingIndicator()
{
    if (!m_thinkingActive) return;

    if (m_thinkingTimer) m_thinkingTimer->Stop();

    if (m_displayCtrl && m_thinkingDotsEndPos > m_thinkingDotsStartPos) {
        m_displayCtrl->Remove(m_thinkingDotsStartPos, m_thinkingDotsEndPos);
        // After Remove, the end of the document is exactly where the dots
        // began.  Subsequent render calls will use SetInsertionPointToEnd()
        // themselves, so no extra positioning needed here.
    }

    m_thinkingActive       = false;
    m_thinkingDotsStartPos = -1;
    m_thinkingDotsEndPos   = -1;
    m_thinkingDotsFrame    = 0;
}

// ── File chip: render ────────────────────────────────────────────
// Writes a styled chip at the current insertion point and registers
// its character range so the click handler can hit-test it.  Doesn't
// do any positioning itself — the caller (MarkdownRenderer code-block
// callback, or a future tool handler) is responsible for where the
// cursor sits before calling.
void ChatDisplay::PresentFile(const PresentedFile& file)
{
    if (!m_displayCtrl) return;

    // Local working copy — we may fill in diskPath below.
    PresentedFile local = file;

    // ── Persistence step ──────────────────────────────────
    // Only persist when: context is set, we have inline bytes, and
    // diskPath isn't already populated (e.g. tool-produced files
    // already on disk are left alone).
    if (!m_filePersistenceDir.empty() &&
        !local.inlineContent.empty() &&
        local.diskPath.empty())
    {
        wxString dirWx = wxString::FromUTF8(m_filePersistenceDir);
        if (!wxDirExists(dirWx)) {
            wxFileName::Mkdir(dirWx, 0755, wxPATH_MKDIR_FULL);
        }
        if (wxDirExists(dirWx)) {
            std::string safeName = path_safety::SanitizeFilename(
                local.displayName, "file");

            std::string onDiskName =
                std::to_string(m_filePersistenceMsgIdx) + "_" +
                std::to_string(m_filePersistenceChipSeq) + "_" +
                safeName;
            ++m_filePersistenceChipSeq;

            std::string absPath = m_filePersistenceDir + "/" + onDiskName;

            wxFile out;
            if (out.Create(wxString::FromUTF8(absPath), /*overwrite*/ true)) {
                out.Write(local.inlineContent.data(), local.inlineContent.size());
                out.Close();
                local.diskPath = absPath;
            }
        }
    }

    const std::string title         = GetBestFileName(local);
    const std::string meta          = DescribePresentedFileMeta(local);
    const bool canSaveAs            = !local.inlineContent.empty() || !local.diskPath.empty();
    // Generated Markdown reports are document artifacts, not executable content.
    // Keep code/script files hidden from the direct [Open] action, but allow
    // .md report artifacts to open in the user's default Markdown/text viewer.
    const bool canOpen              = !local.diskPath.empty() &&
                                      (!IsLikelyCodeFile(local) || IsMarkdownDocumentFile(local));
    const bool canOpenFolder        = !local.diskPath.empty();

    std::vector<std::pair<std::string, FileAction>> actions;
    if (canOpen)       actions.push_back({"[Open]", FileAction::Open});
    if (canSaveAs)     actions.push_back({"[Save As...]", FileAction::SaveAs});
    if (canOpenFolder) actions.push_back({"[Open Folder]", FileAction::OpenFolder});

    std::string contentLine = title;
    if (!meta.empty()) contentLine += "  •  " + meta;

    std::string actionsPreview;
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i > 0) actionsPreview += "  ";
        actionsPreview += actions[i].first;
    }

    int innerWidth = std::max(44, DisplayCharCount(contentLine));
    if (!actionsPreview.empty()) innerWidth = std::max(innerWidth, DisplayCharCount(actionsPreview));

    wxFont baseFont = m_displayCtrl->GetFont();
    int baseSize = baseFont.GetPointSize();
    if (baseSize <= 0) baseSize = 14;

    wxRichTextAttr cardAttr;
    cardAttr.SetTextColour(m_stdoutColor);
    cardAttr.SetFontSize(baseSize);
    cardAttr.SetFontFaceName("Consolas");

    wxRichTextAttr actionAttr;
    actionAttr.SetTextColour(m_fileChipColor);
    actionAttr.SetFontStyle(wxFONTSTYLE_ITALIC);
    actionAttr.SetFontSize(baseSize);
    actionAttr.SetFontFaceName("Consolas");

    auto writeCardText = [&](const std::string& s) {
        m_displayCtrl->BeginStyle(cardAttr);
        m_displayCtrl->WriteText(wxString::FromUTF8(s));
        m_displayCtrl->EndStyle();
    };

    const std::string label = " " + DescribePresentedFileCardLabel(local) + " ";
    const int topFill = std::max(2, innerWidth + 2 - DisplayCharCount(label));
    writeCardText("┌" + label + RepeatText("─", topFill) + "┐\n");
    writeCardText("│ " + PadRight(contentLine, innerWidth) + " │\n");

    if (!actions.empty()) {
        writeCardText("│ " + std::string(static_cast<size_t>(innerWidth), ' ') + " │\n");

        writeCardText("│ ");
        int usedWidth = 0;
        for (size_t i = 0; i < actions.size(); ++i) {
            if (i > 0) {
                writeCardText("  ");
                usedWidth += 2;
            }

            long actionStart = m_displayCtrl->GetLastPosition();
            m_displayCtrl->BeginStyle(actionAttr);
            m_displayCtrl->WriteText(wxString::FromUTF8(actions[i].first));
            m_displayCtrl->EndStyle();
            long actionEnd = m_displayCtrl->GetLastPosition();

            FileChipRegion region;
            region.startPos = actionStart;
            region.endPos   = actionEnd;
            region.file     = local;
            region.action   = actions[i].second;
            m_fileChips.push_back(std::move(region));

            usedWidth += DisplayCharCount(actions[i].first);
        }

        if (usedWidth < innerWidth) {
            writeCardText(std::string(static_cast<size_t>(innerWidth - usedWidth), ' '));
        }
        writeCardText(" │\n");
    }

    writeCardText("└" + RepeatText("─", innerWidth + 2) + "┘");
}
void ChatDisplay::SetFilePersistenceContext(const std::string& absDir, size_t msgIdx)
{
    m_filePersistenceDir     = absDir;
    m_filePersistenceMsgIdx  = msgIdx;
    m_filePersistenceChipSeq = 0;
}

void ChatDisplay::ClearFilePersistenceContext()
{
    m_filePersistenceDir.clear();
    m_filePersistenceMsgIdx  = 0;
    m_filePersistenceChipSeq = 0;
}

int ChatDisplay::HitTestFileChip(long pos) const
{
    for (size_t i = 0; i < m_fileChips.size(); ++i) {
        if (pos >= m_fileChips[i].startPos && pos < m_fileChips[i].endPos)
            return static_cast<int>(i);
    }
    return -1;
}

void ChatDisplay::HandleFileChipClick(size_t chipIdx)
{
    if (chipIdx >= m_fileChips.size()) return;
    const FileChipRegion& region = m_fileChips[chipIdx];
    const PresentedFile& file = region.file;

    switch (region.action) {
    case FileAction::SaveAs:
    {
        std::string safeDefault = path_safety::SanitizeFilename(
            GetBestFileName(file), "file.txt");
        wxString defaultName = wxString::FromUTF8(safeDefault);

        wxFileDialog dlg(m_displayCtrl,
                         "Save file",
                         wxEmptyString,
                         defaultName,
                         "All files (*.*)|*.*",
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;

        wxString destPath = dlg.GetPath();

        if (!file.inlineContent.empty()) {
            wxFile out;
            if (out.Create(destPath, /*overwrite*/ true)) {
                out.Write(file.inlineContent.data(), file.inlineContent.size());
                out.Close();
            }
        }
        else if (!file.diskPath.empty()) {
            wxCopyFile(wxString::FromUTF8(file.diskPath), destPath, /*overwrite*/ true);
        }
        break;
    }
    case FileAction::Open:
        if (!file.diskPath.empty()) {
            if (!wxLaunchDefaultApplication(wxString::FromUTF8(file.diskPath))) {
                wxMessageBox("Unable to open the file with the default application.",
                             "Open File", wxOK | wxICON_WARNING, m_displayCtrl);
            }
        }
        break;
    case FileAction::OpenFolder:
        if (!file.diskPath.empty()) {
            const wxString path = wxString::FromUTF8(file.diskPath);
            wxString cmd = "explorer.exe /select,\"";
            cmd += path;
            cmd += "\"";
            if (wxExecute(cmd, wxEXEC_ASYNC) == 0) {
                wxFileName fn(path);
                wxLaunchDefaultApplication(fn.GetPath());
            }
        }
        break;
    }
}
int ChatDisplay::HitTestToolBlockAffordance(long pos) const
{
    for (size_t i = 0; i < m_toolBlocks.size(); ++i) {
        if (pos >= m_toolBlocks[i].affordanceStart &&
            pos <  m_toolBlocks[i].affordanceEnd)
            return static_cast<int>(i);
    }
    return -1;
}

long ChatDisplay::WriteToolBodyAtCursor(const std::string& body,
                                        const std::string& errorBody)
{
    wxFont baseFont = m_displayCtrl->GetFont();
    int baseSize = baseFont.GetPointSize();
    if (baseSize <= 0) baseSize = 14;

    // P3c-iii.1: strip trailing whitespace from each section before
    // writing, then unconditionally append a single '\n'.  This kills
    // the "blank lines before [details]" gap that PowerShell's
    // formatters and certain text files produce, without touching any
    // rich-text range or the click-mutation logic the bug-9 truce
    // depends on.  The trim is render-only — the saved-history JSON
    // still carries the original body verbatim, so reload renders the
    // same trimmed shape from the same stored content.  Both calls go
    // through the same lambda so the rule stays uniform.
    auto rtrim = [](std::string s) {
        size_t end = s.find_last_not_of(" \t\r\n\f\v");
        if (end == std::string::npos) return std::string();
        return s.substr(0, end + 1);
    };
    std::string bodyTrim    = rtrim(body);
    std::string errorTrim   = rtrim(errorBody);

    long before = m_displayCtrl->GetInsertionPoint();

    if (!bodyTrim.empty()) {
        wxRichTextAttr outAttr;
        outAttr.SetTextColour(m_stdoutColor);
        outAttr.SetFontSize(baseSize);
        outAttr.SetFontFaceName("Consolas");

        m_displayCtrl->BeginStyle(outAttr);
        m_displayCtrl->WriteText(wxString::FromUTF8(bodyTrim));
        m_displayCtrl->WriteText("\n");
        m_displayCtrl->EndStyle();
    }
    if (!errorTrim.empty()) {
        wxRichTextAttr errAttr;
        errAttr.SetTextColour(wxColour(220, 90, 90));
        errAttr.SetFontSize(baseSize);
        errAttr.SetFontFaceName("Consolas");

        m_displayCtrl->BeginStyle(errAttr);
        m_displayCtrl->WriteText(wxString::FromUTF8(errorTrim));
        m_displayCtrl->WriteText("\n");
        m_displayCtrl->EndStyle();
    }

    long after = m_displayCtrl->GetInsertionPoint();
    return after - before;
}

void ChatDisplay::ShiftOtherRegions(const ToolBlockRegion* skip,
                                    long pivot, long delta)
{
    for (auto& fc : m_fileChips) {
        if (fc.startPos >= pivot) fc.startPos += delta;
        if (fc.endPos   >= pivot) fc.endPos   += delta;
    }
    for (auto& tb : m_toolBlocks) {
        if (&tb == skip) continue;
        if (tb.bodyStart       >= pivot) tb.bodyStart       += delta;
        if (tb.bodyEnd         >= pivot) tb.bodyEnd         += delta;
        if (tb.affordanceStart >= pivot) tb.affordanceStart += delta;
        if (tb.affordanceEnd   >= pivot) tb.affordanceEnd   += delta;
    }
}

void ChatDisplay::SetAffordanceText(ToolBlockRegion& r, const wxString& newText)
{
    if (!m_displayCtrl) return;

    // "[show details]" and "[hide details]" are deliberately the same
    // display length.  That means we can swap the affordance text without
    // shifting any stored rich-text ranges.  This keeps the UX clear while
    // avoiding the older wxRichTextCtrl issue caused by changing label sizes.
    if ((r.affordanceEnd - r.affordanceStart) != static_cast<long>(newText.length())) {
        return;
    }

    wxFont baseFont = m_displayCtrl->GetFont();
    int baseSize = baseFont.GetPointSize();
    if (baseSize <= 0) baseSize = 14;

    wxRichTextAttr affAttr;
    affAttr.SetTextColour(m_fileChipColor);
    affAttr.SetFontStyle(wxFONTSTYLE_ITALIC);
    affAttr.SetFontSize(baseSize);
    affAttr.SetFontFaceName("Consolas");

    m_displayCtrl->SetInsertionPoint(r.affordanceStart);
    m_displayCtrl->Remove(r.affordanceStart, r.affordanceEnd);
    m_displayCtrl->BeginStyle(affAttr);
    m_displayCtrl->WriteText(newText);
    m_displayCtrl->EndStyle();

    // Same-length replacement: ranges remain valid, but set them explicitly
    // so the invariant is obvious and future-safe.
    r.affordanceEnd = r.affordanceStart + static_cast<long>(newText.length());
}
void ChatDisplay::HandleToolBlockAffordanceClick(size_t idx)
{
    if (idx >= m_toolBlocks.size()) return;
    ToolBlockRegion& r = m_toolBlocks[idx];

    // NOTE: We previously wrapped the toggle in Freeze()/Thaw() to
    // batch redraws on long bodies, but that caused stray characters
    // to leak from the old affordance label into the doc — a known
    // wxRichTextCtrl quirk with mutations across paragraph boundaries
    // inside a frozen control.  Letting the control redraw normally
    // between operations sidesteps it.  The flicker cost on long
    // bodies is acceptable for a click-driven (not auto) operation.

    if (r.expanded) {
        // ── Collapse ──
        long pivot = r.bodyEnd;
        long delta = -(r.bodyEnd - r.bodyStart);

        if (delta != 0) {
            m_displayCtrl->Remove(r.bodyStart, r.bodyEnd);

            // Update r's own positions manually (don't double-shift via
            // ShiftOtherRegions).  bodyStart stays put; bodyEnd collapses
            // onto bodyStart; affordance shifts left by |delta|.
            r.bodyEnd          = r.bodyStart;
            r.affordanceStart += delta;
            r.affordanceEnd   += delta;

            ShiftOtherRegions(&r, pivot, delta);
        }
        SetAffordanceText(r, "[show details]");
        r.expanded = false;
    } else {
        // ── Expand ──
        m_displayCtrl->SetInsertionPoint(r.bodyStart);
        long inserted = WriteToolBodyAtCursor(r.body, r.errorBody);
        long pivot = r.bodyStart;
        long delta = inserted;

        // Update r manually: bodyStart anchors the insertion (stays put),
        // bodyEnd grows by `inserted`, affordance shifts right by `inserted`.
        r.bodyEnd          = r.bodyStart + inserted;
        r.affordanceStart += delta;
        r.affordanceEnd   += delta;

        ShiftOtherRegions(&r, pivot, delta);

        SetAffordanceText(r, "[hide details]");
        r.expanded = true;
    }

    // Keep the affordance line visible after the toggle so the user
    // can immediately click again or see the result of their click.
    m_displayCtrl->ShowPosition(r.affordanceStart);
}

void ChatDisplay::DisplayUserMessage(const std::string& text,
                                     const std::string& target,
                                     const std::vector<std::string>& inlineImages)
{
    ClearThinkingIndicator();  // defensive: shouldn't happen mid-stream, but kill dots if so
    SetInsertionPointToEnd();

    wxRichTextAttr prefixAttr;
    prefixAttr.SetTextColour(m_userColor);
    prefixAttr.SetFontWeight(wxFONTWEIGHT_BOLD);
    m_displayCtrl->BeginStyle(prefixAttr);

    if (!target.empty()) {
        // Shorten the target name for display:
        // "pidrilkin/gemma3:Q4_K_M" → "gemma3:Q4_K_M"
        std::string shortTarget = target;
        size_t slash = shortTarget.rfind('/');
        if (slash != std::string::npos && slash + 1 < shortTarget.size())
            shortTarget = shortTarget.substr(slash + 1);

        m_displayCtrl->WriteText(wxString::FromUTF8(
            "You \xe2\x86\x92 " + shortTarget + ": "));  // → arrow
    }
    else {
        m_displayCtrl->WriteText("You: ");
    }
    m_displayCtrl->EndStyle();

    wxRichTextAttr textAttr;
    textAttr.SetTextColour(m_userColor);
    textAttr.SetFontWeight(wxFONTWEIGHT_NORMAL);
    m_displayCtrl->BeginStyle(textAttr);
    m_displayCtrl->WriteText(wxString::FromUTF8(text + "\n"));
    m_displayCtrl->EndStyle();

    // ── Inline image thumbnails ──────────────────────────────────
    for (const auto& imgPath : inlineImages) {
        wxImage img;
        if (img.LoadFile(wxString::FromUTF8(imgPath))) {
            int w = img.GetWidth(), h = img.GetHeight();
            if (w <= 0 || h <= 0) continue;  // Skip corrupt/empty images

            // Scale to thumbnail preserving aspect ratio
            if (w > kImageMaxWidth || h > kImageMaxHeight) {
                double scaleW = static_cast<double>(kImageMaxWidth) / w;
                double scaleH = static_cast<double>(kImageMaxHeight) / h;
                double scale = (scaleW < scaleH) ? scaleW : scaleH;
                int newW = std::max(1, static_cast<int>(w * scale));
                int newH = std::max(1, static_cast<int>(h * scale));
                img.Rescale(newW, newH, wxIMAGE_QUALITY_HIGH);
            }
            m_displayCtrl->WriteImage(img);
            m_displayCtrl->WriteText("\n");
        }
    }

    // Trailing spacing
    m_displayCtrl->WriteText("\n");

    EnsureVisibleAtEnd();
}

void ChatDisplay::DisplaySystemMessage(const std::string& text)
{
    // Critical for the error path: when generation fails, the error handler
    // calls DisplaySystemMessage directly without going through
    // DisplayAssistantComplete, so stray dots would otherwise remain.
    ClearThinkingIndicator();
    SetInsertionPointToEnd();

    wxRichTextAttr attr;
    attr.SetTextColour(m_systemColor);
    attr.SetFontStyle(wxFONTSTYLE_ITALIC);
    m_displayCtrl->BeginStyle(attr);
    m_displayCtrl->WriteText(wxString::FromUTF8(text + "\n\n"));
    m_displayCtrl->EndStyle();

    EnsureVisibleAtEnd();
}

// ─── Generic tool-result block ──────────────────────────────────
// One rendering path for /cmd today and /read, /ls, /grep, and the
// Phase 4 agent harness tomorrow.  Header + echo + body + errorBody
// are all independently optional; e.g. /ls has no errorBody on
// success, /read may have no echo on a repeat invocation, etc.
bool ChatDisplay::IsToolBlockFailure(const ToolBlock& block)
{
    // Any stderr output is treated as failure — covers PowerShell
    // errors, policy denials ("Command rejected by policy: ..."),
    // grep path resolution failures, etc.
    if (!block.errorBody.empty()) return true;

    // Chip-level failure signals.  Catches the cases where exit was
    // non-zero but stderr happened to be empty (e.g. a script that
    // calls `exit 1` silently), plus the synthesized chips from
    // tool_dispatcher's policy/startup error paths.
    for (const auto& chip : block.statusChips) {
        if (chip == "blocked")   return true;
        if (chip == "cancelled") return true;
        if (chip == "timed out") return true;
        if (chip == "error")     return true;
        // "exit N" — failure when N != 0.  Substring check guards
        // against any future chip that happens to start with "exit".
        if (chip.size() > 5 &&
            chip.compare(0, 5, "exit ") == 0 &&
            chip != "exit 0")
            return true;
    }
    return false;
}

void ChatDisplay::DisplayToolBlock(const ToolBlock& block, bool startExpanded)
{
    ClearThinkingIndicator();
    SetInsertionPointToEnd();

    wxFont baseFont = m_displayCtrl->GetFont();
    int baseSize = baseFont.GetPointSize();
    if (baseSize <= 0) baseSize = 14;

    // ── Header: "<icon> <toolName>  ·  <chip>  ·  <chip> ..."
    // Bold, system color, base font.  Chips are ·-separated using
    // U+00B7 MIDDLE DOT so they visually float above the block.
    if (!block.toolName.empty() || !block.iconUtf8.empty()) {
        std::ostringstream header;
        if (!block.iconUtf8.empty())
            header << block.iconUtf8 << " ";
        header << block.toolName;
        for (const auto& chip : block.statusChips)
            header << "  \xC2\xB7  " << chip;   // "  ·  "

        wxRichTextAttr headerAttr;
        headerAttr.SetTextColour(m_systemColor);
        headerAttr.SetFontWeight(wxFONTWEIGHT_BOLD);
        headerAttr.SetFontSize(baseSize);
        if (!baseFont.GetFaceName().empty())
            headerAttr.SetFontFaceName(baseFont.GetFaceName());

        m_displayCtrl->BeginStyle(headerAttr);
        m_displayCtrl->WriteText(wxString::FromUTF8(header.str() + "\n"));
        m_displayCtrl->EndStyle();
    }

    // ── Command echo: "> <commandEcho>" — monospace, system color.
    if (!block.commandEcho.empty()) {
        wxRichTextAttr cmdAttr;
        cmdAttr.SetTextColour(m_systemColor);
        cmdAttr.SetFontStyle(wxFONTSTYLE_NORMAL);
        cmdAttr.SetFontSize(baseSize);
        cmdAttr.SetFontFaceName("Consolas");

        m_displayCtrl->BeginStyle(cmdAttr);
        m_displayCtrl->WriteText(wxString::FromUTF8("> " + block.commandEcho + "\n"));
        m_displayCtrl->EndStyle();
    }

    // ── Presented files — always visible, even when details are
    // collapsed.  This is the first artifact-style affordance for
    // tool-created files: /write can create a real file on disk and
    // attach it here as a clickable chip.  Clicking the chip opens
    // the existing Save As path in PresentFile().
    for (const auto& file : block.presentedFiles) {
        PresentFile(file);
        m_displayCtrl->WriteText("\n");
    }

    // ── Body region — written via helper so we can re-render it on
    // expand.  bodyStart is captured BEFORE the write; bodyEnd is
    // bodyStart + chars written.  When body and errorBody are both
    // empty this is a zero-length region and we skip the affordance
    // entirely (nothing to hide/show).
    //
    // Initial expanded state is decided by caller intent (startExpanded)
    // OR by automatic failure classification.  User-typed slash commands
    // pass startExpanded=true so they always show output; agent paths
    // pass the default (false), letting failures auto-expand while
    // successes start collapsed.
    bool expanded = startExpanded || IsToolBlockFailure(block);

    long bodyStart = m_displayCtrl->GetInsertionPoint();
    long bodyChars = expanded
        ? WriteToolBodyAtCursor(block.body, block.errorBody)
        : 0;
    long bodyEnd   = bodyStart + bodyChars;

    // ── Affordance: "[hide details]" when expanded, "[show details]"
    // when collapsed — the label invites the OPPOSITE action.  Same
    // soft-blue italic monospace as file chips so all click targets
    // read consistently.  Trailing \n keeps the affordance on its own
    // line BUT is captured outside the click range so only the
    // bracketed text is hittable.
    if (!block.body.empty() || !block.errorBody.empty()) {
        long affStart = m_displayCtrl->GetInsertionPoint();

        wxRichTextAttr affAttr;
        affAttr.SetTextColour(m_fileChipColor);
        affAttr.SetFontStyle(wxFONTSTYLE_ITALIC);
        affAttr.SetFontSize(baseSize);
        affAttr.SetFontFaceName("Consolas");

        m_displayCtrl->BeginStyle(affAttr);
        m_displayCtrl->WriteText(expanded ? "[hide details]" : "[show details]");
        m_displayCtrl->EndStyle();

        long affEnd = m_displayCtrl->GetInsertionPoint();

        ToolBlockRegion region;
        region.affordanceStart = affStart;
        region.affordanceEnd   = affEnd;
        region.bodyStart       = bodyStart;
        region.bodyEnd         = bodyEnd;
        region.body            = block.body;
        region.errorBody       = block.errorBody;
        region.expanded        = expanded;
        m_toolBlocks.push_back(region);

        m_displayCtrl->WriteText("\n");
    }

    // Trailing blank line for separation.
    m_displayCtrl->WriteText("\n");

    EnsureVisibleAtEnd();
}

void ChatDisplay::DisplayAssistantPrefix(const std::string& modelName)
{
    DisplayAssistantPrefix(modelName, m_assistantColor);
}

void ChatDisplay::DisplayAssistantPrefix(const std::string& modelName, const wxColour& accentColor)
{
    SetInsertionPointToEnd();
    m_currentAssistantStartPos = m_displayCtrl->GetInsertionPoint();

    // Reset state for the new message
    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();
    m_activeAssistantColor = accentColor;
    m_markdownRenderer->Reset();

    wxFont baseFont = m_displayCtrl->GetFont();
    int baseSize = baseFont.GetPointSize();
    if (baseSize <= 0) baseSize = 14;

    wxRichTextAttr prefixAttr;
    prefixAttr.SetTextColour(accentColor);
    prefixAttr.SetFontWeight(wxFONTWEIGHT_BOLD);
    prefixAttr.SetFontStyle(wxFONTSTYLE_NORMAL);
    prefixAttr.SetFontSize(baseSize);

    if (!baseFont.GetFaceName().empty()) {
        prefixAttr.SetFontFaceName(baseFont.GetFaceName());
    }

    m_displayCtrl->BeginStyle(prefixAttr);
    m_displayCtrl->WriteText(wxString::FromUTF8(modelName + ": "));
    m_displayCtrl->EndStyle();

    // Kick off the animated dots.  They'll be cleared by the first delta
    // that carries visible characters (see DisplayAssistantDelta).
    StartThinkingIndicator();
}

void ChatDisplay::DisplayAssistantDelta(const std::string& delta)
{
    SetInsertionPointToEnd();
    std::string remainingDelta = delta;

    const auto trimLeadingWhitespace = [](std::string& text)
        {
            size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                text.clear();
            }
            else if (first > 0) {
                text.erase(0, first);
            }
        };

    const auto hasVisibleChars = [](const std::string& text) -> bool
        {
            return text.find_first_not_of(" \t\r\n") != std::string::npos;
        };

    // Markers for different reasoning models
    const std::string thought_start_marker = "<think>";
    const std::string thought_end_marker = "</think>";

    // ── Probe phase: accumulate first few bytes to detect <think> ──
    // The tag is 7 characters and may arrive split across deltas
    // (e.g. "<thi" + "nk>\n...").  We buffer until we can decide.
    if (m_isFirstAssistantDelta) {
        m_thinkProbeBuffer += remainingDelta;

        // Check if buffer starts with <think>
        if (m_thinkProbeBuffer.size() >= thought_start_marker.size()) {
            if (m_thinkProbeBuffer.compare(0, thought_start_marker.size(),
                                           thought_start_marker) == 0) {
                // Confirmed: thinking model response
                m_isInThoughtBlock = true;
                m_isFirstAssistantDelta = false;
                remainingDelta = m_thinkProbeBuffer.substr(thought_start_marker.size());
                m_thinkProbeBuffer.clear();
                // Fall through to process remainingDelta as thought content
            }
            else {
                // Not a <think> tag — flush entire buffer as normal content
                m_isFirstAssistantDelta = false;
                remainingDelta = m_thinkProbeBuffer;
                m_thinkProbeBuffer.clear();
                // Fall through to process remainingDelta as normal content
            }
        }
        else if (thought_start_marker.compare(0, m_thinkProbeBuffer.size(),
                                              m_thinkProbeBuffer) == 0) {
            // Partial prefix match (e.g. "<thi") — keep buffering, don't render yet
            return;
        }
        else {
            // Buffer can't possibly match <think> — flush as normal content
            m_isFirstAssistantDelta = false;
            remainingDelta = m_thinkProbeBuffer;
            m_thinkProbeBuffer.clear();
            // Fall through to process remainingDelta as normal content
        }
    }

    if (m_isInThoughtBlock) {
        // Prepend any buffered tail from the previous delta so that a
        // </think> tag split across two deltas is detected correctly.
        std::string combined = m_thinkEndProbeBuffer + remainingDelta;
        m_thinkEndProbeBuffer.clear();

        size_t end_pos = combined.find(thought_end_marker);
        if (end_pos != std::string::npos) {
            // End marker found. Part is thought, rest is answer.
            std::string thought_part = combined.substr(0, end_pos);
            std::string answer_part = combined.substr(end_pos + thought_end_marker.length());

            // If nothing visible has been rendered yet, strip leading blank lines/spaces.
            if (!m_hasRenderedAssistantContent) {
                trimLeadingWhitespace(thought_part);
            }

            // Only render thought text if it actually contains visible characters.
            if (hasVisibleChars(thought_part)) {
                ClearThinkingIndicator();
                AppendFormattedText(thought_part, m_thoughtColor);
                m_hasRenderedAssistantContent = true;
            }

            m_isInThoughtBlock = false;

            // Trim leading blank space before the first visible answer text.
            if (!m_hasRenderedAssistantContent) {
                trimLeadingWhitespace(answer_part);
            }

            if (!answer_part.empty()) {
                ClearThinkingIndicator();
                // ProcessDelta scrolls internally — no extra scroll needed
                m_markdownRenderer->ProcessDelta(answer_part, m_activeAssistantColor);
                if (hasVisibleChars(answer_part)) {
                    m_hasRenderedAssistantContent = true;
                }
            }
            else {
                // Only thought text was rendered (via AppendFormattedText
                // which doesn't scroll) — scroll now.
                EnsureVisibleAtEnd();
            }
        }
        else {
            // No end marker yet. Hold back the last 7 chars (length of
            // "</think>" minus 1) so a split tag is caught on the next delta.
            const size_t kHoldBack = thought_end_marker.size() - 1;  // 7
            size_t safeLen = (combined.size() > kHoldBack)
                           ? combined.size() - kHoldBack : 0;
            std::string safeToRender = combined.substr(0, safeLen);
            m_thinkEndProbeBuffer = combined.substr(safeLen);

            if (!m_hasRenderedAssistantContent) {
                trimLeadingWhitespace(safeToRender);
            }

            if (hasVisibleChars(safeToRender)) {
                ClearThinkingIndicator();
                AppendFormattedText(safeToRender, m_thoughtColor);
                m_hasRenderedAssistantContent = true;
            }
            // AppendFormattedText doesn't scroll — do it here
            EnsureVisibleAtEnd();
        }
    }
    else {
        // Normal answer text — trim leading blank space only at the very start
        if (!m_hasRenderedAssistantContent) {
            trimLeadingWhitespace(remainingDelta);
        }

        if (!remainingDelta.empty()) {
            ClearThinkingIndicator();
            // ProcessDelta scrolls internally
            m_markdownRenderer->ProcessDelta(remainingDelta, m_activeAssistantColor);
            if (hasVisibleChars(remainingDelta)) {
                m_hasRenderedAssistantContent = true;
            }
        }
    }
}

void ChatDisplay::DisplayAssistantComplete()
{
    // Stop the dots immediately — either we're about to render buffered
    // content, or the message ended with nothing visible at all.
    ClearThinkingIndicator();

    // If message ended while still probing for <think>, flush the
    // buffered bytes as normal content (it wasn't a thinking response).
    if (m_isFirstAssistantDelta && !m_thinkProbeBuffer.empty()) {
        m_isFirstAssistantDelta = false;
        m_markdownRenderer->ProcessDelta(m_thinkProbeBuffer, m_activeAssistantColor);
        m_thinkProbeBuffer.clear();
    }

    // Flush any remaining buffered text in the markdown renderer
    if (m_isInThoughtBlock) {
        // Message ended while still in thought block (unusual but handle it).
        // Flush any held-back tail from the split-boundary probe.
        if (!m_thinkEndProbeBuffer.empty()) {
            AppendFormattedText(m_thinkEndProbeBuffer, m_thoughtColor);
            m_thinkEndProbeBuffer.clear();
        }
        AppendFormattedText("\n\n", m_thoughtColor);
    }
    else {
        m_markdownRenderer->Flush(m_activeAssistantColor);
        AppendFormattedText("\n\n", m_activeAssistantColor);
    }

    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();
    m_currentAssistantStartPos = -1;
}

void ChatDisplay::CancelPendingAssistantDisplay()
{
    ClearThinkingIndicator();

    if (m_displayCtrl && m_currentAssistantStartPos >= 0) {
        SetInsertionPointToEnd();
        long end = m_displayCtrl->GetInsertionPoint();
        if (end > m_currentAssistantStartPos) {
            m_displayCtrl->Remove(m_currentAssistantStartPos, end);
        }
    }

    if (m_markdownRenderer) {
        m_markdownRenderer->Reset();
    }

    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();
    m_currentAssistantStartPos = -1;

    EnsureVisibleAtEnd();

}

void ChatDisplay::DisplayAssistantMessage(const std::string& modelName,
    const std::string& content,
    const wxColour& accentColor)
{
    // Non-streaming replay path: render the full message immediately.
    // Clear first so the dots that DisplayAssistantPrefix would otherwise
    // start don't linger — there are no deltas coming to clear them.
    ClearThinkingIndicator();
    DisplayAssistantPrefix(modelName, accentColor);
    ClearThinkingIndicator();  // kill the dots the prefix just started

    if (!content.empty()) {
        // Strip leading whitespace/newlines so the first paragraph renders
        // flush with the prefix — matches the trim that DisplayAssistantDelta
        // performs on the streaming path.
        std::string trimmed = content;
        size_t first = trimmed.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            trimmed.clear();
        } else if (first > 0) {
            trimmed.erase(0, first);
        }

        if (!trimmed.empty()) {
            // ── Handle <think>...</think> blocks on replay ─────────────
            // The streaming path uses a state machine to detect and dimly
            // render thought blocks. The replay (non-streaming) path must
            // do the same, otherwise raw <think> tags appear literally.
            const std::string kThinkStart = "<think>";
            const std::string kThinkEnd   = "</think>";

            std::string toProcess = trimmed;
            bool renderedAny = false;

            while (!toProcess.empty()) {
                size_t ts = toProcess.find(kThinkStart);
                if (ts == std::string::npos) {
                    // No (more) think block — render remainder as answer
                    m_markdownRenderer->ProcessDelta(toProcess, accentColor);
                    renderedAny = true;
                    toProcess.clear();
                } else {
                    // Render any answer text that precedes the think block
                    if (ts > 0) {
                        m_markdownRenderer->ProcessDelta(toProcess.substr(0, ts), accentColor);
                        renderedAny = true;
                    }

                    size_t contentStart = ts + kThinkStart.size();
                    size_t te = toProcess.find(kThinkEnd, contentStart);

                    if (te == std::string::npos) {
                        // Malformed: no closing tag — treat rest as thought text
                        std::string thought = toProcess.substr(contentStart);
                        // strip leading whitespace
                        size_t f = thought.find_first_not_of(" \t\r\n");
                        if (f != std::string::npos && f > 0) thought.erase(0, f);
                        if (!thought.empty()) {
                            AppendFormattedText(thought, m_thoughtColor);
                            renderedAny = true;
                        }
                        toProcess.clear();
                    } else {
                        // Well-formed block — render thought dimly
                        std::string thought = toProcess.substr(contentStart, te - contentStart);
                        size_t f = thought.find_first_not_of(" \t\r\n");
                        if (f != std::string::npos && f > 0) thought.erase(0, f);
                        if (!thought.empty()) {
                            AppendFormattedText(thought, m_thoughtColor);
                            renderedAny = true;
                        }
                        // Continue with whatever follows </think>
                        toProcess = toProcess.substr(te + kThinkEnd.size());
                        // strip leading whitespace before the answer
                        size_t fa = toProcess.find_first_not_of(" \t\r\n");
                        if (fa == std::string::npos)
                            toProcess.clear();
                        else if (fa > 0)
                            toProcess.erase(0, fa);
                    }
                }
            }

            m_markdownRenderer->Flush(accentColor);
            (void)renderedAny;
        }
    }

    AppendFormattedText("\n\n", accentColor);

    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();
    m_currentAssistantStartPos = -1;

    EnsureVisibleAtEnd();
}

void ChatDisplay::Clear()
{
    // Must stop the timer before wiping the document, otherwise the next
    // tick will try to Remove() a range that no longer exists.
    ClearThinkingIndicator();

    // Char-position ranges in m_fileChips become invalid once the document
    // is cleared — drop them so the click handler can't hit stale regions.
    m_fileChips.clear();
    m_toolBlocks.clear();

    // Drop the persistence context too — the new conversation's context
    // (if any) will be set by whoever drives the next stream.
    ClearFilePersistenceContext();

    if (m_displayCtrl) {
        m_displayCtrl->Clear();
    }
    if (m_markdownRenderer) {
        m_markdownRenderer->Reset();
        m_markdownRenderer->ClearCodeBlocks();
    }
}

void ChatDisplay::ScrollToBottom()
{
    EnsureVisibleAtEnd();
}

void ChatDisplay::SetUserColor(const wxColour& color)
{
    m_userColor = color;
}

void ChatDisplay::SetAssistantColor(const wxColour& color)
{
    m_assistantColor = color;
}

void ChatDisplay::SetSystemColor(const wxColour& color)
{
    m_systemColor = color;
}

void ChatDisplay::SetThoughtColor(const wxColour& color)
{
    m_thoughtColor = color;
}

void ChatDisplay::SetFont(const wxFont& font)
{
    if (m_displayCtrl) {
        m_displayCtrl->SetFont(font);
    }
}

void ChatDisplay::ApplyTheme(const ThemeData& theme)
{
    m_userColor = theme.chatUser;
    m_assistantColor = theme.chatAssistant;
    m_systemColor = theme.chatSystem;
    m_thoughtColor = theme.chatThought;
    m_stdoutColor = theme.textPrimary;

    if (m_markdownRenderer) {
        m_markdownRenderer->SetCodeColor(theme.mdCode);
        m_markdownRenderer->SetHeadingColor(theme.mdHeading);
        m_markdownRenderer->SetCodeLabelColor(theme.mdCodeLabel);
        m_markdownRenderer->SetHorizontalRuleColor(theme.mdHorizontalRule);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  ASCII Animation rendering
// ═══════════════════════════════════════════════════════════════════

void ChatDisplay::BeginAnimationFrame()
{
    m_displayCtrl->Freeze();

    if (m_animActive && m_animStartPos >= 0) {
        // Delete previous frame text
        long end = m_displayCtrl->GetLastPosition();
        if (end > m_animStartPos)
            m_displayCtrl->Remove(m_animStartPos, end);
    }

    m_displayCtrl->SetInsertionPointEnd();
    m_animStartPos = m_displayCtrl->GetInsertionPoint();
    m_animActive = true;
}

void ChatDisplay::WriteAnimationLine(const std::vector<ColoredChar>& line)
{
    // Build a monospace font matching the base size
    wxFont baseFont = m_displayCtrl->GetFont();
    int sz = baseFont.GetPointSize();
    if (sz <= 0) sz = 11;
    wxFont monoFont(sz - 1, wxFONTFAMILY_TELETYPE,
                    wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL,
                    false, "Consolas");

    // Batch consecutive chars of the same color into one WriteText call
    // to avoid per-character style overhead (huge perf win on wxRichTextCtrl)
    size_t i = 0;
    while (i < line.size()) {
        wxColour color = line[i].color;
        std::string run;
        while (i < line.size() && line[i].color == color) {
            run += line[i].ch;
            ++i;
        }

        wxRichTextAttr attr;
        attr.SetTextColour(color);
        attr.SetFontWeight(wxFONTWEIGHT_NORMAL);
        attr.SetFontStyle(wxFONTSTYLE_NORMAL);
        attr.SetFont(monoFont);
        m_displayCtrl->BeginStyle(attr);
        m_displayCtrl->WriteText(wxString::FromUTF8(run));
        m_displayCtrl->EndStyle();
    }

    // Newline between rows (plain, no special style needed)
    m_displayCtrl->WriteText("\n");
}

void ChatDisplay::EndAnimationFrame()
{
    m_displayCtrl->Thaw();
    EnsureVisibleAtEnd();
}

void ChatDisplay::ClearAnimation()
{
    m_animActive   = false;
    m_animStartPos = -1;
}

// Private helper methods

void ChatDisplay::AppendFormattedText(const std::string& text, const wxColour& color,
    bool bold, bool italic)
{
    if (text.empty()) return;

    SetInsertionPointToEnd();

    wxRichTextAttr attr;
    attr.SetTextColour(color);
    if (bold) attr.SetFontWeight(wxFONTWEIGHT_BOLD);
    if (italic) attr.SetFontStyle(wxFONTSTYLE_ITALIC);
    if (!bold) attr.SetFontWeight(wxFONTWEIGHT_NORMAL);

    m_displayCtrl->BeginStyle(attr);
    m_displayCtrl->WriteText(wxString::FromUTF8(text));
    m_displayCtrl->EndStyle();
}

void ChatDisplay::SetInsertionPointToEnd()
{
    if (m_displayCtrl) {
        m_displayCtrl->SetInsertionPointEnd();
    }
}

void ChatDisplay::EnsureVisibleAtEnd()
{
    if (m_displayCtrl) {
        m_displayCtrl->ShowPosition(m_displayCtrl->GetLastPosition());
    }
}
