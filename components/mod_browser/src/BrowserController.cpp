#include "BrowserController.h"

#include "Bookmarks.h"
#include "BrowserExtractTypes.h"
#include "BrowserPageView.h"
#include "ContentExtractor.h"
#include "HtmlTokenizer.h"
#include "PageFetcher.h"

#include "cdc_hal/IWifiController.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_views/ListView.h"
#include "cdc_views/MarkdownView.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_views/T9InputView.h"
#include "cdc_views/ToastView.h"
#include "cdc_log.h"

#include "cdc_core/Raii.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace cdc::browser {
namespace {

static constexpr const char* TAG = "BROWSER";

static constexpr uint8_t kMaxHistory = 8;
static constexpr size_t  kRawCap = 60u * 1024u;
static constexpr size_t  kScanCap = 512u * 1024u;
static constexpr size_t  kPostBodyCap = 4u * 1024u;

// Captive-portal detection probe (HTTP; behind a portal it redirects to the portal).
static constexpr const char* kCaptiveProbeUrl =
    "http://connectivitycheck.gstatic.com/generate_204";

enum class NavMode : uint8_t { Fresh, Forward, Back, Reload };
enum class Job : uint8_t { LoadPage, Captive, Probe, Submit, Source };

// ListItem userData sentinels (distinct from bookmark indices 1..N and the
// nullptr "open webpage" entry).
static constexpr intptr_t kCaptiveItem = 1000;

// Views (shared, single-instance).
ui::ListView     s_homeList;
ui::ListView     s_linksList;
ui::ListView     s_pageMenu;
ui::ListView     s_bmMenu;
ui::T9InputView  s_urlInput;
BrowserPageView  s_pageView;
ui::ToastView    s_loadingToast;

// Async fetch handshake between the UI task and the fetch worker task.
enum : uint8_t { kIdle = 0, kLoading = 1, kDone = 2 };
std::atomic<uint8_t> s_fetchState{kIdle};
const char* s_resultMsg = nullptr;  // non-null -> show this message instead of content
char        s_pendingUrl[kUrlCap] = {0};
NavMode     s_pendingMode = NavMode::Fresh;
Job         s_job = Job::LoadPage;
CookieJar   s_jar;                      // session cookies for the captive/login flow
bool        s_captiveDetected = false;  // last probe saw a portal -> show the login entry
bool        s_probeResult = false;      // worker -> UI handoff of the latest probe verdict
int         s_lastWifiState = -1;       // connect-transition detection in the tick
bool        s_fullMode = false;         // full-page view: extraction filter disabled

// The fetch worker's stack lives in PSRAM (internal RAM is scarce); its TCB is a
// small internal static. Reused across serialized loads.
constexpr size_t kWorkerStackBytes = 24u * 1024u;
EXT_RAM_BSS_ATTR StackType_t s_workerStack[kWorkerStackBytes / sizeof(StackType_t)];
StaticTask_t s_workerTcb;

// Menu item arrays (pointers stored by ListView; must outlive the list).
ui::ListItem s_homeItems[kMaxBookmarks + 2];
ui::ListItem s_linkItems[kMaxLinks];
ui::ListItem s_pageMenuItems[7];
ui::ListItem s_bmMenuItems[1];
uint8_t      s_homeCount = 0;

// PSRAM working buffers (lazy).
cdc::core::PsramUniquePtr<char>    s_raw;       // extractor UTF-8 output
cdc::core::PsramUniquePtr<char>    s_disp;      // CP437 source for MarkdownView
cdc::core::PsramUniquePtr<LinkRef> s_links;     // current page link table
cdc::core::PsramUniquePtr<char>    s_labels;    // CP437 link labels
cdc::core::PsramUniquePtr<FormSpec> s_forms;    // captured forms on the current page
cdc::core::PsramUniquePtr<char>    s_postBody;  // urlencoded body for a form submit
cdc::core::PsramUniquePtr<char>    s_localHtml; // retained source of the current local file

uint16_t s_linkCount = 0;
uint8_t  s_formCount = 0;
int      s_curFormIdx = -1;  // form being filled
size_t   s_postLen = 0;
char     s_curUrl[kUrlCap] = {0};
char     s_curTitle[kTitleCap] = {0};

char    s_hist[kMaxHistory][kUrlCap];
uint8_t s_histDepth = 0;
uint8_t s_pendingBmIndex = 0;
bool    s_localReturn = false;        // back from a link should restore the local file
char    s_localTitle[kTitleCap] = {0};

void copyStr(char* dst, const char* src, size_t cap)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t n = std::strlen(src);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

bool ensureBuffers()
{
    if (!s_raw) s_raw = cdc::core::psramAlloc<char>(kRawCap);
    if (!s_disp) s_disp = cdc::core::psramAlloc<char>(kRawCap);
    if (!s_links) s_links = cdc::core::psramAlloc<LinkRef>(kMaxLinks);
    if (!s_labels) s_labels = cdc::core::psramAlloc<char>(static_cast<size_t>(kMaxLinks) * kLabelCap);
    if (!s_forms) s_forms = cdc::core::psramAlloc<FormSpec>(kMaxForms);
    if (!s_postBody) s_postBody = cdc::core::psramAlloc<char>(kPostBodyCap);
    return s_raw && s_disp && s_links && s_labels && s_forms && s_postBody;
}

struct PassResult {
    bool       transportOk = false;
    bool       bodyFed = false;
    int        status = 0;
    HeadSignals head;
    SourceKind kind = SourceKind::Empty;
    uint16_t   linkCount = 0;
    uint8_t    formCount = 0;
};

void pickTitle(const HeadSignals& head, const char* url, char* out)
{
    const char* t = head.title[0] ? head.title : (head.ogTitle[0] ? head.ogTitle : url);
    copyStr(out, t, kTitleCap);
}

// The extractor and tokenizer carry multi-KB buffers; they live in PSRAM (not on
// the main task's stack) to keep the fetch/extract path's stack footprint small.
struct ExtractEngine {
    cdc::core::PsramUniquePtr<uint8_t> exMem;
    cdc::core::PsramUniquePtr<uint8_t> tokMem;
    ContentExtractor* ex = nullptr;
    HtmlTokenizer* tok = nullptr;

