// ascii_animation.h
// Extensible ASCII animation framework for LlamaBoss Easter eggs.
// Add new animations by subclassing AsciiAnimation.
#pragma once

#include <wx/colour.h>
#include <vector>
#include <string>
#include <cmath>
#include <random>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════
//  Core types
// ═══════════════════════════════════════════════════════════════════

struct ColoredChar {
    char     ch    = ' ';
    wxColour color = wxColour(100, 100, 100);
};

using AnimationFrame = std::vector<std::vector<ColoredChar>>;

// ═══════════════════════════════════════════════════════════════════
//  Base class — subclass this for new animations
// ═══════════════════════════════════════════════════════════════════

class AsciiAnimation {
public:
    virtual ~AsciiAnimation() = default;

    virtual void  Reset() = 0;
    virtual bool  Tick()   = 0;  // advance one frame; returns false when done
    virtual AnimationFrame GetFrame() const = 0;

    virtual int GetWidth()  const = 0;
    virtual int GetHeight() const = 0;

    // Suggested timer interval in milliseconds
    virtual int GetIntervalMs() const { return 100; }
};

// ═══════════════════════════════════════════════════════════════════
//  /yay!  —  ASCII Fireworks
// ═══════════════════════════════════════════════════════════════════

class FireworksAnimation : public AsciiAnimation {
public:
    static constexpr int W = 60;
    static constexpr int H = 20;

    FireworksAnimation() { Reset(); }

    void Reset() override
    {
        m_tick = 0;
        m_rockets.clear();
        m_particles.clear();
        ClearGrid();

        // Stagger a few initial launches
        ScheduleLaunch(0,  W * 0.30);
        ScheduleLaunch(6,  W * 0.55);
        ScheduleLaunch(14, W * 0.75);
        ScheduleLaunch(25, W * 0.20);
        ScheduleLaunch(35, W * 0.60);
        ScheduleLaunch(45, W * 0.40);
        ScheduleLaunch(52, W * 0.80);
    }

    bool Tick() override
    {
        if (m_tick >= kMaxTicks) return false;

        ClearGrid();

        // Launch scheduled rockets
        for (auto& s : m_scheduled) {
            if (s.tick == m_tick)
                LaunchRocket(s.x);
        }

        // Update rockets
        for (int i = static_cast<int>(m_rockets.size()) - 1; i >= 0; --i) {
            auto& r = m_rockets[i];
            r.y += r.vy;
            r.x += RandF(-0.2, 0.2);

            // Draw trail
            int rx = Clamp(static_cast<int>(r.x), 0, W - 1);
            int ry = Clamp(static_cast<int>(r.y), 0, H - 1);
            int ty = std::min(ry + 1, H - 1);
            int ty2 = std::min(ry + 2, H - 1);
            Plot(rx, ry, '^', kYellow);
            Plot(rx, ty, '|', wxColour(180, 150, 50));
            Plot(rx, ty2, '.', wxColour(120, 100, 40));

            if (r.y <= r.targetY) {
                Explode(r.x, r.y, r.color);
                m_rockets.erase(m_rockets.begin() + i);
            }
        }

        // Update particles
        for (int i = static_cast<int>(m_particles.size()) - 1; i >= 0; --i) {
            auto& p = m_particles[i];
            p.x  += p.vx;
            p.y  += p.vy;
            p.vy += p.gravity;
            p.vx *= 0.97;
            p.life--;

            if (p.life <= 0) {
                m_particles.erase(m_particles.begin() + i);
                continue;
            }

            int px = static_cast<int>(std::round(p.x));
            int py = static_cast<int>(std::round(p.y));
            if (px < 0 || px >= W || py < 0 || py >= H) continue;

            double fade = static_cast<double>(p.life) / p.maxLife;
            char ch = (fade < 0.15) ? '.' : (fade < 0.35) ? '+' : p.ch;

            // Dim the color as particle fades
            wxColour c = p.color;
            if (fade < 0.5) {
                int r = static_cast<int>(c.Red()   * fade * 1.6);
                int g = static_cast<int>(c.Green() * fade * 1.6);
                int b = static_cast<int>(c.Blue()  * fade * 1.6);
                c = wxColour(Clamp(r, 0, 255), Clamp(g, 0, 255), Clamp(b, 0, 255));
            }
            Plot(px, py, ch, c);
        }

        ++m_tick;
        return true;
    }

