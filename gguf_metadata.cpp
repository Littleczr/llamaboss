// gguf_metadata.cpp
//
// Minimal GGUF v2/v3 metadata reader.  Walks the KV region of the
// header looking for `{arch}.nextn_predict_layers` and returns its
// integer value.  Everything else is skipped by type — including
// the tokenizer vocabulary arrays, which is why string arrays must
// be skipped element-by-element (each element carries its own u64
// length prefix; there is no aggregate byte count to seek past).
//
// GGUF layout (v2/v3, little-endian):
//   u32  magic 'GGUF' (0x46554747)
//   u32  version (2 or 3)
//   u64  tensor_count
//   u64  metadata_kv_count
//   then metadata_kv_count of:
//     string key            (u64 len + bytes)
//     u32    value_type
//     value                 (type-dependent)
//
// Value types:
//   0 u8   1 i8   2 u16   3 i16   4 u32   5 i32   6 f32
//   7 bool(1B)    8 string(u64+bytes)
//   9 array(u32 elem_type + u64 count + elems)
//   10 u64  11 i64  12 f64

#include "gguf_metadata.h"

#include <fstream>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// ── Sanity caps ─────────────────────────────────────────────────
// A corrupt or truncated file must not turn into a multi-gigabyte
// read loop on the launch path.  Real models sit far below all of
// these (typical kv_count is 25-60; the largest strings are chat
// templates at a few tens of KB; vocab arrays run to ~150k-300k
// elements).
constexpr uint64_t kMaxKvCount            = 4096;
constexpr uint64_t kMaxKeyLen             = 65535;             // GGUF spec limit
constexpr uint64_t kMaxStringLen          = 64ull * 1024 * 1024; // 64 MB
constexpr uint64_t kMaxFixedArrayCount    = 64ull * 1024 * 1024; // seek-only
constexpr uint64_t kMaxVariableArrayCount = 2ull * 1024 * 1024;  // walked entries
constexpr int      kMaxArrayDepth         = 4;                    // spec allows nesting

enum GgufType : uint32_t {
    GGUF_U8 = 0, GGUF_I8, GGUF_U16, GGUF_I16, GGUF_U32, GGUF_I32,
    GGUF_F32, GGUF_BOOL, GGUF_STRING, GGUF_ARRAY,
    GGUF_U64, GGUF_I64, GGUF_F64,
};

// Fixed on-disk size for scalar types; 0 = variable (string/array).
int ScalarSize(uint32_t t)
{
    switch (t) {
        case GGUF_U8: case GGUF_I8: case GGUF_BOOL:   return 1;
        case GGUF_U16: case GGUF_I16:                 return 2;
        case GGUF_U32: case GGUF_I32: case GGUF_F32:  return 4;
        case GGUF_U64: case GGUF_I64: case GGUF_F64:  return 8;
        default:                                      return 0;
    }
}

bool ReadRaw(std::ifstream& f, void* dst, size_t n)
{
    f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return f.good();
}

bool ReadU32(std::ifstream& f, uint32_t& v) { return ReadRaw(f, &v, 4); }
bool ReadU64(std::ifstream& f, uint64_t& v) { return ReadRaw(f, &v, 8); }

bool SkipBytes(std::ifstream& f, uint64_t n)
{
    f.seekg(static_cast<std::streamoff>(n), std::ios::cur);
    // seekg past EOF doesn't fail until the next read, so probe.
    return f.good() && f.peek() != std::char_traits<char>::eof();
}

// Skip one value of the given type.  `depth` guards nested arrays.
bool SkipValue(std::ifstream& f, uint32_t type, int depth)
{
    if (int sz = ScalarSize(type); sz > 0)
        return SkipBytes(f, static_cast<uint64_t>(sz));

    if (type == GGUF_STRING) {
        uint64_t len = 0;
        if (!ReadU64(f, len) || len > kMaxStringLen) return false;
        return len == 0 || SkipBytes(f, len);
    }

    if (type == GGUF_ARRAY) {
        if (depth >= kMaxArrayDepth) return false;
        uint32_t elemType = 0;
        uint64_t count    = 0;
        if (!ReadU32(f, elemType) || !ReadU64(f, count)) return false;

        if (int sz = ScalarSize(elemType); sz > 0) {
            if (count > kMaxFixedArrayCount) return false;
            return count == 0 ||
                   SkipBytes(f, count * static_cast<uint64_t>(sz));
        }

        // Variable-size elements (strings, nested arrays): no
        // aggregate length exists — walk them one by one.  Keep a
        // much tighter cap than fixed arrays so a corrupt file cannot
        // force millions of per-element reads on the launch thread.
        if (elemType != GGUF_STRING && elemType != GGUF_ARRAY) return false;
        if (count > kMaxVariableArrayCount) return false;
        for (uint64_t i = 0; i < count; ++i)
            if (!SkipValue(f, elemType, depth + 1)) return false;
        return true;
    }

    return false;   // unknown type → treat file as unparseable
}