    void destroy()
    {
        if (tok) { tok->~HtmlTokenizer(); tok = nullptr; }
        if (ex) { ex->~ContentExtractor(); ex = nullptr; }
    }

    bool make(const ExtractConfig& cfg, const char* baseUrl)
    {
        destroy();
        if (!exMem) exMem = cdc::core::psramAlloc<uint8_t>(sizeof(ContentExtractor));
        if (!tokMem) tokMem = cdc::core::psramAlloc<uint8_t>(sizeof(HtmlTokenizer));
        if (!exMem || !tokMem) return false;
        ex = new (exMem.get()) ContentExtractor(cfg, baseUrl, s_raw.get(), kRawCap, s_links.get(),
                                                kMaxLinks, s_forms.get(), kMaxForms);
        tok = new (tokMem.get()) HtmlTokenizer(*ex);
        ex->setTokenizer(tok);
        return true;
    }

    ~ExtractEngine() { destroy(); }
};

ExtractConfig makeConfig()
{
    ExtractConfig cfg;
    cfg.maxSourceBytes = kRawCap - 1;
    cfg.maxScanBytes = kScanCap;
    cfg.keepAll = s_fullMode;  // full-page view keeps every block
    return cfg;
}

// Stable base URL for the active extractor (it stores the pointer, not a copy).
char s_extractBase[kUrlCap] = {0};

// Called by fetchExtract once the final (post-redirect) URL is known.
HtmlTokenizer* engineProvider(void* ctx, const char* finalUrl)
{
    auto* eng = static_cast<ExtractEngine*>(ctx);
    copyStr(s_extractBase, (finalUrl && finalUrl[0]) ? finalUrl : "", kUrlCap);
    if (!eng->make(makeConfig(), s_extractBase)) return nullptr;
    return eng->tok;
}

// Fetch `url` (GET or POST) and run the extractor over the response, resolving links
// and form actions against the final (post-redirect) URL.
PassResult extractVia(const char* url, bool isPost, const char* body, size_t bodyLen)
{
    PassResult pr;
    ExtractEngine eng;
    FetchResult fr = fetchExtract(url, isPost, body, bodyLen, kScanCap, &s_jar, engineProvider, &eng);
    pr.transportOk = fr.ok;
    pr.bodyFed = fr.bodyFed;
    pr.status = fr.status;
    if (eng.ex) {
        eng.tok->finish();
        eng.ex->finalize();
        pr.head = eng.ex->head();
        pr.kind = eng.ex->kind();
        pr.linkCount = eng.ex->linkCount();
        pr.formCount = eng.ex->formCount();
    }
    return pr;
}

PassResult extractHtml(const char* url) { return extractVia(url, false, nullptr, 0); }

void pushHistory(const char* url)
{
    if (!url || !url[0]) return;
    if (s_histDepth < kMaxHistory) {
        copyStr(s_hist[s_histDepth++], url, kUrlCap);
    } else {
        for (uint8_t i = 1; i < kMaxHistory; ++i) std::memcpy(s_hist[i - 1], s_hist[i], kUrlCap);
        copyStr(s_hist[kMaxHistory - 1], url, kUrlCap);
    }
}

bool popHistory(char* out)
{
    if (s_histDepth == 0) return false;
    copyStr(out, s_hist[--s_histDepth], kUrlCap);
    return true;
}

void applyNav(const char* url, NavMode mode)
{
    if (mode == NavMode::Fresh) {
        s_histDepth = 0;
        s_localReturn = false;
    } else if (mode == NavMode::Forward) {
        if (s_curUrl[0] == '\0') s_localReturn = true;  // leaving a local file via a link
        pushHistory(s_curUrl);
    }
    copyStr(s_curUrl, url, kUrlCap);
}

void showPageInternal(const char* title, const char* src, size_t len, const char* url)
{
    s_pageView.loadDoc(title, src, len, url);
    auto& vs = ui::ViewStack::instance();
    if (vs.current() == &s_pageView) {
        s_pageView.markDirty();
    } else {
        vs.push(&s_pageView);
    }
}

void showMessage(const char* msg)
{
    static char s_msgBuf[256];
    std::snprintf(s_msgBuf, sizeof(s_msgBuf), "%s", msg ? msg : "");
    showPageInternal(ui::tr("mod_browser.title"), s_msgBuf, std::strlen(s_msgBuf), s_curUrl);
}

// Forward decls (referenced before their definitions).
void startCaptive();
void startProbe();
void refreshHome();
void initHomeList();
void maybePromptCaptive();
void renderLocalPage();

// Decode the extracted content for display, or set s_resultMsg when nothing usable.
void finalizeContent(SourceKind kind, const char* titleBuf)
{
    if (kind == SourceKind::Empty) { s_resultMsg = ui::tr("mod_browser.err_empty"); return; }
    ui::render::decodeWebText(titleBuf, s_curTitle, sizeof(s_curTitle),
                              ui::render::DisplayTarget::Cp437);
    ui::render::decodeWebText(s_raw.get(), s_disp.get(), kRawCap, ui::render::DisplayTarget::Cp437);
}

// Append the current page's first form as interactive CP437 marker lines to the
// display source: "[ ]"/"[x]" per checkbox, then a "[>]" submit. \p always forces
// emission (captive portals); otherwise only accept-style forms (with a checkbox)
// are shown so a plain search box does not add a stray submit.
void appendFormControls(bool always)
{
    if (s_formCount == 0 || !s_forms || !s_disp) return;
    const FormSpec& f = s_forms.get()[0];
    bool hasCheck = false, hasSubmit = false;
    const FormField* submit = nullptr;
    for (uint8_t i = 0; i < f.fieldCount; ++i) {
        if (f.fields[i].kind == FormField::Kind::Checkbox) hasCheck = true;
        else if (f.fields[i].kind == FormField::Kind::Submit && !submit) {
            hasSubmit = true;
            submit = &f.fields[i];
        }
    }
    if (!hasSubmit) return;
    if (!always && !hasCheck) return;

    char* d = s_disp.get();
    size_t pos = std::strlen(d);
    char label[96];
    auto put = [&](const char* s) {
        size_t n = std::strlen(s);
        if (pos + n > kRawCap - 1) n = (pos < kRawCap - 1) ? (kRawCap - 1 - pos) : 0;
        std::memcpy(d + pos, s, n);
        pos += n;
    };
    auto putLabel = [&](const char* utf8, const char* cp437Fallback) {
        if (utf8 && utf8[0]) {
            ui::render::decodeWebText(utf8, label, sizeof(label), ui::render::DisplayTarget::Cp437);
            put(label);
        } else {
            put(cp437Fallback);
        }
    };
    put("\n\n");
    for (uint8_t i = 0; i < f.fieldCount; ++i) {
        const FormField& fld = f.fields[i];
        if (fld.kind != FormField::Kind::Checkbox) continue;
        put(fld.checked ? "[x] " : "[ ] ");
        putLabel(fld.name, ui::tr("mod_browser.accept"));
        put("\n");
    }
    put("[>] ");
    putLabel(submit->value[0] ? submit->value : submit->name, ui::tr("mod_browser.submit"));
    put("\n");
    d[pos <= kRawCap - 1 ? pos : kRawCap - 1] = '\0';
}

// Render the retained local HTML file (no network) as an interactive page.
void renderLocalPage()
{
    if (!s_localHtml || !s_localHtml.get()[0] || !ensureBuffers()) return;
    s_fullMode = false;
    ExtractEngine eng;
    if (!eng.make(makeConfig(), "")) return;  // local file: no base URL
    eng.tok->feed(s_localHtml.get(), std::strlen(s_localHtml.get()));
    eng.tok->finish();
    eng.ex->finalize();
    s_linkCount = eng.ex->linkCount();
    s_formCount = eng.ex->formCount();
    ui::render::decodeWebText(s_raw.get(), s_disp.get(), kRawCap, ui::render::DisplayTarget::Cp437);
    appendFormControls(false);
    s_curUrl[0] = '\0';  // local file: no URL
    copyStr(s_curTitle, s_localTitle, kTitleCap);
    showPageInternal(s_curTitle, s_disp.get(), std::strlen(s_disp.get()), "");
}

// Fetch + extract a page into the shared buffers (GET, or POST a urlencoded body).
void loadPageWork(const char* url, bool isPost, const char* body, size_t bodyLen)
{
    PassResult pr = extractVia(url, isPost, body, bodyLen);
    if (!pr.transportOk) { s_resultMsg = ui::tr("mod_browser.err_net"); return; }
    if (!pr.bodyFed) { s_resultMsg = ui::tr("mod_browser.err_type"); return; }
    SourceKind kind = pr.kind;
    s_linkCount = pr.linkCount;
    s_formCount = pr.formCount;
    char titleBuf[kTitleCap];
    pickTitle(pr.head, url, titleBuf);
    // An advertised AMP variant is the same article in lighter markup (GET loads only).
    // Full-page mode shows the literal page, so the AMP substitution is skipped.
    // A page's advertised RSS/Atom feed is the site feed, not this page, so it is unused.
    if (!isPost && !s_fullMode && pr.head.amphtml[0]) {
        PassResult pa = extractHtml(pr.head.amphtml);
        if (pa.transportOk && pa.bodyFed && pa.kind != SourceKind::Empty) {
            kind = pa.kind;
            s_linkCount = pa.linkCount;
            s_formCount = pa.formCount;
            pickTitle(pa.head, url, titleBuf);
        } else {
            PassResult pm = extractHtml(url);
            kind = pm.kind;
            s_linkCount = pm.linkCount;
            s_formCount = pm.formCount;
            pickTitle(pm.head, url, titleBuf);
        }
    }
    finalizeContent(kind, titleBuf);
    if (!s_resultMsg) appendFormControls(false);
}

// Runs on the fetch worker task. Touches no view-stack/UI state.
void doFetchWork()
{
    s_resultMsg = nullptr;
    auto* wifi = hal::getWifiControllerInstance();
    if (s_job == Job::Probe) {  // background captive detection (silent; verdict applied in pollLoad)
        int st = (wifi && wifi->isConnected()) ? probe(kCaptiveProbeUrl, &s_jar) : -1;
        s_probeResult = (st > 0 && st != 204);
        return;
    }
    if (!wifi || !wifi->isConnected()) { s_resultMsg = ui::tr("mod_browser.offline"); return; }

    if (s_job == Job::Submit) {
        loadPageWork(s_pendingUrl, true, s_postBody.get(), s_postLen);
        return;
    }
    if (s_job == Job::Source) {  // re-fetch the current URL and keep the raw bytes
        size_t rawLen = 0;
        FetchResult fr = fetchExtract(s_pendingUrl, false, nullptr, 0, kScanCap, &s_jar, nullptr,
                                      nullptr, s_raw.get(), kRawCap, &rawLen);
        if (!fr.ok) { s_resultMsg = ui::tr("mod_browser.err_net"); return; }
        if (!fr.bodyFed) { s_resultMsg = ui::tr("mod_browser.err_type"); return; }
        return;
    }
    if (s_job == Job::Captive) {
        s_jar.clear();
        PassResult pr = extractVia(kCaptiveProbeUrl, false, nullptr, 0);
        if (!pr.transportOk) { s_resultMsg = ui::tr("mod_browser.err_net"); return; }
        if (pr.status == 204) { s_resultMsg = ui::tr("mod_browser.connected"); return; }
        copyStr(s_pendingUrl, s_extractBase, kUrlCap);  // the real portal URL after redirects
        s_linkCount = pr.linkCount;
        s_formCount = pr.formCount;
        char titleBuf[kTitleCap];
        pickTitle(pr.head, s_pendingUrl, titleBuf);
        if (pr.kind == SourceKind::Empty && s_formCount == 0) {
            s_resultMsg = ui::tr("mod_browser.err_empty");
            return;
        }
        // Decode the (possibly empty) body, then append the portal form inline.
        ui::render::decodeWebText(titleBuf, s_curTitle, sizeof(s_curTitle),
                                  ui::render::DisplayTarget::Cp437);
        ui::render::decodeWebText(s_raw.get(), s_disp.get(), kRawCap,
                                  ui::render::DisplayTarget::Cp437);
        appendFormControls(true);
        return;
    }
    loadPageWork(s_pendingUrl, false, nullptr, 0);  // Job::LoadPage
}

void fetchWorker(void*)
{
    doFetchWork();
    s_fetchState.store(kDone, std::memory_order_release);
    vTaskDelete(nullptr);
}

// Begin an async job: optionally show the "loading" toast and spawn the worker.
void startJob(Job job, const char* url, NavMode mode, bool modal)
{
    if (s_fetchState.load(std::memory_order_acquire) != kIdle) return;
    if (job != Job::Probe && !ensureBuffers()) {
        applyNav(url, mode);
        showMessage(ui::tr("mod_browser.err_mem"));
        return;
    }
    s_job = job;
    copyStr(s_pendingUrl, url ? url : "", kUrlCap);
    s_pendingMode = mode;
    s_resultMsg = nullptr;
    s_fetchState.store(kLoading, std::memory_order_release);
    if (modal) {
        s_loadingToast.init(ui::tr("mod_browser.loading"), ui::ToastView::Icon::TASK, 0, false);
        ui::ViewStack::instance().showModal(&s_loadingToast);
    }
    TaskHandle_t h = xTaskCreateStatic(fetchWorker, "browser_fetch",
                                       sizeof(s_workerStack) / sizeof(StackType_t), nullptr, 5,
                                       s_workerStack, &s_workerTcb);
    if (h == nullptr) {
        s_fetchState.store(kIdle, std::memory_order_relaxed);
        if (modal) ui::ViewStack::instance().hideModal();
        if (job != Job::Probe) {
            applyNav(url, mode);
            showMessage(ui::tr("mod_browser.err_mem"));
        }
    }
}

void startLoad(const char* url, NavMode mode)
{
    if (mode != NavMode::Reload) s_fullMode = false;  // a fresh navigation resets full-page mode
    startJob(Job::LoadPage, url, mode, true);
}

// Runs on the UI task (module onTick). Completes a finished job.
void pollLoad()
{
    if (s_fetchState.load(std::memory_order_acquire) != kDone) return;
    s_fetchState.store(kIdle, std::memory_order_relaxed);
    Job job = s_job;
    if (job == Job::Probe) {
        bool was = s_captiveDetected;
        s_captiveDetected = s_probeResult;
        if (ui::ViewStack::instance().current() == &s_homeList) refreshHome();
        if (s_captiveDetected && !was) maybePromptCaptive();  // freshly detected -> ask to log in
        return;
    }
    ui::ViewStack::instance().hideModal();
    if (job == Job::Source) {  // raw HTML in s_raw -> CP437 (no entity decode) -> plain view
        if (s_resultMsg) { showMessage(s_resultMsg); return; }
        size_t n = std::strlen(s_raw.get());
        if (n >= kRawCap) n = kRawCap - 1;
        std::memcpy(s_disp.get(), s_raw.get(), n);
        s_disp.get()[n] = '\0';
        ui::render::utf8ToCp437Inplace(s_disp.get());
        ui::showPlainText(s_curTitle[0] ? s_curTitle : ui::tr("mod_browser.title"), s_disp.get(),
                          std::strlen(s_disp.get()));
        return;
    }
    applyNav(s_pendingUrl, s_pendingMode);
    if (s_resultMsg) {
        showMessage(s_resultMsg);
        return;
    }
    showPageInternal(s_curTitle, s_disp.get(), std::strlen(s_disp.get()), s_curUrl);
}

// ---- URL entry -----------------------------------------------------------

void onUrlEntered(const char* text)
{
    if (!text || std::strlen(text) < 4) return;
    bool onPage = (ui::ViewStack::instance().current() == &s_pageView);
    startLoad(text, onPage ? NavMode::Forward : NavMode::Fresh);
}

void openWebpagePrompt()
{
    s_urlInput.init(ui::tr("mod_browser.open_webpage"), "https://", 255);
    s_urlInput.setOnSave(onUrlEntered);
    ui::ViewStack::instance().push(&s_urlInput);
}

// ---- Home / bookmarks list ------------------------------------------------

void buildHomeItems()
{
    uint8_t n = 0;
    s_homeItems[n++] = {ui::tr("mod_browser.open_webpage"), 0, false, nullptr};
    if (s_captiveDetected) {  // only when a captive portal was detected
        s_homeItems[n++] = {ui::tr("mod_browser.captive_login"), 0, false,
                            reinterpret_cast<void*>(kCaptiveItem)};
    }
    auto& bm = Bookmarks::instance();
    for (uint8_t i = 0; i < bm.count() && n < kMaxBookmarks + 2; ++i) {
        s_homeItems[n++] = {bm.at(i).label, 0, false,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i + 1))};
    }
    s_homeCount = n;
}

