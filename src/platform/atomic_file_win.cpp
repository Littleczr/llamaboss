// atomic_file_win.cpp — Windows atomic write (MoveFileExW)
#include "atomic_file.h"
#include "str_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <random>

namespace atomic_file {

static std::string MakeTempName(std::string_view finalPath) {
    // finalPath.tmp.<random>
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    std::string tmp = std::string(finalPath) + ".tmp." + std::to_string(dis(gen));
    return tmp;
}

bool Write(std::string_view finalPath, std::string_view content, bool overwrite) {
    std::wstring wFinal = str_util::Utf8ToWide(finalPath);
    if (wFinal.empty()) return false;

    std::string tmpPath = MakeTempName(finalPath);
    std::wstring wTmp = str_util::Utf8ToWide(tmpPath);
    if (wTmp.empty()) return false;

    // Write to temp file
    HANDLE hFile = CreateFileW(wTmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    const char* data = content.data();
    size_t remain = content.size();
    while (remain > 0) {
        DWORD chunk = (remain > 0x40000000U) ? 0x40000000U : static_cast<DWORD>(remain);
        DWORD written = 0;
        if (!WriteFile(hFile, data, chunk, &written, nullptr) || written == 0) {
            CloseHandle(hFile);
            DeleteFileW(wTmp.c_str());
            return false;
        }
        data += written;
        remain -= written;
    }
    CloseHandle(hFile);

    // Atomic rename
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (overwrite) flags |= MOVEFILE_REPLACE_EXISTING;

    if (!MoveFileExW(wTmp.c_str(), wFinal.c_str(), flags)) {
        DeleteFileW(wTmp.c_str());
        return false;
    }
    return true;
}

} // namespace atomic_file