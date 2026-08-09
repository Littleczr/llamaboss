// chat_display.cpp
#include "chat_display.h"
#include "markdown_renderer.h"
#include "ascii_animation.h"
#include "theme.h"
#include "path_safety.h"
#include <wx/clipbrd.h>
#include <wx/caret.h>
#include <wx/filedlg.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/menu.h>       // image thumbnail context menu
#include <wx/statbmp.h>    // image viewer lightbox
#include <wx/dialog.h>     // image viewer lightbox
#include "lb_modal_scrim.h" // LbShowModalWithScrim for the image viewer
#include <wx/utils.h>
#include <wx/stdpaths.h>   // thumbnail cache location
#include <wx/log.h>        // wxLogNull around cache reads
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

#ifdef __WXMSW__
#include <windows.h>       // thumbnail cache prune enumeration
#endif

namespace {

int DisplayCharCount(const std::string& s)
{
    return static_cast<int>(wxString::FromUTF8(s).length());
}

void HideRichTextCaret(wxRichTextCtrl* ctrl)
{
    if (!ctrl) return;

    // wxRichTextCtrl is used here as a read-only transcript.  Programmatic
    // writes and mouse clicks can still move the insertion point, which makes
    // wxWidgets show a blinking caret.  Hiding the wxCaret keeps selection and
    // clickable ranges working without making the transcript look editable.
    if (wxCaret* caret = ctrl->GetCaret()) {
        caret->Hide();
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Thumbnail cache
// ═══════════════════════════════════════════════════════════════════
// Opening a conversation containing images used to decode every original
// file at full resolution and box-filter it down to the display cap, on
// the UI thread, inside the frozen replay batch — tens of ms per image
// for the 1–2K px files image models emit, paid on EVERY open.  The
// first render still pays that cost once; the scaled result is then
// saved as a small PNG under %LOCALAPPDATA%\LlamaBoss\thumbcache and
// every later replay loads the thumbnail instead.
//
// Cache key: FNV-1a hash of (absolute path | mtime ms | size | target
// box).  Any change to the original file or to the display cap yields a
// different key, so a stale thumbnail can never be shown — the old
// entry is simply orphaned and eventually removed by the size-cap prune.
//
// Failure policy: every cache miss, unreadable cache file, or failed
// cache write silently falls back to the original decode-and-rescale
// path.  The cache can only ever make things faster, never wrong.

unsigned long long ThumbKeyFnv1a64(const std::string& s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string ThumbCacheDir()
{
    // %LOCALAPPDATA%\LlamaBoss\thumbcache — same root the logger,
    // config, and secrets already use (SetAppName in MyApp::OnInit
    // makes GetUserLocalDataDir resolve there regardless of exe name).
    wxString dir = wxStandardPaths::Get().GetUserLocalDataDir();
    dir += wxFileName::GetPathSeparator();
    dir += "thumbcache";
    return std::string(dir.ToUTF8().data());
}

#ifdef __WXMSW__
// Size-cap prune: once per session, and only when we're about to WRITE
// a new entry (reads never pay for this).  Thumbnails are ~50–150KB
// PNGs, so the 1500-file cap bounds the cache to roughly 100–200MB
// worst case; pruning down to 750 keeps the sweep rare.  Enumeration
// uses FindFirstFileExW so no per-file handles are opened.
void PruneThumbCacheIfOversized(const std::string& dir)
{
    constexpr size_t kMaxFiles = 1500;
    constexpr size_t kKeep     = 750;

    const std::wstring wdir = path_safety::Utf8ToWide(dir);
    if (wdir.empty()) return;

    struct Item {
        unsigned long long mtime;   // FILETIME ticks — only order matters
        std::wstring       wpath;
    };
    std::vector<Item> items;

    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileExW((wdir + L"\\*.png").c_str(),
                                  FindExInfoBasic, &fd,
                                  FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER uli;
        uli.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
        uli.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        items.push_back({ uli.QuadPart, wdir + L"\\" + fd.cFileName });
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);

    if (items.size() <= kMaxFiles) return;

    std::sort(items.begin(), items.end(),
        [](const Item& a, const Item& b) { return a.mtime < b.mtime; });

    const size_t deleteCount = items.size() - kKeep;
    for (size_t i = 0; i < deleteCount; ++i)
        ::DeleteFileW(items[i].wpath.c_str());
}
#endif

// Loads |imgPath| as a display-ready thumbnail into |out|: already
// scaled to fit maxW × maxH when the original was larger, unscaled
// otherwise.  Returns false only when the original can't be read
// (matching the old skip-corrupt-images behaviour at both call sites).
bool LoadImageThumbnailCached(const std::string& imgPath,
                              int maxW, int maxH, wxImage& out)
{
    // ── Stat the original for the cache key ──────────────────────
    // A failed stat (file vanished, odd path) just disables the cache
    // for this image; the direct path below still runs.  Unlike the
    // sidebar's hundreds-of-files scan, this is a handful of images
    // per conversation, so portable wxFileName here is fine.
    long long          mtimeMs  = 0;
    unsigned long long fileSize = 0;
    bool               haveStat = false;
    {
        wxFileName fn(wxString::FromUTF8(imgPath));
        wxDateTime mod;
        if (fn.FileExists() && fn.GetTimes(nullptr, &mod, nullptr) &&
            mod.IsValid()) {
            mtimeMs  = mod.GetValue().GetValue();
            fileSize = fn.GetSize().GetValue();
            haveStat = true;
        }
    }

    std::string cachePath;
    if (haveStat) {
        const std::string key =
            imgPath + "|" + std::to_string(mtimeMs) +
            "|" + std::to_string(fileSize) +
            "|" + std::to_string(maxW) + "x" + std::to_string(maxH);
        char name[32];
        snprintf(name, sizeof(name), "%016llx", ThumbKeyFnv1a64(key));
        cachePath = ThumbCacheDir();
        cachePath += wxFileName::GetPathSeparator();
        cachePath += name;
        cachePath += ".png";

        if (wxFileExists(wxString::FromUTF8(cachePath))) {
            // Suppress wx error popups for a corrupt/truncated cache
            // entry — that's a regenerate, not a user-facing error.
            wxLogNull noLog;
            if (out.LoadFile(wxString::FromUTF8(cachePath)) &&
                out.GetWidth() > 0 && out.GetHeight() > 0) {
                return true;
            }
            out = wxImage();   // fall through and regenerate
        }
    }

    // ── Direct path (first sighting, stat failure, or bad cache) ─
    if (!out.LoadFile(wxString::FromUTF8(imgPath))) return false;

    const int w = out.GetWidth(), h = out.GetHeight();
    if (w <= 0 || h <= 0) return false;

    if (w > maxW || h > maxH) {
        const double scaleW = static_cast<double>(maxW) / w;
        const double scaleH = static_cast<double>(maxH) / h;
        const double scale  = (scaleW < scaleH) ? scaleW : scaleH;
        const int newW = std::max(1, static_cast<int>(w * scale));
        const int newH = std::max(1, static_cast<int>(h * scale));
        out.Rescale(newW, newH, wxIMAGE_QUALITY_HIGH);

        // Only rescaled images are worth caching — small originals
        // decode quickly anyway, and caching them would just duplicate
        // their bytes on disk.  Best-effort: a failed mkdir or save
        // means the next open pays the rescale again, nothing worse.
        if (!cachePath.empty()) {
            const std::string dir = ThumbCacheDir();
            const wxString wxDirPath = wxString::FromUTF8(dir);
            if (wxDirExists(wxDirPath) ||
                wxFileName::Mkdir(wxDirPath, wxS_DIR_DEFAULT,
                                  wxPATH_MKDIR_FULL)) {
#ifdef __WXMSW__
                static bool s_prunedThisSession = false;
                if (!s_prunedThisSession) {
                    s_prunedThisSession = true;
                    PruneThumbCacheIfOversized(dir);
                }
#endif
                wxLogNull noLog;
                out.SaveFile(wxString::FromUTF8(cachePath),
                             wxBITMAP_TYPE_PNG);
            }
        }
    }

    return true;
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

// Returns the effective point size of the control's font, falling back
// to a sane default when GetPointSize() returns 0 or negative (which
// can happen on a freshly-constructed wxFont before the host frame
// installs the chat font).  Called at the top of every renderer that
// builds wxRichTextAttr instances off the base font size.
int ResolveBaseFontSize(wxRichTextCtrl* ctrl, int fallback = 14)
{
    if (!ctrl) return fallback;
    const int sz = ctrl->GetFont().GetPointSize();
    return (sz > 0) ? sz : fallback;
}

// Builds a Consolas-faced text attribute at the given color, size, and
// (optional) style.  Style is set EXPLICITLY -- the original ad-hoc
// attrs sometimes omitted SetFontStyle(), which means "inherit from
// surrounding style".  In the call sites this helper replaces, every
// write happens in a fresh BeginStyle scope where no italic is
// inherited, so explicit NORMAL produces byte-identical output while
// being defensive against future code that calls these writes inside
// an outer italic scope.
wxRichTextAttr MakeMonoAttr(const wxColour& color, int size,
                            wxFontStyle style = wxFONTSTYLE_NORMAL)
{
    wxRichTextAttr a;
    a.SetTextColour(color);
    a.SetFontStyle(style);
    a.SetFontSize(size);
    a.SetFontFaceName("Consolas");
    return a;
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

    // ── Sticky autoscroll: follow-mode tracking ───────────────────
    // The renderer consults follow mode before its per-delta scrolls,
    // and any user scroll input (wheel or scrollbar) re-evaluates the
    // flag AFTER the scroll has been applied.  The check is deferred
    // with CallAfter because this handler runs before the control's
    // default scroll processing; the weak token makes a late-firing
    // check on a destroyed ChatDisplay a silent no-op.
    m_markdownRenderer->SetAutoScrollPredicate(
        [this]() { return m_followStream; });

    {
        std::weak_ptr<int> alive = m_followCheckAlive;
        auto scheduleFollowCheck = [this, alive](wxEvent& event) {
            event.Skip();   // let the control actually scroll first
            if (!m_displayCtrl) return;
            m_displayCtrl->CallAfter([this, alive]() {
                if (alive.expired()) return;
                UpdateFollowFromScrollPosition();
            });
        };
        m_displayCtrl->Bind(wxEVT_MOUSEWHEEL, scheduleFollowCheck);
        const wxEventTypeTag<wxScrollWinEvent> kScrollTypes[] = {
            wxEVT_SCROLLWIN_TOP,        wxEVT_SCROLLWIN_BOTTOM,
            wxEVT_SCROLLWIN_LINEUP,     wxEVT_SCROLLWIN_LINEDOWN,
            wxEVT_SCROLLWIN_PAGEUP,     wxEVT_SCROLLWIN_PAGEDOWN,
            wxEVT_SCROLLWIN_THUMBTRACK, wxEVT_SCROLLWIN_THUMBRELEASE,
        };
        for (const auto& type : kScrollTypes)
            m_displayCtrl->Bind(type, scheduleFollowCheck);
    }

    // ── Image thumbnails: right-click menu ────────────────────────
    // Save/reveal for any thumbnail tagged with its source path (see
    // TagLastWrittenImage).  Non-image positions Skip() through so
    // the control's default context behavior is untouched.
    m_displayCtrl->Bind(wxEVT_CONTEXT_MENU,
                        &ChatDisplay::OnImageContextMenu, this);

    // ── Code block copy: click handler ────────────────────────────
    m_displayCtrl->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        // wxRichTextCtrl captures the mouse on left-down for text selection.
        // If we consume the matching left-up for our inline affordances
        // (Copy/file chips/details), wxWidgets can keep this control in the
        // capture stack.  A rapid second click then trips the debug assert:
        // "Recapturing the mouse in the same window?".
        //
        // We intentionally still do not Skip() handled affordance clicks
        // because some handlers mutate rich-text ranges.  Instead, release
        // this control's capture before returning.
        auto releaseDisplayCapture = [this]() {
            if (m_displayCtrl && m_displayCtrl->HasCapture()) {
                m_displayCtrl->ReleaseMouse();
            }
        };

        long pos = 0;
        auto hit = m_displayCtrl->HitTest(event.GetPosition(), &pos);
        if (hit == wxTE_HT_ON_TEXT || hit == wxTE_HT_BEFORE) {
            // [Copy] link — existing behavior
            int blockIdx = m_markdownRenderer->HitTestCopyLink(pos);
            if (blockIdx >= 0) {
                const std::string& code = m_markdownRenderer->GetCodeBlock(static_cast<size_t>(blockIdx));
                if (wxTheClipboard->Open()) {
                    wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(code)));
                    // Render the data to the OS clipboard immediately so
                    // the copied code survives app close (and even an
                    // unclean exit).  Without Flush, wx keeps the bytes
                    // process-owned and empties the clipboard on exit.
                    wxTheClipboard->Flush();
                    wxTheClipboard->Close();
                    m_markdownRenderer->MarkCopyLinkCopied(static_cast<size_t>(blockIdx));
                }
                releaseDisplayCapture();
                return;
            }
            // File chip — new behavior
            int chipIdx = HitTestFileChip(pos);
            if (chipIdx >= 0) {
                HandleFileChipClick(static_cast<size_t>(chipIdx));
                releaseDisplayCapture();
                return;
            }
            // Tool block "[details]" affordance.  While a turn is still
            // appending text, don't let clicks mutate wxRichTextCtrl ranges.
            int tbIdx = HitTestToolBlockAffordance(pos);
            if (tbIdx >= 0) {
                if (!m_toolBlockInteractionEnabled) {
                    wxBell();
                    releaseDisplayCapture();
                    return;
                }
                HandleToolBlockAffordanceClick(static_cast<size_t>(tbIdx));
                releaseDisplayCapture();
                return;
            }
            // Approval buttons (Allow Once / Allow Always / Deny).
            // Same interaction-enabled gate as the affordance, since
            // the callback may trigger further chat doc mutations
            // (denial system message, next tool block stream, etc.).
            // In practice approval cards only appear while the agent
            // is paused — interaction will already be enabled — but
            // this stays consistent with the rest of the click chain.
            int abIdx = HitTestApprovalButton(pos);
            if (abIdx >= 0) {
                if (!m_toolBlockInteractionEnabled) {
                    wxBell();
                    releaseDisplayCapture();
                    return;
                }
                HandleApprovalButtonClick(static_cast<size_t>(abIdx));
                releaseDisplayCapture();
                return;
            }
            // Image thumbnail — open the lightbox viewer.  Read-only
            // (no buffer mutation), so no interaction-enabled gate:
            // safe to view an image even while a turn is streaming.
            // Capture is released BEFORE the modal viewer runs so the
            // richtext control isn't left holding the mouse across a
            // nested event loop.
            const wxString imgSrc = ImageSrcAtPosition(pos);
            if (!imgSrc.empty()) {
                releaseDisplayCapture();
                ShowImageViewer(imgSrc);
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
    // wxEVT_MOTION fires on every pixel of cursor movement, but a single
    // character cell spans many pixels.  Cache the last resolved
    // character position (with a sentinel of -1 for off-text) so that
    // motion within the same cell bails out before re-running the
    // 4-way HitTest scan over file chips, tool blocks, and approval
    // buttons.  In long agent runs these vectors grow to dozens of
    // entries; without the cache the scan ran on every motion pixel.
    //
    // Caveat: if SetToolBlockInteractionEnabled() flips state while
    // the mouse is stationary, the cached cursor decision stays put
    // until the next character-transition.  Acceptable for a perf
    // optimization -- one mouse twitch and the cursor catches up.
    m_displayCtrl->Bind(wxEVT_MOTION,
        [this, lastPos = -1L, lastOverLink = false]
        (wxMouseEvent& event) mutable {
            // Fast path: a transcript with no interactive ranges at all
            // (no code-block Copy links, file chips, tool blocks,
            // approval buttons, or image thumbnails — e.g. a
            // plain-prose conversation) has
            // nothing for the scan below to find.  More importantly,
            // wxRichTextCtrl::HitTest itself walks the paragraph layout
            // tree and is the expensive part of this handler on a long
            // transcript, so skip it too.  Resetting the cache keys means
            // the first motion event after a range appears re-evaluates
            // from scratch.
            if (m_fileChips.empty() && m_toolBlocks.empty() &&
                m_approvalButtons.empty() &&
                !m_hasImageThumbnails &&
                !m_markdownRenderer->HasCopyLinks()) {
                lastPos      = -1L;
                lastOverLink = false;
                event.Skip();
                return;
            }

            long pos = 0;
            auto hit = m_displayCtrl->HitTest(event.GetPosition(), &pos);

            const bool onText =
                (hit == wxTE_HT_ON_TEXT || hit == wxTE_HT_BEFORE);
            // Coalesce all "off text" hits to a single cache key so we
            // bail on every motion event that leaves the text area.
            const long effectivePos = onText ? pos : -1L;

            if (effectivePos == lastPos) {
                // Same cell (or same off-text state) -- the cursor we
                // set last time is still correct.  Preserve the original
                // Skip() semantics: Skip when IBEAM (text-selection drag
                // needs the event), don't Skip when HAND (otherwise
                // wxRichTextCtrl resets the cursor).
                if (!lastOverLink) event.Skip();
                return;
            }

            bool overLink = false;
            if (onText) {
                overLink = (m_markdownRenderer->HitTestCopyLink(pos) >= 0)
                        || (HitTestFileChip(pos) >= 0)
                        || (m_toolBlockInteractionEnabled &&
                            HitTestToolBlockAffordance(pos) >= 0)
                        || (m_toolBlockInteractionEnabled &&
                            HitTestApprovalButton(pos) >= 0)
                        // Image thumbnail — clickable lightbox target.
                        // Guarded by the flag so transcripts without
                        // images pay nothing; the leaf lookup itself is
                        // cheap next to the HitTest above, and the
                        // cell-change cache means it only runs on
                        // character transitions anyway.
                        || (m_hasImageThumbnails &&
                            !ImageSrcAtPosition(pos).empty());
            }

            lastPos      = effectivePos;
            lastOverLink = overLink;

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

    // Replay renders saved assistant messages synchronously. Starting and then
    // immediately clearing the animated dots for every historical assistant
    // message creates extra timer churn and rich-text mutations for no visible
    // benefit, so suppress the indicator while replay batching is active.
    if (IsReplayBatchActive()) return;

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
    EnsureVisibleAtEndIfFollowing();

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
    HideRichTextCaret(m_displayCtrl);
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
        HideRichTextCaret(m_displayCtrl);
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

    const int baseSize = ResolveBaseFontSize(m_displayCtrl);

    wxRichTextAttr cardAttr   = MakeMonoAttr(m_stdoutColor,   baseSize);
    wxRichTextAttr actionAttr = MakeMonoAttr(m_fileChipColor, baseSize,
                                             wxFONTSTYLE_ITALIC);

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

    // COPY, do not take references into m_fileChips here.  The SaveAs
    // branch below opens wxFileDialog::ShowModal(), which spins a nested
    // event loop — streaming deltas keep arriving underneath the dialog.
    // A delta that calls PresentFile() push_back()s into m_fileChips
    // (possible reallocation), and CancelPendingAssistantDisplay()/Clear()
    // erase from it.  Either would leave a reference dangling by the time
    // ShowModal() returns and we touch file.inlineContent.
    const PresentedFile file   = m_fileChips[chipIdx].file;
    const FileAction    action = m_fileChips[chipIdx].action;

    switch (action) {
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
        const ToolBlockRegion& r = m_toolBlocks[i];

        if (r.chevronStart >= 0 &&
            pos >= r.chevronStart &&
            pos <  r.chevronEnd)
            return static_cast<int>(i);

        if (pos >= r.affordanceStart &&
            pos <  r.affordanceEnd)
            return static_cast<int>(i);
    }
    return -1;
}

long ChatDisplay::WriteToolBodyAtCursor(const std::string& body,
                                        const std::string& errorBody)
{
    const int baseSize = ResolveBaseFontSize(m_displayCtrl);

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
        wxRichTextAttr outAttr = MakeMonoAttr(m_stdoutColor, baseSize);

        m_displayCtrl->BeginStyle(outAttr);
        m_displayCtrl->WriteText(wxString::FromUTF8(bodyTrim));
        m_displayCtrl->WriteText("\n");
        m_displayCtrl->EndStyle();
    }
    if (!errorTrim.empty()) {
        wxRichTextAttr errAttr = MakeMonoAttr(wxColour(220, 90, 90), baseSize);

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
        if (tb.chevronStart    >= pivot) tb.chevronStart    += delta;
        if (tb.chevronEnd      >= pivot) tb.chevronEnd      += delta;
    }
    // Approval row: at most one is live at any time, but its position
    // shifts the same way file chips do.  When [show details] above
    // expands/collapses the body, the row below must follow.
    for (auto& ab : m_approvalButtons) {
        if (ab.startPos >= pivot) ab.startPos += delta;
        if (ab.endPos   >= pivot) ab.endPos   += delta;
    }
    if (m_approvalRowStart >= pivot) m_approvalRowStart += delta;
    if (m_approvalRowEnd   >= pivot) m_approvalRowEnd   += delta;

    // Code-block [Copy] affordances are tracked by MarkdownRenderer but
    // live in the SAME document, so they shift identically.  Omitting
    // them left links stale after any toggle above a code block: the
    // hit-test then matched unrelated prose, and MarkCopyLinkCopied
    // would Remove() a stale range and write "Copied" into it.
    if (m_markdownRenderer) m_markdownRenderer->ShiftCopyLinks(pivot, delta);
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

    wxRichTextAttr affAttr = MakeMonoAttr(m_fileChipColor,
                                          ResolveBaseFontSize(m_displayCtrl),
                                          wxFONTSTYLE_ITALIC);

    m_displayCtrl->SetInsertionPoint(r.affordanceStart);
    m_displayCtrl->Remove(r.affordanceStart, r.affordanceEnd);
    m_displayCtrl->BeginStyle(affAttr);
    m_displayCtrl->WriteText(newText);
    m_displayCtrl->EndStyle();

    // Same-length replacement: ranges remain valid, but set them explicitly
    // so the invariant is obvious and future-safe.
    r.affordanceEnd = r.affordanceStart + static_cast<long>(newText.length());
}

void ChatDisplay::SetChevronText(ToolBlockRegion& r, const wxString& newText)
{
    if (!m_displayCtrl) return;
    if (r.chevronStart < 0 || r.chevronEnd <= r.chevronStart) return;

    // Keep this replacement one displayed character. Collapsed uses ">" and
    // expanded uses "▾" so the command line does not shift.
    if ((r.chevronEnd - r.chevronStart) != static_cast<long>(newText.length())) {
        return;
    }

    wxRichTextAttr chevronAttr = MakeMonoAttr(m_fileChipColor,
                                              ResolveBaseFontSize(m_displayCtrl),
                                              wxFONTSTYLE_ITALIC);

    m_displayCtrl->SetInsertionPoint(r.chevronStart);
    m_displayCtrl->Remove(r.chevronStart, r.chevronEnd);
    m_displayCtrl->BeginStyle(chevronAttr);
    m_displayCtrl->WriteText(newText);
    m_displayCtrl->EndStyle();

    r.chevronEnd = r.chevronStart + static_cast<long>(newText.length());
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
        SetChevronText(r, ">");
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
        SetChevronText(r, wxString::FromUTF8("\xE2\x96\xBE"));  // ▾
        r.expanded = true;
    }

    // Keep the affordance line visible after the toggle so the user
    // can immediately click again or see the result of their click.
    m_displayCtrl->ShowPosition(r.affordanceStart);
}

// ── Approval buttons ─────────────────────────────────────────────
// Lifecycle: DisplayToolBlock writes the row when block.requiresApproval
// is true (see below).  Resolution happens in one of two ways:
//   - User clicks one of the buttons → HandleApprovalButtonClick fires,
//     which calls ClearApprovalButtons to wipe the row, then invokes
//     the registered callback with the chosen ApprovalChoice.
//   - User types the keyboard fallback ("approve" / "approve once" /
//     "deny", etc.) in the frame's TryHandlePendingApprovalInput → the
//     frame calls ClearApprovalButtons explicitly before dispatching.
// Either path leaves m_approvalButtons empty and the row removed from
// the rich-text doc, so any subsequent DisplayToolBlock starts clean.

void ChatDisplay::SetApprovalCallback(std::function<void(ApprovalChoice)> callback)
{
    m_approvalCallback = std::move(callback);
}

int ChatDisplay::HitTestApprovalButton(long pos) const
{
    for (size_t i = 0; i < m_approvalButtons.size(); ++i) {
        if (pos >= m_approvalButtons[i].startPos &&
            pos <  m_approvalButtons[i].endPos)
            return static_cast<int>(i);
    }
    return -1;
}

void ChatDisplay::HandleApprovalButtonClick(size_t idx)
{
    if (idx >= m_approvalButtons.size()) return;
    ApprovalChoice choice = m_approvalButtons[idx].choice;

    // Snapshot the callback before mutating UI state so it can't be
    // re-entered with stale buttons in the vector.
    auto cb = m_approvalCallback;

    // Wipe the row first so the user sees their click commit before
    // any follow-up rendering (denial system message, next tool block,
    // etc.) starts streaming.
    ClearApprovalButtons();

    if (cb) cb(choice);
}

void ChatDisplay::ClearApprovalButtons()
{
    if (m_approvalRowStart < 0) return;

    long rowStart = m_approvalRowStart;
    long rowEnd   = m_approvalRowEnd;
    long delta    = -(rowEnd - rowStart);
    long pivot    = rowEnd;

    // Reset tracking state BEFORE shifting so the now-defunct approval
    // entries aren't included in ShiftOtherRegions' update pass.
    m_approvalButtons.clear();
    m_approvalRowStart = -1;
    m_approvalRowEnd   = -1;

    m_displayCtrl->Remove(rowStart, rowEnd);

    // Approval rows live at the chat tail when visible, so in practice
    // nothing follows them and ShiftOtherRegions is a no-op.  Calling
    // it anyway keeps the invariant clean in case future flows queue
    // content beneath an unresolved approval.
    ShiftOtherRegions(nullptr, pivot, delta);
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
    // Cached: the decode-at-full-resolution + high-quality rescale only
    // happens the first time an image is seen; replays load the small
    // cached PNG (see LoadImageThumbnailCached at the top of the file).
    for (const auto& imgPath : inlineImages) {
        wxImage img;
        if (LoadImageThumbnailCached(imgPath, kImageMaxWidth,
                                     kImageMaxHeight, img)) {
            m_displayCtrl->WriteImage(img);
            TagLastWrittenImage(imgPath);
            m_displayCtrl->WriteText("\n");
        }
    }

    // Trailing spacing
    m_displayCtrl->WriteText("\n");

    EnsureVisibleAtEnd();
}

// ─── Image thumbnail source tagging + context menu ───────────────

void ChatDisplay::TagLastWrittenImage(const std::string& absPath)
{
    if (absPath.empty()) return;

    // WriteImage leaves the insertion point just past the image
    // object (images occupy exactly one buffer position), so the
    // leaf at insertion-1 is the image we just wrote.
    const long pos = m_displayCtrl->GetInsertionPoint();
    if (pos <= 0) return;

    wxRichTextObject* obj =
        m_displayCtrl->GetBuffer().GetLeafObjectAtPosition(pos - 1);
    if (auto* img = wxDynamicCast(obj, wxRichTextImage)) {
        img->GetProperties().SetProperty(
            "lb:src", wxString::FromUTF8(absPath));
        // Wake the motion handler's fast path: hover over this
        // thumbnail should now show the hand cursor.
        m_hasImageThumbnails = true;
    }
}

void ChatDisplay::OnImageContextMenu(wxContextMenuEvent& event)
{
    // Keyboard menu key sends wxDefaultPosition — nothing to hit-test.
    const wxPoint screenPt = event.GetPosition();
    if (screenPt == wxDefaultPosition) {
        event.Skip();
        return;
    }

    const wxPoint pt = m_displayCtrl->ScreenToClient(screenPt);
    long pos = 0;
    const auto hit = m_displayCtrl->HitTest(pt, &pos);
    if (hit != wxTE_HT_ON_TEXT && hit != wxTE_HT_BEFORE) {
        event.Skip();
        return;
    }

    const wxString src = ImageSrcAtPosition(pos);
    if (src.empty()) {
        event.Skip();   // not one of our thumbnails — default behavior
        return;
    }

    enum { kIdView = 1, kIdSaveAs, kIdShowInFolder };
    wxMenu menu;
    menu.Append(kIdView,         "View image");
    menu.Append(kIdSaveAs,       "Save image as...");
    menu.Append(kIdShowInFolder, "Show in folder");

    const int sel =
        m_displayCtrl->GetPopupMenuSelectionFromUser(menu, pt);
    if      (sel == kIdView)         ShowImageViewer(src);
    else if (sel == kIdSaveAs)       SaveImageAs(src);
    else if (sel == kIdShowInFolder) ShowInFolder(src);
}

wxString ChatDisplay::ImageSrcAtPosition(long pos) const
{
    // The image is a single-position leaf; depending on which half of
    // the cell was clicked, HitTest may resolve to the position after
    // it.  Check the resolved position first, then one back.
    for (long p : { pos, pos - 1 }) {
        if (p < 0) continue;
        wxRichTextObject* obj =
            m_displayCtrl->GetBuffer().GetLeafObjectAtPosition(p);
        if (auto* img = wxDynamicCast(obj, wxRichTextImage)) {
            const wxString src =
                img->GetProperties().GetPropertyString("lb:src");
            if (!src.empty()) return src;
        }
    }
    return wxString();
}

void ChatDisplay::ShowImageViewer(const wxString& srcPath)
{
    if (!wxFileExists(srcPath)) {
        wxMessageBox("The image file no longer exists:\n" + srcPath,
                     "View image", wxOK | wxICON_INFORMATION,
                     m_displayCtrl);
        return;
    }

    // Full-resolution load — the chat shows a <=300px cached
    // thumbnail, so the viewer is where the real pixels appear.
    wxImage full;
    {
        wxLogNull quiet;   // odd EXIF/ICC chunks shouldn't spam dialogs
        if (!full.LoadFile(srcPath) || !full.IsOk()) {
            wxBell();
            return;
        }
    }

    wxWindow* top = wxGetTopLevelParent(m_displayCtrl);
    if (!top) top = m_displayCtrl;

    // Fit within ~85% of the frame client area, minus a reservation
    // for the close-X header row and padding ring.  Downscale only —
    // upscaling past native resolution just trades sharpness for
    // size, and native is almost always larger than the thumbnail.
    const wxSize avail = top->GetClientSize();
    const int maxW = std::max(320, (int)(avail.GetWidth()  * 0.85));
    const int maxH = std::max(240, (int)(avail.GetHeight() * 0.85) - 48);

    int w = full.GetWidth();
    int h = full.GetHeight();
    if (w > maxW || h > maxH) {
        const double scale = std::min((double)maxW / (double)w,
                                      (double)maxH / (double)h);
        w = std::max(1, (int)(w * scale));
        h = std::max(1, (int)(h * scale));
        full.Rescale(w, h, wxIMAGE_QUALITY_HIGH);
    }

    // Borderless lightbox over the modal scrim: the scrim dims the
    // frame, the dialog is just the image on the chat background
    // with a thin padding ring and a close "X" in the top-right
    // corner.  Any click or key dismisses it; the X is an explicit
    // affordance so the exit is discoverable.
    wxDialog dlg(top, wxID_ANY, wxEmptyString,
                 wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    const wxColour bg = m_displayCtrl->GetBackgroundColour();
    const wxColour fg = m_displayCtrl->GetForegroundColour();
    dlg.SetBackgroundColour(bg);

    // Muted at rest, full foreground on hover — works in both dark
    // and light themes since it's derived from the live palette.
    const wxColour mutedFg((fg.Red()   + bg.Red())   / 2,
                           (fg.Green() + bg.Green()) / 2,
                           (fg.Blue()  + bg.Blue())  / 2);

    auto* closeX = new wxStaticText(&dlg, wxID_ANY, wxString(L"\u2715"));
    closeX->SetForegroundColour(mutedFg);
    closeX->SetBackgroundColour(bg);
    {
        wxFont f = closeX->GetFont();
        f.SetPointSize(f.GetPointSize() + 3);
        closeX->SetFont(f);
    }
    closeX->SetCursor(wxCursor(wxCURSOR_HAND));
    closeX->SetToolTip("Close (Esc)");
    closeX->Bind(wxEVT_ENTER_WINDOW, [closeX, fg](wxMouseEvent& e) {
        closeX->SetForegroundColour(fg);
        closeX->Refresh();
        e.Skip();
    });
    closeX->Bind(wxEVT_LEAVE_WINDOW, [closeX, mutedFg](wxMouseEvent& e) {
        closeX->SetForegroundColour(mutedFg);
        closeX->Refresh();
        e.Skip();
    });

    auto* bitmap = new wxStaticBitmap(&dlg, wxID_ANY, wxBitmap(full));

    auto* topRow = new wxBoxSizer(wxHORIZONTAL);
    topRow->AddStretchSpacer(1);
    topRow->Add(closeX, 0, wxTOP | wxRIGHT, 10);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(topRow, 0, wxEXPAND);
    sizer->Add(bitmap, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();

    const auto dismiss = [&dlg](wxMouseEvent&) {
        dlg.EndModal(wxID_OK);
    };
    closeX->Bind(wxEVT_LEFT_UP, dismiss);
    bitmap->Bind(wxEVT_LEFT_UP, dismiss);
    dlg.Bind(wxEVT_LEFT_UP, dismiss);

    // Right-click parity with the chat thumbnail: the same Save /
    // Show-in-folder menu is available while the image is expanded
    // ("View image" is omitted — it is already in view).  Bound on
    // both the bitmap and the dialog; wxContextMenuEvent propagates,
    // and the handled event is not Skip()ed, so it fires once.
    // srcPath is captured by value: the viewer runs a nested modal
    // loop and must not depend on the caller's reference outliving
    // menu callbacks.
    const wxString viewerSrc = srcPath;
    const auto contextMenu =
        [this, &dlg, viewerSrc](wxContextMenuEvent&) {
            enum { kIdSaveAs = 1, kIdShowInFolder };
            wxMenu menu;
            menu.Append(kIdSaveAs,       "Save image as...");
            menu.Append(kIdShowInFolder, "Show in folder");

            const int sel = dlg.GetPopupMenuSelectionFromUser(menu);
            if      (sel == kIdSaveAs)       SaveImageAs(viewerSrc);
            else if (sel == kIdShowInFolder) ShowInFolder(viewerSrc);
        };
    bitmap->Bind(wxEVT_CONTEXT_MENU, contextMenu);
    dlg.Bind(wxEVT_CONTEXT_MENU, contextMenu);
    dlg.Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& e) {
        const int key = e.GetKeyCode();
        if (key == WXK_ESCAPE || key == WXK_RETURN || key == WXK_SPACE) {
            dlg.EndModal(wxID_CANCEL);
        } else {
            e.Skip();
        }
    });

    LbShowModalWithScrim(*top, dlg);
}

void ChatDisplay::SaveImageAs(const wxString& srcPath)
{
    if (!wxFileExists(srcPath)) {
        wxMessageBox("The image file no longer exists:\n" + srcPath,
                     "Save image", wxOK | wxICON_INFORMATION,
                     m_displayCtrl);
        return;
    }

    const wxFileName fn(srcPath);
    const wxString ext = fn.GetExt().Lower();
    const wxString wildcard = ext.IsEmpty()
        ? wxString("All files (*.*)|*.*")
        : wxString::Format("%s image (*.%s)|*.%s|All files (*.*)|*.*",
                           ext.Upper(), ext, ext);

    wxFileDialog dlg(m_displayCtrl, "Save image as", "",
                     fn.GetFullName(), wildcard,
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;

    if (!wxCopyFile(srcPath, dlg.GetPath(), /*overwrite*/ true)) {
        wxMessageBox("Could not write:\n" + dlg.GetPath(),
                     "Save image", wxOK | wxICON_ERROR, m_displayCtrl);
    }
}

void ChatDisplay::ShowInFolder(const wxString& path)
{
#ifdef __WXMSW__
    // Native separators — explorer's /select rejects forward slashes.
    const wxString native = wxFileName(path).GetFullPath();
    wxExecute("explorer /select,\"" + native + "\"", wxEXEC_ASYNC);
#else
    wxLaunchDefaultApplication(wxFileName(path).GetPath());
#endif
}

void ChatDisplay::DisplayInlineImages(const std::vector<std::string>& imagePaths)
{
    if (imagePaths.empty()) return;

    SetInsertionPointToEnd();

    bool wroteAny = false;
    for (const auto& imgPath : imagePaths) {
        // Cached thumbnail path — same rules as the user-attachment
        // thumbnails in DisplayUserMessage (see LoadImageThumbnailCached
        // at the top of the file).
        wxImage img;
        if (!LoadImageThumbnailCached(imgPath, kImageMaxWidth,
                                      kImageMaxHeight, img)) continue;

        m_displayCtrl->WriteImage(img);
        TagLastWrittenImage(imgPath);
        m_displayCtrl->WriteText("\n");
        wroteAny = true;
    }

    if (wroteAny) {
        m_displayCtrl->WriteText("\n");
        EnsureVisibleAtEnd();
    }
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

    // Long tool commands (especially generated PowerShell) can wrap across
    // most of the viewport. Show a compact one-line preview in the card and
    // retain the exact command inside [show details]. This is render-only:
    // ToolBlock/history still keep the original command verbatim.
    wxString commandPreview = wxString::FromUTF8(block.commandEcho);
    commandPreview.Replace("\r", " ");
    commandPreview.Replace("\n", " ");
    commandPreview.Replace("\t", " ");
    while (commandPreview.Find("  ") != wxNOT_FOUND)
        commandPreview.Replace("  ", " ");

    constexpr size_t kMaxCommandPreviewChars = 200;
    constexpr size_t kCommandPreviewHeadChars = 196;
    const bool commandWasCompacted =
        commandPreview.length() > kMaxCommandPreviewChars;
    if (commandWasCompacted) {
        commandPreview = commandPreview.Left(kCommandPreviewHeadChars) +
                         wxString::FromUTF8("\xE2\x80\xA6"); // …
    }

    std::string detailBody = block.body;
    if (commandWasCompacted && !block.requiresApproval) {
        detailBody = "Full command:\n" + block.commandEcho;
        if (!block.body.empty())
            detailBody += "\n\nOutput:\n" + block.body;
    }

    const bool hasDetails = !detailBody.empty() || !block.errorBody.empty();

    // Initial expanded state is decided by caller intent (startExpanded)
    // OR by automatic failure classification.  User-typed slash commands
    // pass startExpanded=true so they always show output; agent paths
    // pass the default (false), letting failures auto-expand while
    // successes start collapsed.
    bool expanded = hasDetails && (startExpanded || IsToolBlockFailure(block));

    long commandChevronStart = -1;
    long commandChevronEnd   = -1;

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
    // The leading chevron is also a details toggle when body output exists:
    //   >  collapsed
    //   ▾  expanded
    if (!commandPreview.empty()) {
        wxRichTextAttr cmdAttr = MakeMonoAttr(m_systemColor, baseSize);
        wxRichTextAttr chevronAttr = hasDetails
            ? MakeMonoAttr(m_fileChipColor, baseSize, wxFONTSTYLE_ITALIC)
            : cmdAttr;

        const wxString chevronText = (hasDetails && expanded)
            ? wxString::FromUTF8("\xE2\x96\xBE")  // ▾
            : wxString(">");

        m_displayCtrl->BeginStyle(chevronAttr);
        commandChevronStart = m_displayCtrl->GetInsertionPoint();
        m_displayCtrl->WriteText(chevronText);
        commandChevronEnd = m_displayCtrl->GetInsertionPoint();
        m_displayCtrl->EndStyle();

        m_displayCtrl->BeginStyle(cmdAttr);
        m_displayCtrl->WriteText(" ");
        m_displayCtrl->WriteText(commandPreview);
        m_displayCtrl->WriteText("\n");
        m_displayCtrl->EndStyle();

        if (!hasDetails) {
            commandChevronStart = -1;
            commandChevronEnd   = -1;
        }
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
    // Initial expanded state was decided before the command echo so the
    // command chevron can render with the correct direction.
    long bodyStart = m_displayCtrl->GetInsertionPoint();
    long bodyChars = expanded
        ? WriteToolBodyAtCursor(detailBody, block.errorBody)
        : 0;
    long bodyEnd   = bodyStart + bodyChars;

    // ── Affordance: "[hide details]" when expanded, "[show details]"
    // when collapsed — the label invites the OPPOSITE action.  Same
    // soft-blue italic monospace as file chips so all click targets
    // read consistently.  Trailing \n keeps the affordance on its own
    // line BUT is captured outside the click range so only the
    // bracketed text is hittable.
    if (!detailBody.empty() || !block.errorBody.empty()) {
        long affStart = m_displayCtrl->GetInsertionPoint();

        wxRichTextAttr affAttr = MakeMonoAttr(m_fileChipColor, baseSize,
                                              wxFONTSTYLE_ITALIC);

        m_displayCtrl->BeginStyle(affAttr);
        m_displayCtrl->WriteText(expanded ? "[hide details]" : "[show details]");
        m_displayCtrl->EndStyle();

        long affEnd = m_displayCtrl->GetInsertionPoint();

        ToolBlockRegion region;
        region.affordanceStart = affStart;
        region.affordanceEnd   = affEnd;
        region.chevronStart    = commandChevronStart;
        region.chevronEnd      = commandChevronEnd;
        region.bodyStart       = bodyStart;
        region.bodyEnd         = bodyEnd;
        region.body            = detailBody;
        region.errorBody       = block.errorBody;
        region.expanded        = expanded;
        m_toolBlocks.push_back(region);

        m_displayCtrl->WriteText("\n");
    }

    // ── Approval button row.  Written even when body+errorBody are
    // empty (an approval card commonly has only commandEcho + the
    // proposed tool name visible by default).  Style matches the
    // [show details] affordance so click targets read uniformly.
    // The trailing \n is captured INSIDE m_approvalRowEnd so
    // ClearApprovalButtons removes the entire visual line in one
    // Remove() call.
    if (block.requiresApproval) {
        // Defensive: if a previous approval card is somehow still
        // tracked (shouldn't happen — both the frame's typed fallback
        // and HandleApprovalButtonClick wipe it on resolution), clear
        // its visible row before writing the new one.
        ClearApprovalButtons();

        long rowStart = m_displayCtrl->GetInsertionPoint();

        wxRichTextAttr btnAttr = MakeMonoAttr(m_fileChipColor, baseSize,
                                              wxFONTSTYLE_ITALIC);

        m_displayCtrl->BeginStyle(btnAttr);

        long onceStart = m_displayCtrl->GetInsertionPoint();
        m_displayCtrl->WriteText("[ Allow Once ]");
        long onceEnd = m_displayCtrl->GetInsertionPoint();

        m_displayCtrl->WriteText("   ");

        long alwaysStart = m_displayCtrl->GetInsertionPoint();
        m_displayCtrl->WriteText("[ Allow Always ]");
        long alwaysEnd = m_displayCtrl->GetInsertionPoint();

        m_displayCtrl->WriteText("   ");

        long denyStart = m_displayCtrl->GetInsertionPoint();
        m_displayCtrl->WriteText("[ Deny ]");
        long denyEnd = m_displayCtrl->GetInsertionPoint();

        m_displayCtrl->EndStyle();
        m_displayCtrl->WriteText("\n");

        long rowEnd = m_displayCtrl->GetInsertionPoint();

        ApprovalButtonRegion once   { onceStart,   onceEnd,   ApprovalChoice::Once   };
        ApprovalButtonRegion always { alwaysStart, alwaysEnd, ApprovalChoice::Always };
        ApprovalButtonRegion deny   { denyStart,   denyEnd,   ApprovalChoice::Deny   };
        m_approvalButtons.push_back(once);
        m_approvalButtons.push_back(always);
        m_approvalButtons.push_back(deny);
        m_approvalRowStart = rowStart;
        m_approvalRowEnd   = rowEnd;
    }

    // Trailing blank line for separation.
    m_displayCtrl->WriteText("\n");

    EnsureVisibleAtEndIfFollowing();
    HideRichTextCaret(m_displayCtrl);
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
    HideRichTextCaret(m_displayCtrl);
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
    //
    // Leading whitespace is tolerated -- some models emit "\n<think>"
    // or "  <think>" as the first delta.  Matching only at byte 0
    // would render the literal tag in streaming while the replay path
    // (DisplayAssistantMessage, which uses find()) would handle it
    // correctly -- same response, different display.  We find the
    // first non-whitespace byte and probe from there, falling back to
    // "keep buffering" while the buffer is entirely whitespace.
    if (m_isFirstAssistantDelta) {
        m_thinkProbeBuffer += remainingDelta;

        const size_t firstNonWs =
            m_thinkProbeBuffer.find_first_not_of(" \t\r\n");

        if (firstNonWs == std::string::npos) {
            // Entirely whitespace so far -- wait for more bytes.
            return;
        }

        const size_t availLen = m_thinkProbeBuffer.size() - firstNonWs;

        // Enough bytes after the leading whitespace to compare against
        // the full marker?
        if (availLen >= thought_start_marker.size()) {
            if (m_thinkProbeBuffer.compare(firstNonWs,
                                           thought_start_marker.size(),
                                           thought_start_marker) == 0) {
                // Confirmed: thinking model response.  Drop both the
                // leading whitespace and the tag.
                m_isInThoughtBlock = true;
                m_isFirstAssistantDelta = false;
                remainingDelta = m_thinkProbeBuffer.substr(
                    firstNonWs + thought_start_marker.size());
                m_thinkProbeBuffer.clear();
                // Fall through to process remainingDelta as thought content
            }
            else {
                // Not a <think> tag — flush entire buffer as normal content
                // (including the leading whitespace; downstream trim handles it).
                m_isFirstAssistantDelta = false;
                remainingDelta = m_thinkProbeBuffer;
                m_thinkProbeBuffer.clear();
                // Fall through to process remainingDelta as normal content
            }
        }
        else if (thought_start_marker.compare(0, availLen,
                                              m_thinkProbeBuffer,
                                              firstNonWs, availLen) == 0) {
            // Partial prefix match (e.g. " <thi") — keep buffering.
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

            // Same visibility/emission split as the holdback branch
            // below: a thought_part that is pure whitespace still has to
            // be written once visible content exists, or the space
            // before a </think> that lands on a chunk boundary is lost.
            if (!thought_part.empty()) {
                const bool visible = hasVisibleChars(thought_part);
                if (visible) ClearThinkingIndicator();
                if (visible || m_hasRenderedAssistantContent) {
                    AppendFormattedText(thought_part, m_thoughtColor);
                }
                if (visible) m_hasRenderedAssistantContent = true;
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
                // which doesn't scroll) — scroll now (if following).
                EnsureVisibleAtEndIfFollowing();
            }
        }
        else {
            // No end marker yet. Hold back the last 7 chars (length of
            // "</think>" minus 1) so a split tag is caught on the next delta.
            const size_t kHoldBack = thought_end_marker.size() - 1;  // 7
            size_t safeLen = (combined.size() > kHoldBack)
                           ? combined.size() - kHoldBack : 0;

            // Don't cut a UTF-8 sequence in half.  safeLen is a byte
            // offset; if it lands on a continuation byte (10xxxxxx),
            // back off until the cut sits on a lead/ASCII byte so the
            // entire partial character moves into the probe buffer.
            // wxString::FromUTF8 on wxMSW returns an EMPTY string for
            // invalid UTF-8 — without this, a chunk of thought text
            // ending mid-character would silently vanish (and the
            // orphaned lead byte would corrupt the next chunk too).
            // Hits regularly on models that think in non-ASCII.
            while (safeLen > 0 &&
                   (static_cast<unsigned char>(combined[safeLen]) & 0xC0) == 0x80) {
                --safeLen;
            }

            std::string safeToRender = combined.substr(0, safeLen);
            m_thinkEndProbeBuffer = combined.substr(safeLen);

            if (!m_hasRenderedAssistantContent) {
                trimLeadingWhitespace(safeToRender);
            }

            // ── Whitespace-only chunks must still be written ─────
            // The old form was `if (hasVisibleChars(safeToRender))
            // { Clear; Append; mark rendered; }`, which silently
            // DROPPED a chunk that was pure whitespace -- it lives in
            // a local, and only combined.substr(safeLen) survives in
            // the probe buffer.
            //
            // That is reachable constantly, not rarely.  "</think>" is
            // 8 bytes so kHoldBack is 7; a streamed reasoning token of
            // exactly 8 bytes beginning with a space (" summary",
            // " economy", " clearly" -- i.e. any 7-letter word) leaves
            // safeLen == 1 and safeToRender == " ".  Dropping it welds
            // the word onto the previous one: "provide asummary".
            //
            // So: decide visibility and byte-emission separately.  Once
            // anything visible has been rendered, EVERY subsequent byte
            // goes out, whitespace included.  Before that point the
            // trim above has already removed leading blanks, so a
            // still-empty safeToRender is genuinely nothing to draw.
            if (!safeToRender.empty()) {
                const bool visible = hasVisibleChars(safeToRender);
                if (visible) ClearThinkingIndicator();
                if (visible || m_hasRenderedAssistantContent) {
                    AppendFormattedText(safeToRender, m_thoughtColor);
                }
                if (visible) m_hasRenderedAssistantContent = true;
            }
            // AppendFormattedText doesn't scroll — do it here
            // (only while the user is following the stream)
            EnsureVisibleAtEndIfFollowing();
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

    HideRichTextCaret(m_displayCtrl);
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
    HideRichTextCaret(m_displayCtrl);
}

void ChatDisplay::CancelPendingAssistantDisplay()
{
    ClearThinkingIndicator();

    if (m_displayCtrl && m_currentAssistantStartPos >= 0) {
        SetInsertionPointToEnd();
        long end = m_displayCtrl->GetInsertionPoint();
        if (end > m_currentAssistantStartPos) {
            // Prune region entries that lived inside the range about to be
            // removed.  Without this, their stale startPos/endPos remain in
            // the vectors, and the HitTest* scans can later match clicks
            // against new content that lands at the same offsets.
            //
            // Earlier regions (from prior turns) with positions below the
            // pivot stay put -- the doc range they reference isn't being
            // touched, so they remain valid click targets.
            const long pivot = m_currentAssistantStartPos;
            m_fileChips.erase(
                std::remove_if(m_fileChips.begin(), m_fileChips.end(),
                    [pivot](const FileChipRegion& r) {
                        return r.startPos >= pivot;
                    }),
                m_fileChips.end());
            m_toolBlocks.erase(
                std::remove_if(m_toolBlocks.begin(), m_toolBlocks.end(),
                    [pivot](const ToolBlockRegion& r) {
                        return r.bodyStart >= pivot;
                    }),
                m_toolBlocks.end());
            // Approval row + its three buttons are written as one unit
            // at the same insertion point, so they prune atomically.
            if (m_approvalRowStart >= pivot) {
                m_approvalButtons.clear();
                m_approvalRowStart = -1;
                m_approvalRowEnd   = -1;
            }

            // Reset() below deliberately preserves m_copyLinks so links
            // from earlier turns keep working -- but links inside the
            // range we are about to Remove() are not "earlier turns",
            // they are about to point at whatever lands here next.
            if (m_markdownRenderer) m_markdownRenderer->PruneCopyLinksFrom(pivot);

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

    EnsureVisibleAtEndIfFollowing();
    HideRichTextCaret(m_displayCtrl);

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

            while (!toProcess.empty()) {
                size_t ts = toProcess.find(kThinkStart);
                if (ts == std::string::npos) {
                    // No (more) think block — render remainder as answer
                    m_markdownRenderer->ProcessDelta(toProcess, accentColor);
                    toProcess.clear();
                } else {
                    // Render any answer text that precedes the think block
                    if (ts > 0) {
                        m_markdownRenderer->ProcessDelta(toProcess.substr(0, ts), accentColor);
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
                        }
                        toProcess.clear();
                    } else {
                        // Well-formed block — render thought dimly
                        std::string thought = toProcess.substr(contentStart, te - contentStart);
                        size_t f = thought.find_first_not_of(" \t\r\n");
                        if (f != std::string::npos && f > 0) thought.erase(0, f);
                        if (!thought.empty()) {
                            AppendFormattedText(thought, m_thoughtColor);
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
        }
    }

    AppendFormattedText("\n\n", accentColor);

    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();
    m_currentAssistantStartPos = -1;

    EnsureVisibleAtEndIfFollowing();
    HideRichTextCaret(m_displayCtrl);
}

void ChatDisplay::BeginReplayBatch()
{
    if (!m_displayCtrl) return;

    if (m_replayBatchDepth++ == 0) {
        m_replayBatchNeedsScroll = false;
        m_displayCtrl->Freeze();
        m_replayBatchFrozen = true;

        // Put the markdown renderer in bulk mode for the replay: it skips
        // its per-message partial-line preview and per-call ShowPosition,
        // the latter of which would otherwise force one layout pass per
        // message over a growing document (O(n^2) replay). The single
        // scroll-to-end is handled by EndReplayBatch.
        if (m_markdownRenderer) m_markdownRenderer->SetBulkMode(true);
    }
}

void ChatDisplay::EndReplayBatch()
{
    if (m_replayBatchDepth <= 0) return;

    --m_replayBatchDepth;
    if (m_replayBatchDepth > 0) return;

    // Replay finished — return the renderer to live-streaming behaviour.
    if (m_markdownRenderer) m_markdownRenderer->SetBulkMode(false);

    if (m_displayCtrl) {
        HideRichTextCaret(m_displayCtrl);

        if (m_replayBatchFrozen) {
            m_displayCtrl->Thaw();
            m_replayBatchFrozen = false;
        }

        if (m_replayBatchNeedsScroll) {
            // Landing at the end of a restored conversation re-engages
            // stream-follow mode for whatever streams next.
            m_followStream = true;

            // Scroll AFTER Thaw, not before.  ShowPosition against a
            // frozen control computes from a stale layout on MSW; image
            // thumbnails and code-block sizing finish only after the
            // thaw, so the document grows and a pre-thaw scroll lands
            // short of the true bottom (the "old chat opens scrolled
            // slightly up" bug).
            m_displayCtrl->LayoutContent();
            m_displayCtrl->SetInsertionPointEnd();
            m_displayCtrl->ShowPosition(m_displayCtrl->GetLastPosition());

            // Second, deferred pass once pending size/layout events have
            // settled.  Scrolling to the virtual-size bottom (rather than
            // a character position) is what deterministically pins the
            // view to the end of a large restored conversation.
            wxRichTextCtrl* ctrl = m_displayCtrl;
            ctrl->CallAfter([ctrl]() {
                ctrl->SetInsertionPointEnd();
                ctrl->ShowPosition(ctrl->GetLastPosition());
                int vx = 0, vy = 0, ppuX = 0, ppuY = 0;
                ctrl->GetVirtualSize(&vx, &vy);
                ctrl->GetScrollPixelsPerUnit(&ppuX, &ppuY);
                if (ppuY > 0) ctrl->Scroll(-1, vy / ppuY);
            });
        }

        // Thaw usually schedules a repaint, but Refresh() keeps the end of a
        // large restored conversation visually deterministic across wx/MSW
        // builds after a long frozen replay.
        m_displayCtrl->Refresh();
    }

    m_replayBatchNeedsScroll = false;
}

void ChatDisplay::Clear()
{
    // A fresh/cleared conversation always starts following the
    // stream.
    m_followStream = true;

    // Must stop the timer before wiping the document, otherwise the next
    // tick will try to Remove() a range that no longer exists.
    ClearThinkingIndicator();

    // Char-position ranges in m_fileChips become invalid once the document
    // is cleared — drop them so the click handler can't hit stale regions.
    m_fileChips.clear();
    m_toolBlocks.clear();
    m_hasImageThumbnails = false;

    // Approval card state.  If Clear() ever fires while an approval row
    // is live (unusual, but possible if a new chat is started without
    // resolving a pending tool approval), its position offsets point
    // into a doc that no longer exists.  Wipe explicitly.
    m_approvalButtons.clear();
    m_approvalRowStart = -1;
    m_approvalRowEnd   = -1;

    // Per-turn streaming state.  Same defensive reasoning: a Clear()
    // during streaming would leave m_currentAssistantStartPos and the
    // think-probe buffers pointing at content that no longer exists.
    m_isInThoughtBlock = false;
    m_isFirstAssistantDelta = true;
    m_hasRenderedAssistantContent = false;
    m_thinkProbeBuffer.clear();
    m_thinkEndProbeBuffer.clear();
    m_currentAssistantStartPos = -1;

    // Animation state tracks a doc range too.
    m_animActive   = false;
    m_animStartPos = -1;

    // Drop the persistence context too — the new conversation's context
    // (if any) will be set by whoever drives the next stream.
    ClearFilePersistenceContext();

    if (m_displayCtrl) {
        m_displayCtrl->Clear();
        HideRichTextCaret(m_displayCtrl);
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
    EnsureVisibleAtEndIfFollowing();
    HideRichTextCaret(m_displayCtrl);
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
    HideRichTextCaret(m_displayCtrl);
}

void ChatDisplay::SetInsertionPointToEnd()
{
    if (m_displayCtrl) {
        m_displayCtrl->SetInsertionPointEnd();
        HideRichTextCaret(m_displayCtrl);
    }
}

void ChatDisplay::EnsureVisibleAtEnd()
{
    if (!m_displayCtrl) return;

    if (IsReplayBatchActive()) {
        m_replayBatchNeedsScroll = true;
        return;
    }

    // A deliberate jump to the end always re-engages follow mode:
    // sending a message, /commands, ScrollToBottom, replay end.
    m_followStream = true;

    m_displayCtrl->ShowPosition(m_displayCtrl->GetLastPosition());
    HideRichTextCaret(m_displayCtrl);
}

void ChatDisplay::EnsureVisibleAtEndIfFollowing()
{
    if (!m_displayCtrl) return;

    // Replay batches keep their single-scroll-at-end contract.
    if (IsReplayBatchActive()) {
        m_replayBatchNeedsScroll = true;
        return;
    }

    if (m_followStream) {
        m_displayCtrl->ShowPosition(m_displayCtrl->GetLastPosition());
    }
    HideRichTextCaret(m_displayCtrl);
}

bool ChatDisplay::IsNearBottom() const
{
    if (!m_displayCtrl) return true;

    int ppuX = 0, ppuY = 0;
    m_displayCtrl->GetScrollPixelsPerUnit(&ppuX, &ppuY);
    int vx = 0, vy = 0;
    m_displayCtrl->GetViewStart(&vx, &vy);
    int vw = 0, vh = 0;
    m_displayCtrl->GetVirtualSize(&vw, &vh);

    const int clientH = m_displayCtrl->GetClientSize().GetHeight();
    const int topPx   = (ppuY > 0) ? vy * ppuY : vy;
    const int gap     = vh - (topPx + clientH);
    return gap <= kFollowSlackPx;
}

void ChatDisplay::UpdateFollowFromScrollPosition()
{
    // Wherever the user's scroll input landed decides follow mode:
    // near the bottom = follow the stream, anywhere else = stay put.
    m_followStream = IsNearBottom();
}