void refreshHome()
{
    buildHomeItems();
    s_homeList.preservePosition();
    s_homeList.init(ui::tr("mod_browser.title"), s_homeItems, s_homeCount);
    s_homeList.markDirty();
}

void onHomeSelect(uint16_t, void* ud)
{
    if (ud == nullptr) {
        openWebpagePrompt();
        return;
    }
    intptr_t v = reinterpret_cast<intptr_t>(ud);
    if (v == kCaptiveItem) {
        startCaptive();
        return;
    }
    uint8_t bi = static_cast<uint8_t>(v - 1);
    auto& bm = Bookmarks::instance();
    if (bi < bm.count()) startLoad(bm.at(bi).url, NavMode::Fresh);
}

void onBmMenuSelect(uint16_t, void*)
{
    Bookmarks::instance().removeAt(s_pendingBmIndex);
    ui::ViewStack::instance().pop();  // close the delete menu
    refreshHome();
}

void onHomeMenu(uint16_t, void*)
{
    const ui::ListItem* sel = s_homeList.getSelectedItem();
    if (!sel || sel->userData == nullptr) return;  // "Open webpage" entry, nothing to delete
    s_pendingBmIndex = static_cast<uint8_t>(reinterpret_cast<intptr_t>(sel->userData) - 1);
    s_bmMenuItems[0] = {ui::tr("mod_browser.delete_bookmark"), 0, false, nullptr};
    s_bmMenu.init(ui::tr("mod_browser.bookmark"), s_bmMenuItems, 1);
    s_bmMenu.setOnSelect(onBmMenuSelect);
    ui::ViewStack::instance().push(&s_bmMenu);
}

