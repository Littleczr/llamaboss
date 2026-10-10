// atomic_file_posix.cpp — POSIX atomic write (link + rename)
#include "atomic_file.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <random>
#include <string>
#include <cerrno>

namespace atomic_file {

static std::string MakeTempName(std::string_view finalPath) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    std::string tmp = std::string(finalPath) + ".tmp." + std::to_string(dis(gen));
    return tmp;
}

bool Write(std::string_view finalPath, std::string_view content, bool overwrite) {
    std::string tmpPath = MakeTempName(finalPath);

    // Write to temp file (O_EXCL ensures we don't overwrite existing)
    int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    const char* data = content.data();
    size_t remain = content.size();
    while (remain > 0) {
        ssize_t written = write(fd, data, remain);
        if (written <= 0) {
            close(fd);
            unlink(tmpPath.c_str());
            return false;
        }
        data += written;
        remain -= written;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmpPath.c_str());
        return false;
    }
    close(fd);

    // Atomic rename: link + unlink for overwrite, plain rename for create-new
    if (overwrite) {
        // link() creates a hard link, then unlink() removes the old name
        // This is atomic on POSIX
        if (link(tmpPath.c_str(), finalPath.data()) != 0) {
            // If link fails because file exists, try unlink + link
            if (errno == EEXIST) {
                unlink(finalPath.data());
                if (link(tmpPath.c_str(), finalPath.data()) != 0) {
                    unlink(tmpPath.c_str());
                    return false;
                }
            } else {
                unlink(tmpPath.c_str());
                return false;
            }
        }
        unlink(tmpPath.c_str());
    } else {
        // Create-new: rename fails if target exists
        if (rename(tmpPath.c_str(), finalPath.data()) != 0) {
            unlink(tmpPath.c_str());
            return false;
        }
    }
    return true;
}

} // namespace atomic_file