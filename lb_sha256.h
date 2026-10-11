// lb_sha256.h
//
// Self-contained SHA-256 (FIPS 180-4) for tool-side integrity checks:
// the hash and copy tools, web_fetch_url's response digest, and anything
// else that must report a digest the model can quote instead of retype.
//
// Portable on purpose (no BCrypt, no wx, no Poco) so the same code runs in
// LlamaBoss, in LlamaBossTests, and in off-Windows unit tests.  Throughput
// is a few hundred MB/s in Release, which is plenty for files a chat
// touches.  update_installer.cpp keeps its own BCrypt verifier; this header
// does not replace that security boundary.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace lb_sha256 {

class Hasher {
public:
    Hasher() { Reset(); }

    void Reset()
    {
        static const std::uint32_t kInit[8] = {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
        std::memcpy(m_state, kInit, sizeof(m_state));
        m_bufferLen = 0;
        m_totalBytes = 0;
    }

    void Update(const void* data, std::size_t len)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        m_totalBytes += len;
        if (m_bufferLen > 0) {
            const std::size_t take = (len < 64 - m_bufferLen) ? len : 64 - m_bufferLen;
            std::memcpy(m_buffer + m_bufferLen, p, take);
            m_bufferLen += take;
            p += take;
            len -= take;
            if (m_bufferLen == 64) {
                Compress(m_buffer);
                m_bufferLen = 0;
            }
        }
        while (len >= 64) {
            Compress(p);
            p += 64;
            len -= 64;
        }
        if (len > 0) {
            std::memcpy(m_buffer, p, len);
            m_bufferLen = len;
        }
    }

    void Update(const std::string& s) { Update(s.data(), s.size()); }

    // Lowercase 64-character hex digest.  Finalizes; call Reset() to reuse.
    std::string HexDigest()
    {
        const std::uint64_t bitLen = m_totalBytes * 8u;
        const unsigned char pad = 0x80;
        Update(&pad, 1);
        const unsigned char zero = 0;
        while (m_bufferLen != 56) Update(&zero, 1);
        unsigned char lenBytes[8];
        for (int i = 0; i < 8; ++i)
            lenBytes[i] = static_cast<unsigned char>(bitLen >> (56 - 8 * i));
        Update(lenBytes, 8);

        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (std::uint32_t word : m_state) {
            for (int shift = 28; shift >= 0; shift -= 4)
                out.push_back(kHex[(word >> shift) & 0xFu]);
        }
        return out;
    }

private:
    static std::uint32_t Rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Compress(const unsigned char* block)
    {
        static const std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u };

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(block[4 * i]) << 24) | (std::uint32_t(block[4 * i + 1]) << 16) |
                   (std::uint32_t(block[4 * i + 2]) << 8) | std::uint32_t(block[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
    }

    std::uint32_t m_state[8];
    unsigned char m_buffer[64];
    std::size_t   m_bufferLen = 0;
    std::uint64_t m_totalBytes = 0;
};

inline std::string HexOf(const std::string& bytes)
{
    Hasher h;
    h.Update(bytes);
    return h.HexDigest();
}

// True for exactly 64 hex characters (either case).
inline bool IsSha256Hex(const std::string& s)
{
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

inline std::string LowerHex(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

} // namespace lb_sha256
