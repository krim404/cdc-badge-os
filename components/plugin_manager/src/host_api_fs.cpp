/**
 * \file host_api_fs.cpp
 * \brief Sandboxed file storage for plugins on the plugins FAT partition.
 *
 * Each plugin gets a private directory `<plugins>/data/<id>/` derived from the
 * active plugin's id. The plugin only ever passes a bare filename; the host
 * builds the full path and rejects anything that could escape the folder, so
 * cross-plugin and out-of-sandbox access is physically impossible. Gated on
 * the `vfat` capability.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginStorage.h"
#include "cdc_core/Raii.h"
#include "cdc_views/ImageView.h"
#include "cdc_views/MarkdownView.h"
#include "host_str_conv.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <dirent.h>

extern "C" void* plg_get_active_plugin(void);

namespace {

constexpr size_t MAX_NAME_LEN = 64;

/// A bare filename: [A-Za-z0-9._-], no path separators, no leading dot.
bool nameOk(const char* name)
{
    if (!name || !*name) return false;
    size_t n = std::strlen(name);
    if (n > MAX_NAME_LEN || name[0] == '.') return false;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/// Resolve (and create) the active plugin's private data directory.
int resolveDir(std::string& out)
{
    auto* plugin = static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
    if (!plugin) return HOST_ERR_GENERIC;
    if (!plugin->manifest().capabilities.vfat) return HOST_ERR_NO_CAPABILITY;

    std::string dataRoot =
        std::string(cdc::plugin_manager::PluginStorage::basePath()) + "/data";
    mkdir(dataRoot.c_str(), 0755);
    out = dataRoot + "/" + plugin->id();
    mkdir(out.c_str(), 0755);
    return HOST_OK;
}

/// Resolve a sandboxed full path for `name`. Capability is checked first.
int resolvePath(const char* name, std::string& out)
{
    std::string dir;
    int rc = resolveDir(dir);
    if (rc != HOST_OK) return rc;
    if (!nameOk(name)) return HOST_ERR_INVALID_ARG;
    out = dir + "/" + name;
    return HOST_OK;
}

}  // namespace

extern "C" {

int host_fs_write(const char* name, const uint8_t* data, size_t len)
{
    if (!data && len > 0) return HOST_ERR_INVALID_ARG;
    std::string path;
    int rc = resolvePath(name, path);
    if (rc != HOST_OK) return rc;

    auto f = cdc::core::openFile(path.c_str(), "wb");
    if (!f) return HOST_ERR_GENERIC;
    if (len > 0 && std::fwrite(data, 1, len, f.get()) != len) return HOST_ERR_GENERIC;
    return HOST_OK;
}

int host_fs_read(const char* name, uint8_t* buf, size_t* len)
{
    if (!buf || !len) return HOST_ERR_INVALID_ARG;
    std::string path;
    int rc = resolvePath(name, path);
    if (rc != HOST_OK) return rc;

    auto f = cdc::core::openFile(path.c_str(), "rb");
    if (!f) return HOST_ERR_NOT_FOUND;
    *len = std::fread(buf, 1, *len, f.get());
    return HOST_OK;
}

int host_fs_remove(const char* name)
{
    std::string path;
    int rc = resolvePath(name, path);
    if (rc != HOST_OK) return rc;
    return (std::remove(path.c_str()) == 0) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_fs_size(const char* name, size_t* out)
{
    if (!out) return HOST_ERR_INVALID_ARG;
    std::string path;
    int rc = resolvePath(name, path);
    if (rc != HOST_OK) return rc;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) return HOST_ERR_NOT_FOUND;
    *out = static_cast<size_t>(st.st_size);
    return HOST_OK;
}

int host_fs_list(char* out, size_t* out_len)
{
    if (!out_len) return HOST_ERR_INVALID_ARG;
    std::string dir;
    int rc = resolveDir(dir);
    if (rc != HOST_OK) return rc;

    DIR* d = opendir(dir.c_str());
    if (!d) return HOST_ERR_NOT_FOUND;

    size_t written = 0;
    const size_t max = (out ? *out_len : 0);
    for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        size_t need = std::strlen(e->d_name) + 1;
        if (out && (written + need) <= max) {
            std::memcpy(out + written, e->d_name, need - 1);
            out[written + need - 1] = '\n';
        }
        written += need;
    }
    closedir(d);
    *out_len = written;
    return HOST_OK;
}

int host_fs_view(const char* name)
{
    std::string path;
    int rc = resolvePath(name, path);
    if (rc != HOST_OK) return rc;

    auto f = cdc::core::openFile(path.c_str(), "rb");
    if (!f) return HOST_ERR_NOT_FOUND;

    // Match the on-screen text viewer's capacity (InfoView buffer).
    constexpr size_t VIEW_MAX = 2048;
    std::string content;
    content.resize(VIEW_MAX);
    size_t n = std::fread(&content[0], 1, VIEW_MAX - 1, f.get());
    content.resize(n);
    // Files and filenames are UTF-8; host_ui_push_info converts to display.
    return host_ui_push_info(name, content.c_str());
}

int host_fs_view_image(const char* name)
{
    std::string path;
    int rc = resolvePath(name, path);
    if (rc != HOST_OK) return rc;

    auto f = cdc::core::openFile(path.c_str(), "rb");
    if (!f) return HOST_ERR_NOT_FOUND;

    constexpr size_t IMG_MAX = 512 * 1024;
    std::string buf;
    buf.resize(IMG_MAX);
    size_t n = std::fread(&buf[0], 1, IMG_MAX, f.get());
    buf.resize(n);

    std::string title = cdc::plugin_manager::toDisplay(name);
    cdc::ui::showImage(title.c_str(),
                       reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
    return HOST_OK;
}

int host_fs_view_markdown(const char* name)
{
    std::string path;
    int rc = resolvePath(name, path);
    if (rc != HOST_OK) return rc;

    auto f = cdc::core::openFile(path.c_str(), "rb");
    if (!f) return HOST_ERR_NOT_FOUND;

    constexpr size_t MD_MAX = 64 * 1024;
    std::string content;
    content.resize(MD_MAX);
    size_t n = std::fread(&content[0], 1, MD_MAX - 1, f.get());
    content.resize(n);

    std::string title = cdc::plugin_manager::toDisplay(name);
    std::string body = cdc::plugin_manager::toDisplay(content.c_str());
    cdc::ui::showMarkdown(title.c_str(), body.c_str(), body.size());
    return HOST_OK;
}

}  // extern "C"
