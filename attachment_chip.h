// attachment_chip.h
// Composer attachment cards — the pending-attachment strip shown above
// the input while files are queued but not yet sent.
//
// Replaces the old text pill ("[icon] name  ×") with two owner-drawn
// card shapes:
//
//   • Image  → square thumbnail card, cover-cropped, rounded corners,
//              filename tucked in a gradient scrim along the bottom.
//   • File   → wide card with an extension tile, filename, and a
//              "KIND · SIZE" meta line.
//
// Both share the same height so a wxWrapSizer row stays level, and both
// carry a circular × badge in the top-right that lights up on hover.
//
// Image cards are clickable: a click anywhere outside the × badge fires
// onOpen (the frame shows the full-size lightbox, image_lightbox.h).
// Hover shows a hand cursor and a light veil over the thumbnail so the
// affordance is discoverable.
//
// No child windows: the whole card is one wxPanel painted through a
// wxGraphicsContext, so hover tracking is a single Bind and there are no
// child-window background seams to fight.

#pragma once

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/control.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

#include "theme.h"

// ═══════════════════════════════════════════════════════════════════
//  AttachmentChipModel — everything the card needs to paint itself.
//  Built by the frame from a PendingAttachment; the widget stays
//  ignorant of AttachmentManager so it can be reused for the
//  already-sent attachment rows later.
// ═══════════════════════════════════════════════════════════════════

struct AttachmentChipModel {
    bool        isImage  = false;
    std::string name;                 // "Capture.PNG"
    std::string kindLabel;            // "PNG image", "C++ source", "PDF"
    size_t      byteSize = 0;         // 0 = unknown, meta line omits size
    wxImage     preview;              // optional; only used when isImage
};

// ═══════════════════════════════════════════════════════════════════
//  AttachmentChip
// ═══════════════════════════════════════════════════════════════════

class AttachmentChip : public wxPanel
{
public:
    AttachmentChip(wxWindow* parent,
                   size_t index,
                   AttachmentChipModel model,
                   const ThemeData& theme,
                   std::function<void(size_t)> onRemove,
                   std::function<void(size_t)> onOpen = {})
        : wxPanel(parent, wxID_ANY)
        , m_index(index)
        , m_model(std::move(model))
        , m_onRemove(std::move(onRemove))
        , m_onOpen(std::move(onOpen))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_ARROW));

        ResolveColours(theme);

        m_cardH   = FromDIP(kCardHeightDip);
        m_radius  = FromDIP(kRadiusDip);
        m_closeR  = FromDIP(kCloseRadiusDip);

        const int cardW = m_model.isImage
                        ? m_cardH                       // square thumbnail
                        : FromDIP(kFileCardWidthDip);
        SetInitialSize(wxSize(cardW, m_cardH));
        SetMinSize(wxSize(cardW, m_cardH));

        if (m_model.isImage && m_model.preview.IsOk())
            m_thumb = BuildRoundedCover(m_model.preview, cardW, m_cardH,
                                        (double)m_radius);

        SetToolTip(BuildTooltip());
        ApplyCursor();

        Bind(wxEVT_PAINT,        &AttachmentChip::OnPaint,   this);
        Bind(wxEVT_MOTION,       &AttachmentChip::OnMotion,  this);
        Bind(wxEVT_ENTER_WINDOW, &AttachmentChip::OnEnter,   this);
        Bind(wxEVT_LEAVE_WINDOW, &AttachmentChip::OnLeave,   this);
        Bind(wxEVT_LEFT_UP,      &AttachmentChip::OnLeftUp,  this);
    }