// ---- Links list -----------------------------------------------------------

void onLinkSelect(uint16_t, void* ud)
{
    uint16_t li = static_cast<uint16_t>(reinterpret_cast<intptr_t>(ud));
    if (li >= s_linkCount) return;
    char href[kUrlCap];
    copyStr(href, s_links.get()[li].href, kUrlCap);
    ui::ViewStack::instance().pop();  // close the links list -> back to page
    startLoad(href, NavMode::Forward);
}

// ---- Form fill / submit ---------------------------------------------------

void appendEnc(char* dst, size_t cap, size_t& pos, const char* s)
{
    static const char* kHex = "0123456789ABCDEF";
    for (; s && *s && pos + 3 < cap; ++s) {
        char c = *s;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = kHex[(c >> 4) & 0xF];
            dst[pos++] = kHex[c & 0xF];
        }
    }
}

void submitForm()
{
    if (s_curFormIdx < 0 || !s_forms || !s_postBody) return;
    const FormSpec& f = s_forms.get()[s_curFormIdx];
    char* body = s_postBody.get();
    size_t pos = 0;
    bool first = true;
    bool submitUsed = false;
    for (uint8_t i = 0; i < f.fieldCount; ++i) {
        const FormField& fld = f.fields[i];
        const char* val = fld.value;
        bool include = false;
        if (fld.kind == FormField::Kind::Hidden) {
            include = (fld.name[0] != 0);
        } else if (fld.kind == FormField::Kind::Checkbox) {
            include = fld.checked && fld.name[0];
            if (!fld.value[0]) val = "on";
        } else {  // Submit: include only the first named submit button
            include = (!submitUsed && fld.name[0]);
            if (include) submitUsed = true;
        }
        if (!include) continue;
        if (!first && pos + 1 < kPostBodyCap) body[pos++] = '&';
        first = false;
        appendEnc(body, kPostBodyCap, pos, fld.name);
        if (pos + 1 < kPostBodyCap) body[pos++] = '=';
        appendEnc(body, kPostBodyCap, pos, val);
    }
    body[pos < kPostBodyCap ? pos : kPostBodyCap - 1] = '\0';
    s_postLen = pos;
    char action[kUrlCap];
    copyStr(action, f.action, kUrlCap);
    startJob(Job::Submit, action, NavMode::Forward, true);  // submitted inline from the page
}

