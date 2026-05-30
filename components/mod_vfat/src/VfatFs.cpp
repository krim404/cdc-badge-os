/**
 * \file VfatFs.cpp
 * \brief Implementation of the plugins-partition file helper.
 */

#include "VfatFs.h"

#include "plugin_manager/PluginStorage.h"
#include "cdc_core/Raii.h"
#include "cdc_log.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <algorithm>

namespace cdc::mod_vfat {
namespace fs {

namespace {
constexpr const char* TAG = "VFAT";
}

const char* root()
{
    return cdc::plugin_manager::PluginStorage::basePath();
}

bool resolve(const std::string& relPath, std::string& absOut)
{
    if (!relPath.empty() && relPath.front() == '/') return false;
    // Reject any ".." component so a relative path can never escape root.
    size_t start = 0;
    while (start <= relPath.size()) {
        size_t slash = relPath.find('/', start);
        std::string seg = relPath.substr(start, slash == std::string::npos
                                                     ? std::string::npos
                                                     : slash - start);
        if (seg == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    absOut = relPath.empty() ? std::string(root())
                             : std::string(root()) + "/" + relPath;
    return true;
}

bool list(const std::string& relDir, std::vector<FsEntry>& out, bool& truncated)
{
    truncated = false;
    out.clear();

    std::string abs;
    if (!resolve(relDir, abs)) return false;

    DIR* d = opendir(abs.c_str());
    if (!d) return false;

    for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
        if (e->d_name[0] == '.') continue;  // skip "." and ".."
        if (out.size() >= MAX_LIST_ENTRIES) { truncated = true; break; }

        FsEntry entry;
        entry.name = e->d_name;

        struct stat st;
        std::string child = abs + "/" + e->d_name;
        if (stat(child.c_str(), &st) == 0) {
            entry.is_dir = S_ISDIR(st.st_mode);
            entry.size   = static_cast<uint32_t>(st.st_size);
        }
        out.push_back(std::move(entry));
    }
    closedir(d);

    // Directories first, then files; alphabetical within each group.
    std::sort(out.begin(), out.end(), [](const FsEntry& a, const FsEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
    return true;
}

bool readText(const std::string& relFile, std::string& out, size_t maxBytes)
{
    std::string abs;
    if (!resolve(relFile, abs)) return false;

    auto f = cdc::core::openFile(abs.c_str(), "rb");
    if (!f) return false;

    out.clear();
    out.resize(maxBytes);
    size_t n = std::fread(&out[0], 1, maxBytes, f.get());
    out.resize(n);
    return true;
}

bool writeText(const std::string& relFile, const char* data, size_t len)
{
    std::string abs;
    if (!resolve(relFile, abs)) return false;

    auto f = cdc::core::openFile(abs.c_str(), "wb");
    if (!f) return false;
    if (len > 0 && std::fwrite(data, 1, len, f.get()) != len) {
        LOG_W(TAG, "short write to %s", relFile.c_str());
        return false;
    }
    return true;
}

bool removeFile(const std::string& relFile)
{
    std::string abs;
    if (!resolve(relFile, abs)) return false;
    return std::remove(abs.c_str()) == 0;
}

bool makeDir(const std::string& relDir)
{
    std::string abs;
    if (!resolve(relDir, abs) || abs == std::string(root())) return false;
    return mkdir(abs.c_str(), 0755) == 0;
}

bool removeDir(const std::string& relDir)
{
    std::string abs;
    if (!resolve(relDir, abs) || abs == std::string(root())) return false;
    return rmdir(abs.c_str()) == 0;
}

bool isDir(const std::string& relPath)
{
    std::string abs;
    if (!resolve(relPath, abs)) return false;
    struct stat st;
    return stat(abs.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool exists(const std::string& relPath)
{
    std::string abs;
    if (!resolve(relPath, abs)) return false;
    struct stat st;
    return stat(abs.c_str(), &st) == 0;
}

bool stats(uint32_t& totalKB, uint32_t& freeKB)
{
    uint64_t freeB = 0, totalB = 0;
    if (!cdc::plugin_manager::PluginStorage::stats(freeB, totalB)) return false;
    totalKB = static_cast<uint32_t>(totalB / 1024);
    freeKB  = static_cast<uint32_t>(freeB / 1024);
    return true;
}

}  // namespace fs
}  // namespace cdc::mod_vfat