private:
    // ── Layout constants (DIP; scaled through FromDIP) ─────────────
    static constexpr int kCardHeightDip    = 62;
    static constexpr int kFileCardWidthDip = 208;
    static constexpr int kRadiusDip        = 10;
    static constexpr int kCloseRadiusDip   = 9;
    static constexpr int kTileDip          = 34;   // extension tile size
    static constexpr int kPadDip           = 10;

    size_t                      m_index;
    AttachmentChipModel         m_model;
    std::function<void(size_t)> m_onRemove;
    std::function<void(size_t)> m_onOpen;

    wxBitmap m_thumb;
    int  m_cardH  = 62;
    int  m_radius = 10;
    int  m_closeR = 9;
    bool m_hover      = false;
    bool m_hoverClose = false;

    wxColour m_surface, m_surfaceHover, m_border, m_borderHover;
    wxColour m_text, m_meta, m_accent, m_tileText;
    wxColour m_closeBg, m_closeBgHover, m_closeGlyph;

    // ── Colour derivation ─────────────────────────────────────────
    // The chip sits on bgMain but should read as a raised surface, so
    // it borrows the input-field background rather than the old flat
    // attachChipBg pill colour.  Everything else is derived by mixing
    // toward the text colour, which keeps all 12 themes working
    // without adding new ThemeData fields.

    static wxColour Mix(const wxColour& a, const wxColour& b, double t)
    {
        t = std::clamp(t, 0.0, 1.0);
        return wxColour(
            (unsigned char)(a.Red()   + (b.Red()   - a.Red())   * t),
            (unsigned char)(a.Green() + (b.Green() - a.Green()) * t),
            (unsigned char)(a.Blue()  + (b.Blue()  - a.Blue())  * t));
    }

    static bool IsDarkColour(const wxColour& c)
    {
        return (0.299 * c.Red() + 0.587 * c.Green() + 0.114 * c.Blue()) < 128.0;
    }

    void ResolveColours(const ThemeData& t)
    {
        const bool dark = IsDarkColour(t.bgMain);

        m_surface      = t.bgInputField;
        m_surfaceHover = Mix(t.bgInputField, t.textPrimary, dark ? 0.08 : 0.05);
        m_border       = Mix(t.bgInputField, t.borderSubtle, 0.85);
        m_borderHover  = Mix(m_border, t.attachIndicator, 0.55);

        m_text     = t.textPrimary;
        m_meta     = t.textMuted;
        m_accent   = t.attachIndicator;
        m_tileText = dark ? t.bgMain : wxColour(255, 255, 255);

        m_closeBg      = dark ? wxColour(18, 18, 20) : wxColour(70, 74, 82);
        m_closeBgHover = dark ? wxColour(48, 50, 56) : wxColour(30, 32, 38);
        m_closeGlyph   = wxColour(245, 245, 247);
    }

    // ── Helpers ───────────────────────────────────────────────────

    static std::string Extension(const std::string& name)
    {
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos || dot + 1 >= name.size()) return "FILE";
        std::string ext = name.substr(dot + 1);
        if (ext.size() > 4) ext = ext.substr(0, 4);
        for (char& c : ext)
            c = (char)std::toupper((unsigned char)c);
        return ext;
    }

    static wxString HumanBytes(size_t bytes)
    {
        if (bytes == 0) return wxEmptyString;
        const double kb = bytes / 1024.0;
        if (kb < 1.0)     return wxString::Format("%zu B", bytes);
        if (kb < 1024.0)  return wxString::Format("%.0f KB", kb);
        const double mb = kb / 1024.0;
        if (mb < 1024.0)  return wxString::Format("%.1f MB", mb);
        return wxString::Format("%.1f GB", mb / 1024.0);
    }

    wxString BuildTooltip() const
    {
        wxString tip = wxString::FromUTF8(m_model.name);
        wxString sz  = HumanBytes(m_model.byteSize);
        if (!m_model.kindLabel.empty())
            tip += "\n" + wxString::FromUTF8(m_model.kindLabel);
        if (!sz.empty())
            tip += (m_model.kindLabel.empty() ? "\n" : "  ·  ") + sz;
        tip += CanOpen() ? "\nClick to view  ·  × to remove"
                         : "\nClick × to remove";
        return tip;
    }

    // Only a decoded image with an open handler is clickable; a payload
    // wx couldn't decode renders as a file card and stays inert.
    bool CanOpen() const
    {
        return m_model.isImage && m_thumb.IsOk() && (bool)m_onOpen;
    }

    void ApplyCursor()
    {
        SetCursor(wxCursor((m_hoverClose || (m_hover && CanOpen()))
                           ? wxCURSOR_HAND : wxCURSOR_ARROW));
    }

    // Cover-crop `src` to w×h, then punch antialiased rounded corners
    // into its alpha channel.  wxGraphicsContext has no path clip, so
    // the mask is baked into the bitmap once at construction instead of
    // being re-applied on every paint.
    static wxBitmap BuildRoundedCover(const wxImage& src, int w, int h,
                                      double radius)
    {
        if (!src.IsOk() || w <= 0 || h <= 0) return wxBitmap();

        const int sw = src.GetWidth(), sh = src.GetHeight();
        if (sw <= 0 || sh <= 0) return wxBitmap();

        const double scale = std::max((double)w / sw, (double)h / sh);
        const int scaledW = std::max(w, (int)std::ceil(sw * scale));
        const int scaledH = std::max(h, (int)std::ceil(sh * scale));

        wxImage img = src.Scale(scaledW, scaledH, wxIMAGE_QUALITY_HIGH);
        img = img.GetSubImage(wxRect((scaledW - w) / 2, (scaledH - h) / 2, w, h));

        if (!img.HasAlpha()) img.InitAlpha();
        unsigned char* alpha = img.GetAlpha();
        if (!alpha) return wxBitmap(img);

        const double r = std::max(0.0, std::min(radius, std::min(w, h) / 2.0));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double cx, cy;
                if      (x < r      && y < r)      { cx = r;     cy = r;     }
                else if (x >= w - r && y < r)      { cx = w - r; cy = r;     }
                else if (x < r      && y >= h - r) { cx = r;     cy = h - r; }
                else if (x >= w - r && y >= h - r) { cx = w - r; cy = h - r; }
                else continue;

                const double dx = (x + 0.5) - cx;
                const double dy = (y + 0.5) - cy;
                const double cov =
                    std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5, 0.0, 1.0);

                unsigned char& a = alpha[y * w + x];
                a = (unsigned char)(a * cov);
            }
        }
        return wxBitmap(img);
    }

    wxRect CloseHitBox() const
    {
        const wxSize sz = GetClientSize();
        const int cx = sz.x - m_closeR - FromDIP(4);
        const int cy = m_closeR + FromDIP(4);
        const int pad = FromDIP(2);   // forgiving target for small badges
        return wxRect(cx - m_closeR - pad, cy - m_closeR - pad,
                      (m_closeR + pad) * 2, (m_closeR + pad) * 2);
    }

    // ── Events ────────────────────────────────────────────────────

    void OnEnter(wxMouseEvent& e)
    {
        m_hover = true;
        ApplyCursor();
        Refresh();
        e.Skip();
    }

    void OnLeave(wxMouseEvent& e)
    {
        m_hover = m_hoverClose = false;
        ApplyCursor();
        Refresh();
        e.Skip();
    }

    void OnMotion(wxMouseEvent& e)
    {
        const bool overClose = CloseHitBox().Contains(e.GetPosition());
        bool changed = false;
        if (overClose != m_hoverClose) { m_hoverClose = overClose; changed = true; }
        if (!m_hover)                  { m_hover = true;           changed = true; }
        if (changed) {
            ApplyCursor();
            Refresh();
        }
        e.Skip();
    }

    void OnLeftUp(wxMouseEvent& e)
    {
        // Both callbacks run via CallAfter on the parent strip:
        //  • remove rebuilds the strip, destroying this chip;
        //  • open runs a modal lightbox (nested event loop).
        // Either way this handler must unwind first.  Only the callback
        // and index are captured, never `this`.
        const auto idx = m_index;

        if (CloseHitBox().Contains(e.GetPosition())) {
            auto fn = m_onRemove;
            GetParent()->CallAfter([fn, idx]() { if (fn) fn(idx); });
            return;
        }

        if (CanOpen() && GetClientRect().Contains(e.GetPosition())) {
            auto fn = m_onOpen;
            // The lightbox appears under the pointer; drop the hover
            // state now so the card doesn't stay lit behind the scrim.
            m_hover = m_hoverClose = false;
            ApplyCursor();
            Refresh();
            GetParent()->CallAfter([fn, idx]() { if (fn) fn(idx); });
            return;
        }

        e.Skip();
    }

    // ── Paint ─────────────────────────────────────────────────────

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        const wxSize sz = GetClientSize();
        const double w = sz.x, h = sz.y;
        const double r = m_radius;

        // Card body
        gc->SetBrush(wxBrush(m_hover ? m_surfaceHover : m_surface));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(0, 0, w, h, r);

        if (m_model.isImage && m_thumb.IsOk())
            PaintImageCard(*gc, dc, w, h);
        else
            PaintFileCard(*gc, dc, w, h);

        // Border last so it sits over the thumbnail edge
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(wxPen(m_hover ? m_borderHover : m_border, 1));
        gc->DrawRoundedRectangle(0.5, 0.5, w - 1, h - 1, r);

        PaintCloseBadge(*gc);
    }

    void PaintImageCard(wxGraphicsContext& gc, wxDC& dc, double w, double h)
    {
        gc.DrawBitmap(m_thumb, 0, 0, w, h);

        // Clickable hint: a faint veil over the picture while hovered
        // (not while the pointer is on the × badge — that click removes).
        if (CanOpen() && m_hover && !m_hoverClose) {
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(wxColour(255, 255, 255, 28)));
            gc.DrawRoundedRectangle(0, 0, w, h, m_radius);
        }

        // Bottom scrim + filename, so the card is identifiable without a
        // hover.  Four stacked bands fake a gradient — cheap, and it
        // avoids a gradient brush that some wx backends render harshly.
        const double bandH = FromDIP(19);
        const double top   = h - bandH;
        gc.SetPen(*wxTRANSPARENT_PEN);
        for (int i = 0; i < 4; ++i) {
            const double y0 = top + (bandH / 4.0) * i;
            gc.SetBrush(wxBrush(wxColour(0, 0, 0,
                                (unsigned char)(70 + i * 40))));
            gc.DrawRectangle(1, y0, w - 2, bandH / 4.0 + 1);
        }

        wxFont f = GetFont();
        f.SetPointSize(std::max(7, f.GetPointSize() - 2));
        dc.SetFont(f);

        const wxString label = wxControl::Ellipsize(
            wxString::FromUTF8(m_model.name), dc, wxELLIPSIZE_MIDDLE,
            (int)(w - FromDIP(10)), wxELLIPSIZE_FLAGS_NONE);

        gc.SetFont(f, wxColour(240, 240, 244));
        double tw = 0, th = 0, desc = 0, lead = 0;
        gc.GetTextExtent(label, &tw, &th, &desc, &lead);

        // Centre the label in the scrim, but never let a tall font run
        // past the card's bottom edge -- the rounded corner would clip
        // the descenders.
        const double labelY = std::min(h - bandH + (bandH - th) / 2.0,
                                       h - th - FromDIP(2));
        gc.DrawText(label, (w - tw) / 2.0, labelY);
    }

    void PaintFileCard(wxGraphicsContext& gc, wxDC& dc, double w, double h)
    {
        const double pad  = FromDIP(kPadDip);
        const double tile = FromDIP(kTileDip);
        const double tileY = (h - tile) / 2.0;

        // Extension tile
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(m_accent));
        gc.DrawRoundedRectangle(pad, tileY, tile, tile, FromDIP(7));

        wxFont extFont = GetFont();
        extFont.SetPointSize(std::max(6, extFont.GetPointSize() - 2));
        extFont.SetWeight(wxFONTWEIGHT_BOLD);
        gc.SetFont(extFont, m_tileText);

        const wxString ext = wxString::FromUTF8(Extension(m_model.name));
        double ew = 0, eh = 0, ed = 0, el = 0;
        gc.GetTextExtent(ext, &ew, &eh, &ed, &el);
        gc.DrawText(ext, pad + (tile - ew) / 2.0, tileY + (tile - eh) / 2.0);

        // Filename + meta line
        const double textX = pad + tile + FromDIP(9);
        const double textW = w - textX - m_closeR * 2 - FromDIP(6);

        wxFont nameFont = GetFont();
        nameFont.SetPointSize(std::max(8, nameFont.GetPointSize() - 1));
        nameFont.SetWeight(wxFONTWEIGHT_MEDIUM);
        dc.SetFont(nameFont);
        const wxString name = wxControl::Ellipsize(
            wxString::FromUTF8(m_model.name), dc, wxELLIPSIZE_MIDDLE,
            (int)std::max(20.0, textW), wxELLIPSIZE_FLAGS_NONE);

        wxFont metaFont = GetFont();
        metaFont.SetPointSize(std::max(7, metaFont.GetPointSize() - 2));

        wxString meta = wxString::FromUTF8(m_model.kindLabel);
        const wxString bytes = HumanBytes(m_model.byteSize);
        if (!bytes.empty())
            meta += meta.empty() ? bytes
                                 : wxString::FromUTF8("  \xC2\xB7  ") + bytes;

        double nw = 0, nh = 0, nd = 0, nl = 0;
        gc.SetFont(nameFont, m_text);
        gc.GetTextExtent(name, &nw, &nh, &nd, &nl);

        double mw = 0, mh = 0, md = 0, ml = 0;
        gc.SetFont(metaFont, m_meta);
        gc.GetTextExtent(meta.IsEmpty() ? wxString("X") : meta,
                         &mw, &mh, &md, &ml);

        const double gap   = FromDIP(3);
        const double block = nh + (meta.IsEmpty() ? 0 : gap + mh);
        const double top   = (h - block) / 2.0;

        gc.SetFont(nameFont, m_text);
        gc.DrawText(name, textX, top);

        if (!meta.IsEmpty()) {
            gc.SetFont(metaFont, m_meta);
            gc.DrawText(meta, textX, top + nh + gap);
        }
    }

    void PaintCloseBadge(wxGraphicsContext& gc)
    {
        const wxSize sz = GetClientSize();
        const double cx = sz.x - m_closeR - FromDIP(4);
        const double cy = m_closeR + FromDIP(4);

        // At rest the badge is subdued so the strip stays calm; it only
        // resolves into a solid button once the pointer is on the card.
        const double bgAlpha = m_hover ? 1.0 : 0.55;
        wxColour bg = m_hoverClose ? m_closeBgHover : m_closeBg;
        bg = wxColour(bg.Red(), bg.Green(), bg.Blue(),
                      (unsigned char)(255 * bgAlpha));

        gc.SetPen(wxPen(wxColour(255, 255, 255, m_hover ? 60 : 30), 1));
        gc.SetBrush(wxBrush(bg));
        gc.DrawEllipse(cx - m_closeR, cy - m_closeR, m_closeR * 2, m_closeR * 2);

        const double a = m_closeR * 0.42;
        gc.SetPen(wxPen(wxColour(m_closeGlyph.Red(), m_closeGlyph.Green(),
                                 m_closeGlyph.Blue(),
                                 (unsigned char)(255 * (m_hover ? 1.0 : 0.75))),
                        FromDIP(2)));
        gc.StrokeLine(cx - a, cy - a, cx + a, cy + a);
        gc.StrokeLine(cx + a, cy - a, cx - a, cy + a);
    }
};