void startCaptive() { startJob(Job::Captive, kCaptiveProbeUrl, NavMode::Fresh, true); }

void startProbe() { startJob(Job::Probe, "", NavMode::Fresh, false); }

// View the current page's raw HTML source (re-fetches the URL).
void startSource()
{
    if (s_curUrl[0]) startJob(Job::Source, s_curUrl, NavMode::Reload, true);
}

// Toggle the readable/full extraction mode and reload the current page in place.
void toggleFullPage()
{
    if (!s_curUrl[0]) return;
    s_fullMode = !s_fullMode;
    startLoad(s_curUrl, NavMode::Reload);
}

// Y on the captive prompt: enter the browser (if not already in it) and load the portal.
void onCaptiveYes(void*)
{
    auto& vs = ui::ViewStack::instance();  // the confirm modal already hid itself
    if (vs.current() != &s_homeList) {
        initHomeList();
        vs.push(&s_homeList);
    }
    startCaptive();
}

// Ask whether to log in to a freshly-detected captive portal. Suppressed while a
// modal is up or the device is locked, so it never covers the lock screen.
void maybePromptCaptive()
{
    auto& vs = ui::ViewStack::instance();
    if (vs.hasModal()) return;
    ui::IView* cur = vs.current();
    if (cur && std::strcmp(cur->getName(), "LockScreenView") == 0) return;
    ui::showConfirm(ui::tr("mod_browser.captive_prompt"), onCaptiveYes, nullptr,
                    ui::ConfirmView::Icon::QUESTION, nullptr);
}