    AnimationFrame GetFrame() const override
    {
        return m_grid;
    }

    int GetWidth()      const override { return W; }
    int GetHeight()     const override { return H; }
    int GetIntervalMs() const override { return 100; }  // 10 fps

private:
    // ── Palette ──────────────────────────────────────────────────
    // Saturated colors that pop on both dark and light backgrounds
    static inline const wxColour kCyan    {0, 210, 220};
    static inline const wxColour kGreen   {50, 210, 50};
    static inline const wxColour kMagenta {210, 70, 250};
    static inline const wxColour kYellow  {255, 220, 50};
    static inline const wxColour kRed     {255, 75, 60};
    static inline const wxColour kBlue    {60, 150, 255};

    static inline const wxColour kPalette[] = {
        kCyan, kGreen, kMagenta, kYellow, kRed, kBlue
    };
    static constexpr int kPaletteSize = 6;

    static constexpr const char kChars[] = {'*', 'x', 'o', '@', '+', 'X', 'O'};
    static constexpr int kCharsSize = 7;

    static constexpr int kMaxTicks = 70;  // ~7 seconds at 10 fps

    // ── Structs ──────────────────────────────────────────────────
    struct Rocket {
        double x, y, vy, targetY;
        wxColour color;
    };

    struct Particle {
        double x, y, vx, vy, gravity;
        int    life, maxLife;
        char   ch;
        wxColour color;
    };

    struct ScheduledLaunch {
        int    tick;
        double x;
    };

    // ── State ────────────────────────────────────────────────────
    int m_tick = 0;
    AnimationFrame m_grid;
    std::vector<Rocket>          m_rockets;
    std::vector<Particle>        m_particles;
    std::vector<ScheduledLaunch> m_scheduled;
    std::mt19937 m_rng{std::random_device{}()};

    // ── Helpers ──────────────────────────────────────────────────
    void ClearGrid()
    {
        m_grid.assign(H, std::vector<ColoredChar>(W, {' ', wxColour(60, 60, 60)}));
    }

    void Plot(int x, int y, char ch, const wxColour& color)
    {
        if (x >= 0 && x < W && y >= 0 && y < H) {
            m_grid[y][x] = {ch, color};
        }
    }

    double RandF(double lo, double hi)
    {
        std::uniform_real_distribution<double> dist(lo, hi);
        return dist(m_rng);
    }

    int RandI(int lo, int hi)
    {
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(m_rng);
    }

    static int Clamp(int v, int lo, int hi)
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    void ScheduleLaunch(int tick, double x)
    {
        m_scheduled.push_back({tick, x});
    }

    void LaunchRocket(double x)
    {
        Rocket r;
        r.x       = x;
        r.y       = H - 1;
        r.vy      = -(1.2 + RandF(0.0, 0.8));
        r.targetY = 2 + RandF(0.0, H * 0.35);
        r.color   = kPalette[RandI(0, kPaletteSize - 1)];
        m_rockets.push_back(r);
    }

    void Explode(double cx, double cy, const wxColour& baseColor)
    {
        int count = 18 + RandI(0, 16);
        for (int i = 0; i < count; ++i) {
            double angle = RandF(0.0, 2.0 * 3.14159265);
            double speed = RandF(0.3, 1.4);

            // 30% chance of a different color for variety
            wxColour c = (RandF(0, 1) > 0.7)
                       ? kPalette[RandI(0, kPaletteSize - 1)]
                       : baseColor;

            Particle p;
            p.x       = cx;
            p.y       = cy;
            p.vx      = std::cos(angle) * speed * 1.6;
            p.vy      = std::sin(angle) * speed;
            p.gravity = RandF(0.02, 0.05);
            p.life    = 12 + RandI(0, 16);
            p.maxLife  = 30;
            p.ch      = kChars[RandI(0, kCharsSize - 1)];
            p.color   = c;
            m_particles.push_back(p);
        }
    }
};
