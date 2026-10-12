// lb_mac_wording.h
//
// macOS only: Mac wording for user-facing text written for Windows PCs
// (Mac identity principle, MAC_PORT_STATUS.md).  Apple silicon has unified
// memory, not a separate VRAM pool, so GPU requirements are stated as the
// Mac memory size to look for.  macOS lets the GPU use about three quarters
// of unified memory, so "N GB VRAM" maps to the smallest common Mac memory
// size whose three quarters is at least N.
#pragma once

#ifdef __WXMAC__

#include <regex>
#include <string>

inline int LbMacMemoryTierForGpuGb(int gpuGb)
{
    static const int kTiers[] = { 8, 16, 24, 32, 48, 64, 96, 128, 192 };
    for (int tier : kTiers)
        if (tier * 3 >= gpuGb * 4) return tier;
    return 256;
}

// Rewrites the model catalog's hardware notes; other text passes through.
inline std::string LbMacWording(const std::string& text)
{
    std::string out = text;
    struct Fixed { const char* from; const char* to; };
    static const Fixed kFixed[] = {
        { "Runs on integrated GPUs and 8 GB laptops.",
          "Runs on any Apple silicon Mac, including 8 GB models." },
        { "Runs on CPU or minimal VRAM.", "Runs on any Apple silicon Mac." },
    };
    for (const Fixed& f : kFixed) {
        const auto pos = out.find(f.from);
        if (pos != std::string::npos) out.replace(pos, std::char_traits<char>::length(f.from), f.to);
    }

    static const std::regex kNeeds(R"((Needs|Requires) (\d+)\+ GB VRAM\.)");
    std::smatch m;
    if (std::regex_search(out, m, kNeeds)) {
        const int tier = LbMacMemoryTierForGpuGb(std::stoi(m[2].str()));
        out.replace(m.position(0), m.length(0),
                    m[1].str() + " a Mac with " + std::to_string(tier) + " GB+ of memory.");
    }
    return out;
}

#endif // __WXMAC__