// Silently probe for a captive portal on a fresh WiFi connection (sets the flag
// that gates the home-screen login entry; no toast).
void wifiTick()
{
    auto* wifi = hal::getWifiControllerInstance();
    int st = wifi ? static_cast<int>(wifi->getWifiState()) : -1;
    auto isConn = [](int s) {
        return s == static_cast<int>(hal::WifiState::GOT_IP) ||
               s == static_cast<int>(hal::WifiState::CONNECTED);
    };
    if (isConn(st) && !isConn(s_lastWifiState) &&
        s_fetchState.load(std::memory_order_acquire) == kIdle) {
        startProbe();
    }
    s_lastWifiState = st;
}

// ---- Page context menu ----------------------------------------------------

void bookmarkCurrent()
{
    if (s_curUrl[0]) Bookmarks::instance().add(s_curUrl, s_curTitle);
}

void onPageMenuSelect(uint16_t, void* ud)
{
    int action = static_cast<int>(reinterpret_cast<intptr_t>(ud));
    ui::ViewStack::instance().pop();  // close the context menu -> back to page
    switch (action) {
        case 2: browserOpenLinks(); break;
        case 3: bookmarkCurrent(); break;
        case 7: startSource(); break;
        case 8: toggleFullPage(); break;
        default: break;
    }
}

}  // namespace