// Read an integer-typed value into `out`.  Returns false for
// non-integer types (the caller then falls back to SkipValue).
bool ReadIntValue(std::ifstream& f, uint32_t type, int64_t& out)
{
    switch (type) {
        case GGUF_U8:  { uint8_t  v; if (!ReadRaw(f, &v, 1)) return false; out = v; return true; }
        case GGUF_I8:  { int8_t   v; if (!ReadRaw(f, &v, 1)) return false; out = v; return true; }
        case GGUF_U16: { uint16_t v; if (!ReadRaw(f, &v, 2)) return false; out = v; return true; }
        case GGUF_I16: { int16_t  v; if (!ReadRaw(f, &v, 2)) return false; out = v; return true; }
        case GGUF_U32: { uint32_t v; if (!ReadRaw(f, &v, 4)) return false; out = v; return true; }
        case GGUF_I32: { int32_t  v; if (!ReadRaw(f, &v, 4)) return false; out = v; return true; }
        case GGUF_U64: { uint64_t v; if (!ReadRaw(f, &v, 8)) return false; out = static_cast<int64_t>(v); return true; }
        case GGUF_I64: { int64_t  v; if (!ReadRaw(f, &v, 8)) return false; out = v; return true; }
        default: return false;
    }
}

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n &&
           std::memcmp(s.data() + s.size() - n, suffix, n) == 0;
}

std::ifstream OpenBinary(const std::string& pathUtf8)
{
#ifdef _WIN32
    // Widen so non-ASCII model folders open correctly — the narrow
    // CRT open would route UTF-8 bytes through the ANSI code page.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), -1,
                                   nullptr, 0);
    if (wlen <= 0) return std::ifstream();
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), -1,
                        wpath.data(), wlen);
    wpath.resize(static_cast<size_t>(wlen) - 1);   // drop NUL
    return std::ifstream(wpath.c_str(), std::ios::binary);
#else
    return std::ifstream(pathUtf8.c_str(), std::ios::binary);
#endif
}

} // namespace

int GgufNextnPredictLayers(const std::string& ggufPathUtf8)
{
    std::ifstream f = OpenBinary(ggufPathUtf8);
    if (!f.is_open()) return 0;

    uint32_t magic = 0, version = 0;
    if (!ReadU32(f, magic) || magic != 0x46554747u) return 0;   // 'GGUF'
    if (!ReadU32(f, version) || version < 2 || version > 3) return 0;

    uint64_t tensorCount = 0, kvCount = 0;
    if (!ReadU64(f, tensorCount) || !ReadU64(f, kvCount)) return 0;
    if (kvCount > kMaxKvCount) return 0;

    std::string key;
    for (uint64_t i = 0; i < kvCount; ++i) {
        uint64_t keyLen = 0;
        if (!ReadU64(f, keyLen) || keyLen == 0 || keyLen > kMaxKeyLen) return 0;

        key.resize(static_cast<size_t>(keyLen));
        if (!ReadRaw(f, key.data(), static_cast<size_t>(keyLen))) return 0;

        uint32_t type = 0;
        if (!ReadU32(f, type)) return 0;

        if (EndsWith(key, ".nextn_predict_layers")) {
            int64_t v = 0;
            if (!ReadIntValue(f, type, v)) return 0;
            if (v < 0)  return 0;
            if (v > 64) return 0;   // implausible → distrust the file
            return static_cast<int>(v);
        }

        if (!SkipValue(f, type, 0)) return 0;
    }

    return 0;   // key absent — not an MTP model
}
