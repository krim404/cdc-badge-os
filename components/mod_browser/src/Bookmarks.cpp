#include "Bookmarks.h"

#include "cdc_core/Raii.h"
#include "cdc_log.h"

#include "nvs.h"

#include <cstring>

namespace cdc::browser {
namespace {

static constexpr const char* TAG = "BROWSER-BM";
static constexpr const char* kNs = "browser";
static constexpr const char* kKey = "bm";
static constexpr uint32_t kMagic = 0x424D4B31;  // "BMK1"

struct BmBlob {
    uint32_t magic;
    uint8_t  count;
    Bookmark items[kMaxBookmarks];
};

void copyStr(char* dst, const char* src, size_t cap)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t n = std::strlen(src);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

}  // namespace

Bookmarks& Bookmarks::instance()
{
    static Bookmarks inst;
    return inst;
}

void Bookmarks::load()
{
    count_ = 0;
    cdc::core::NvsScope nvs(kNs, NVS_READONLY);
    if (!nvs) return;
    auto buf = cdc::core::psramAlloc<BmBlob>(1);
    if (!buf) return;
    BmBlob* blob = buf.get();
    size_t sz = sizeof(BmBlob);
    if (nvs_get_blob(nvs, kKey, blob, &sz) != ESP_OK) return;
    if (sz != sizeof(BmBlob) || blob->magic != kMagic) return;
    uint8_t n = blob->count <= kMaxBookmarks ? blob->count : kMaxBookmarks;
    for (uint8_t i = 0; i < n; ++i) items_[i] = blob->items[i];
    count_ = n;
}

void Bookmarks::save() const
{
    cdc::core::NvsScope nvs(kNs, NVS_READWRITE);
    if (!nvs) {
        LOG_W(TAG, "nvs open failed");
        return;
    }
    auto buf = cdc::core::psramAlloc<BmBlob>(1);
    if (!buf) return;
    BmBlob* blob = buf.get();
    std::memset(blob, 0, sizeof(BmBlob));
    blob->magic = kMagic;
    blob->count = count_;
    for (uint8_t i = 0; i < count_; ++i) blob->items[i] = items_[i];
    if (nvs_set_blob(nvs, kKey, blob, sizeof(BmBlob)) == ESP_OK) nvs.commit();
}

bool Bookmarks::contains(const char* url) const
{
    if (!url) return false;
    for (uint8_t i = 0; i < count_; ++i) {
        if (std::strcmp(items_[i].url, url) == 0) return true;
    }
    return false;
}

bool Bookmarks::add(const char* url, const char* label)
{
    if (!url || !url[0]) return false;
    // Drop an existing duplicate so the entry moves to the front.
    for (uint8_t i = 0; i < count_; ++i) {
        if (std::strcmp(items_[i].url, url) == 0) {
            for (uint8_t j = i; j + 1 < count_; ++j) items_[j] = items_[j + 1];
            --count_;
            break;
        }
    }
    uint8_t keep = count_ < kMaxBookmarks ? count_ : (kMaxBookmarks - 1);
    for (int j = keep; j > 0; --j) items_[j] = items_[j - 1];
    copyStr(items_[0].url, url, kUrlCap);
    copyStr(items_[0].label, (label && label[0]) ? label : url, kLabelCap);
    if (count_ < kMaxBookmarks) ++count_;
    save();
    return true;
}

bool Bookmarks::removeAt(uint8_t index)
{
    if (index >= count_) return false;
    for (uint8_t j = index; j + 1 < count_; ++j) items_[j] = items_[j + 1];
    --count_;
    save();
    return true;
}

}  // namespace cdc::browser