// ---- Public hooks ---------------------------------------------------------

namespace {
void initHomeList()
{
    buildHomeItems();
    s_homeList.init(ui::tr("mod_browser.title"), s_homeItems, s_homeCount);
    s_homeList.setOnSelect(onHomeSelect);
    s_homeList.setOnMenu(onHomeMenu);
}
}  // namespace

ui::IView* browserEntryView()
{
    initHomeList();
    startProbe();  // re-check for a captive portal; refreshes the list when done
    return &s_homeList;
}

void browserOpenUrl(const char* url)
{
    if (!url || std::strlen(url) < 4) return;
    auto& vs = ui::ViewStack::instance();
    if (vs.current() != &s_homeList) {  // enter the browser if we are not already in it
        initHomeList();
        vs.push(&s_homeList);
    }
    startLoad(url, NavMode::Fresh);
}

void browserOpenLinks()
{
    uint16_t cap = s_linkCount < kMaxLinks ? s_linkCount : kMaxLinks;
    for (uint16_t i = 0; i < cap; ++i) {
        char* lab = s_labels.get() + static_cast<size_t>(i) * kLabelCap;
        ui::render::decodeWebText(s_links.get()[i].label, lab, kLabelCap,
                                  ui::render::DisplayTarget::Cp437);
        s_linkItems[i] = {lab, 0, false, reinterpret_cast<void*>(static_cast<intptr_t>(i))};
    }
    s_linksList.init(ui::tr("mod_browser.links"), s_linkItems, cap);
    s_linksList.setOnSelect(onLinkSelect);
    s_linksList.setEmptyText(ui::tr("mod_browser.no_links"));
    ui::ViewStack::instance().push(&s_linksList);
}

