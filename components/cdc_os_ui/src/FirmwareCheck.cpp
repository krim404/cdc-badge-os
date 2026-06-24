#include "cdc_os_ui/FirmwareCheck.h"
#include "cdc_os_ui/WifiHandlers.h"

#include "cdc_views/InfoView.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_core/Raii.h"
#include "cdc_hal/IRtc.h"
#include "plugin_manager/host_api.h"
#include "cdc_log.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_attr.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace cdc::ui {
namespace {

static constexpr const char* TAG = "FWCHECK";

static constexpr const char* kReleaseApiUrl =
    "https://api.github.com/repos/krim404/cdc-badge-os/releases/latest";
static constexpr const char* kReleasesUrl = "github.com/krim404/cdc-badge-os/releases";

static constexpr uint32_t kHttpTimeoutMs = 8000;

// NVS namespace/keys for the last upstream check (also read by the VERSION
// serial command via firmwareCheckLastResult()).
static constexpr const char* kNvsNs      = "fwchk";
static constexpr const char* kNvsKeyVer  = "ver";
static constexpr const char* kNvsKeyDate = "date";

/// Display state shown on the screen and produced by the worker.
enum class Status : uint8_t {
    NeedsWifi,        ///< Opened (or connect failed) without WiFi; no check ran.
    Checking,         ///< Upstream query in flight.
    UpToDate,         ///< Running version is current.
    UpdateAvailable,  ///< A newer release exists (see s_foundTag).
    CheckFailed,      ///< Network/HTTP/parse error.
};

// Worker handshake: the UI task starts the worker, the worker fills s_result /
// s_foundTag and publishes them via the release-store on s_state. The UI task
// consumes the result in onTick() (acquire-load). Only one check runs at a time.
enum : uint8_t { kIdle = 0, kRunning = 1, kDone = 2 };
std::atomic<uint8_t> s_state{kIdle};
Status s_result = Status::CheckFailed;
char   s_foundTag[24] = {0};  ///< latest upstream tag, published by the worker
bool   s_heldWifi = false;    ///< UI task acquired WiFi for this check (must release)

// Only the HTTPS GET runs on this worker. It does NOT disable the flash cache,
// so its stack lives in PSRAM (internal RAM is scarce, especially with WiFi up),
// exactly like the browser fetch worker. The cache-disabling steps (WiFi
// connect, NTP sync, NVS store) run on the UI task instead, whose stack already
// lives in internal RAM. The response scan buffer is also PSRAM-resident.
constexpr size_t kWorkerStackBytes = 20u * 1024u;
EXT_RAM_BSS_ATTR StackType_t s_workerStack[kWorkerStackBytes / sizeof(StackType_t)];
StaticTask_t s_workerTcb;

constexpr size_t kRespCap = 16u * 1024u;
EXT_RAM_BSS_ATTR char s_respBuf[kRespCap];

class FirmwareCheckView : public InfoView {
public:
    void onTick(uint32_t nowMs) override;
};

FirmwareCheckView s_view;

/// \brief Strips a leading 'v'/'V' from a version tag.
const char* stripV(const char* v) {
    return (v && (*v == 'v' || *v == 'V')) ? v + 1 : v;
}

/// \brief Compares up to three dotted numeric version components.
/// \return 1 if a>b, -1 if a<b, 0 if equal across the compared components.
int compareSemver(const char* a, const char* b) {
    for (int i = 0; i < 3; ++i) {
        char* aEnd = nullptr;
        char* bEnd = nullptr;
        long na = std::strtol(a, &aEnd, 10);
        long nb = std::strtol(b, &bEnd, 10);
        if (na != nb) return (na > nb) ? 1 : -1;
        a = (*aEnd == '.') ? aEnd + 1 : aEnd;
        b = (*bEnd == '.') ? bEnd + 1 : bEnd;
    }
    return 0;
}

/// \brief Extracts the quoted value of the JSON "tag_name" field.
/// \return true if a non-empty value was copied into \p out.
bool parseTagName(const char* json, char* out, size_t cap) {
    const char* key = std::strstr(json, "\"tag_name\"");
    if (!key) return false;
    const char* p = key + std::strlen("\"tag_name\"");
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

/// \brief Composes "vX (checked: DATE)" (or just "vX" when no date) into \p out.
void composeLastResult(char* out, size_t cap, const char* ver, const char* date) {
    const char* vp = (ver[0] == 'v' || ver[0] == 'V') ? "" : "v";
    if (date && date[0]) {
        std::snprintf(out, cap, "%s%s (checked: %s)", vp, ver, date);
    } else {
        std::snprintf(out, cap, "%s%s", vp, ver);
    }
}

/// \brief Persists the latest upstream tag and the current date to NVS.
void storeLastCheck(const char* tag) {
    char date[12] = {0};
    auto* rtc = hal::getRtcInstance();
    if (rtc && rtc->isTimeSet()) {
        struct tm t{};
        rtc->getTime(&t);
        unsigned dd = static_cast<unsigned>(t.tm_mday) % 100u;
        unsigned mm = (static_cast<unsigned>(t.tm_mon) + 1u) % 100u;
        unsigned yy = static_cast<unsigned>(t.tm_year + 1900) % 100u;
        std::snprintf(date, sizeof(date), "%02u.%02u.%02u", dd, mm, yy);
    }
    core::NvsScope nvs(kNvsNs, NVS_READWRITE);
    if (nvs.status() != ESP_OK) return;
    nvs_set_str(nvs, kNvsKeyVer, tag);
    nvs_set_str(nvs, kNvsKeyDate, date);
    nvs.commit();
}

/// \brief Performs the upstream GitHub release query and version comparison.
/// \param foundTag Receives the latest upstream tag whenever one was parsed.
/// \return The resulting Status (UpToDate / UpdateAvailable / CheckFailed).
Status queryUpstream(char* foundTag, size_t cap) {
    foundTag[0] = '\0';
    esp_http_client_config_t cfg{};
    cfg.url = kReleaseApiUrl;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = static_cast<int>(kHttpTimeoutMs);
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return Status::CheckFailed;
    esp_http_client_set_header(h, "User-Agent", "cdc-badge-os");
    esp_http_client_set_header(h, "Accept", "application/vnd.github+json");

    Status result = Status::CheckFailed;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        if (esp_http_client_get_status_code(h) == 200) {
            size_t pos = 0;
            int n;
            while (pos < kRespCap - 1 &&
                   (n = esp_http_client_read(h, s_respBuf + pos, kRespCap - 1 - pos)) > 0) {
                pos += static_cast<size_t>(n);
            }
            s_respBuf[pos] = '\0';

            char tag[24];
            if (parseTagName(s_respBuf, tag, sizeof(tag))) {
                std::strncpy(foundTag, tag, cap - 1);
                foundTag[cap - 1] = '\0';
                result = (compareSemver(stripV(tag), APP_VERSION) > 0)
                             ? Status::UpdateAvailable
                             : Status::UpToDate;
            } else {
                LOG_W(TAG, "tag_name not found in response");
            }
        } else {
            LOG_W(TAG, "HTTP status %d", esp_http_client_get_status_code(h));
        }
    } else {
        LOG_W(TAG, "HTTP open failed");
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return result;
}

/// \brief Builds the screen text for \p st and refreshes the (current) view.
void renderStatus(Status st) {
    char buf[256];
    size_t pos = 0;
    auto append = [&](const char* fmt, ...) {
        if (pos >= sizeof(buf)) return;
        va_list args;
        va_start(args, fmt);
        int w = std::vsnprintf(buf + pos, sizeof(buf) - pos, fmt, args);
        va_end(args);
        if (w > 0) {
            size_t ww = static_cast<size_t>(w);
            pos += (ww < sizeof(buf) - pos) ? ww : (sizeof(buf) - pos - 1);
        }
    };

    append("Firmware: %s\n%s: %s\n", APP_VERSION, ui::tr("core.fw_api_level"),
           HOST_API_LEVEL_STR);
    char last[64];
    if (firmwareCheckLastResult(last, sizeof(last))) append("%s\n", last);
    append("\n");

    switch (st) {
        case Status::NeedsWifi:
            append("%s", ui::tr("core.fw_needs_wifi"));
            break;
        case Status::Checking:
            append("%s", ui::tr("core.fw_checking"));
            break;
        case Status::UpToDate:
            append("%s", ui::tr("core.fw_up_to_date"));
            break;
        case Status::UpdateAvailable:
            append("%s %s\n%s", ui::tr("core.fw_update_available"), s_foundTag, kReleasesUrl);
            break;
        case Status::CheckFailed:
            append("%s", ui::tr("core.fw_check_failed"));
            break;
    }

    s_view.init(ui::tr("core.firmware_check"), buf);
    s_view.setHint(ui::tr("core.fw_recheck_hint"));
    if (ViewStack::instance().current() == &s_view) {
        ViewStack::instance().render();
    }
}

/// \brief Worker task (PSRAM stack): runs only the cache-safe HTTPS GET and
///        publishes the result. WiFi/NTP/NVS stay on the UI task.
void fwCheckWorker(void*) {
    char tag[24] = {0};
    Status result = queryUpstream(tag, sizeof(tag));
    if (result == Status::UpToDate || result == Status::UpdateAvailable) {
        std::strncpy(s_foundTag, tag, sizeof(s_foundTag) - 1);
        s_foundTag[sizeof(s_foundTag) - 1] = '\0';
    } else {
        s_foundTag[0] = '\0';
    }
    s_result = result;
    s_state.store(kDone, std::memory_order_release);
    vTaskDelete(nullptr);
}

/// \brief Launches the GET worker if idle. \return true if it was started.
bool spawnWorker() {
    uint8_t expected = kIdle;
    if (!s_state.compare_exchange_strong(expected, kRunning)) return false;
    TaskHandle_t h = xTaskCreateStatic(fwCheckWorker, "fw_check",
                                       sizeof(s_workerStack) / sizeof(StackType_t), nullptr, 5,
                                       s_workerStack, &s_workerTcb);
    if (!h) {
        s_result = Status::CheckFailed;
        s_state.store(kDone, std::memory_order_release);
    }
    return true;
}

/// \brief Runs the cache-disabling steps on the UI task, then starts the GET.
///
/// \param connectFirst When true, brings WiFi up via the existing WifiHandlers
///        connect workflow (saved network, its own "Connecting" toast); on
///        failure the screen falls back to the needs-WiFi hint.
void runCheck(bool connectFirst) {
    if (s_state.load(std::memory_order_acquire) != kIdle) return;
    if (connectFirst) {
        if (!WifiHandlers::instance().acquire()) {
            renderStatus(Status::NeedsWifi);
            return;
        }
        s_heldWifi = true;
    }
    // Sync the clock while online so the stored check date is accurate (reuses
    // the active connection; returns immediately if the time is already set).
    WifiHandlers::instance().syncNtp(false);
    spawnWorker();
}

/// \brief Re-check entry, bound to the InfoView confirm key.
void onRecheckKey(void*) {
    if (s_state.load(std::memory_order_acquire) != kIdle) return;
    renderStatus(Status::Checking);
    runCheck(!WifiHandlers::instance().isConnected());
}

void FirmwareCheckView::onTick(uint32_t /*nowMs*/) {
    if (s_state.load(std::memory_order_acquire) != kDone) return;
    s_state.store(kIdle, std::memory_order_relaxed);
    // Persist the result and release WiFi on the UI task (cache-disabling NVS /
    // teardown must run on this internal-RAM stack, not the PSRAM worker).
    if (s_result == Status::UpToDate || s_result == Status::UpdateAvailable) {
        storeLastCheck(s_foundTag);
    }
    if (s_heldWifi) {
        WifiHandlers::instance().release();
        s_heldWifi = false;
    }
    renderStatus(s_result);
}

}  // namespace

void showFirmwareCheck() {
    s_state.store(kIdle, std::memory_order_relaxed);
    s_heldWifi = false;
    s_view.setYesNoCallbacks(onRecheckKey, nullptr, nullptr);

    bool connected = WifiHandlers::instance().isConnected();
    renderStatus(connected ? Status::Checking : Status::NeedsWifi);
    ViewStack::instance().push(&s_view);
    if (connected) runCheck(false);
}

bool firmwareCheckLastResult(char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';

    core::NvsScope nvs(kNvsNs, NVS_READONLY);
    if (nvs.status() != ESP_OK) return false;

    char ver[24] = {0};
    char date[12] = {0};
    size_t vlen = sizeof(ver);
    size_t dlen = sizeof(date);
    if (nvs_get_str(nvs, kNvsKeyVer, ver, &vlen) != ESP_OK || ver[0] == '\0') return false;
    nvs_get_str(nvs, kNvsKeyDate, date, &dlen);

    composeLastResult(out, cap, ver, date);
    return true;
}

}  // namespace cdc::ui