void browserOpenPageMenu()
{
    uint8_t n = 0;
    s_pageMenuItems[n++] = {ui::tr("mod_browser.links"), 0, false,
                            reinterpret_cast<void*>(static_cast<intptr_t>(2))};
    // Source / full-page / bookmark need a real URL; a local file has none.
    if (s_curUrl[0]) {
        s_pageMenuItems[n++] = {ui::tr("mod_browser.view_source"), 0, false,
                                reinterpret_cast<void*>(static_cast<intptr_t>(7))};
        s_pageMenuItems[n++] = {s_fullMode ? ui::tr("mod_browser.view_readable")
                                           : ui::tr("mod_browser.view_full"),
                                0, false, reinterpret_cast<void*>(static_cast<intptr_t>(8))};
        if (!Bookmarks::instance().contains(s_curUrl)) {
            s_pageMenuItems[n++] = {ui::tr("mod_browser.bookmark"), 0, false,
                                    reinterpret_cast<void*>(static_cast<intptr_t>(3))};
        }
    }
    s_pageMenu.init(ui::tr("mod_browser.menu"), s_pageMenuItems, n);
    s_pageMenu.setOnSelect(onPageMenuSelect);
    ui::ViewStack::instance().push(&s_pageMenu);
}

bool browserHistoryBack()
{
    char prev[kUrlCap];
    if (popHistory(prev)) {
        startLoad(prev, NavMode::Back);
        return true;
    }
    if (s_localReturn) {  // came from a local file via a link -> restore it
        s_localReturn = false;
        renderLocalPage();
        return true;
    }
    return false;
}

uint16_t browserLinkCount() { return s_linkCount; }

bool browserLinkInfo(uint16_t idx, char* labelOut, size_t labelCap, char* hrefOut, size_t hrefCap)
{
    if (idx >= s_linkCount || !s_links) return false;
    const LinkRef& lr = s_links.get()[idx];
    if (labelOut && labelCap) {
        ui::render::decodeWebText(lr.label, labelOut, labelCap, ui::render::DisplayTarget::Cp437);
    }
    if (hrefOut && hrefCap) {
        std::strncpy(hrefOut, lr.href, hrefCap - 1);
        hrefOut[hrefCap - 1] = '\0';
    }
    return true;
}

void browserFollowLink(uint16_t idx)
{
    if (idx >= s_linkCount || !s_links) return;
    char href[kUrlCap];
    copyStr(href, s_links.get()[idx].href, kUrlCap);
    startLoad(href, NavMode::Forward);
}

void browserSetFormCheckbox(uint16_t ordinal, bool checked)
{
    if (!s_forms || s_formCount == 0) return;
    FormSpec& f = s_forms.get()[0];
    uint16_t k = 0;
    for (uint8_t i = 0; i < f.fieldCount; ++i) {
        if (f.fields[i].kind != FormField::Kind::Checkbox) continue;
        if (k == ordinal) { f.fields[i].checked = checked; return; }
        ++k;
    }
}

void browserSubmitForm()
{
    if (s_formCount == 0) return;
    s_curFormIdx = 0;
    submitForm();
}

namespace {
bool isBrowserView(const ui::IView* v)
{
    return v == &s_pageView || v == &s_linksList || v == &s_pageMenu || v == &s_homeList ||
           v == &s_urlInput || v == &s_bmMenu;
}
}  // namespace

void exitBrowser()
{
    // Pop every browser-owned view, returning to wherever we were entered from
    // (the Tools menu, or the file explorer for a local HTML file).
    auto& vs = ui::ViewStack::instance();
    for (int i = 0; i < 16 && isBrowserView(vs.current()); ++i) vs.pop();
}

void browserPoll()
{
    pollLoad();
    wifiTick();
}

void browserShowLocalHtml(const char* title, const char* html, size_t len)
{
    if (!html) return;
    if (s_fetchState.load(std::memory_order_acquire) != kIdle) return;  // a fetch owns the buffers
    if (!ensureBuffers()) return;
    if (!s_localHtml) s_localHtml = cdc::core::psramAlloc<char>(kRawCap);
    if (!s_localHtml) return;
    size_t n = len < kRawCap - 1 ? len : kRawCap - 1;
    std::memcpy(s_localHtml.get(), html, n);
    s_localHtml.get()[n] = '\0';
    ui::render::decodeWebText(title ? title : "", s_localTitle, sizeof(s_localTitle),
                              ui::render::DisplayTarget::Cp437);
    s_histDepth = 0;       // fresh local session
    s_localReturn = false;
    renderLocalPage();
}

}  // namespace cdc::browser
