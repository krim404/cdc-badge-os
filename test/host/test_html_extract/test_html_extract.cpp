// Host unit tests for the browser engine: HTML tokenizer, jusText-lite content
// extractor, RSS/Atom feed parser, and source selector.
// Run with: pio test -e native

#include <unity.h>

#include <cstring>
#include <string>

#include "../../../components/mod_browser/src/HtmlTokenizer.cpp"
#include "../../../components/mod_browser/src/ContentExtractor.cpp"
#include "../../../components/mod_browser/src/FeedParser.cpp"
#include "../../../components/mod_browser/src/SourceSelector.cpp"

using namespace cdc::browser;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

namespace {
struct RecSink : TokenSink {
    std::string log;
    void onStartTag(const char* n, uint16_t l, const AttrList&, bool) override {
        log += "S:" + std::string(n, l) + ";";
    }
    void onEndTag(const char* n, uint16_t l) override { log += "E:" + std::string(n, l) + ";"; }
    void onText(const char* t, size_t l) override { log += "T:" + std::string(t, l) + ";"; }
};

std::string tokenize(const char* html, size_t chunk) {
    RecSink s;
    HtmlTokenizer tok(s);
    size_t n = std::strlen(html);
    if (chunk == 0) {
        tok.feed(html, n);
    } else {
        for (size_t i = 0; i < n; i += chunk) {
            size_t c = (i + chunk <= n) ? chunk : (n - i);
            tok.feed(html + i, c);
        }
    }
    tok.finish();
    return s.log;
}
}  // namespace

void test_tokenizer_basic(void) {
    std::string log = tokenize("<p>Hello <b>world</b></p>", 0);
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "S:p;"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "S:b;"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "E:b;"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "E:p;"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "T:world;"));
}

void test_tokenizer_script_suppressed(void) {
    // The '<p>' inside the script must NOT be tokenized as a start tag.
    std::string log = tokenize("<script>var x = '<p>inner</p>';</script><p>hi</p>", 0);
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "S:script;"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "E:script;"));
    // Exactly one real <p> start (the one after the script).
    const char* first = std::strstr(log.c_str(), "S:p;");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(std::strstr(first + 1, "S:p;"));
    TEST_ASSERT_NULL(std::strstr(log.c_str(), "inner"));
}

void test_tokenizer_split_chunk_invariance(void) {
    const char* html =
        "<div class=\"x\"><a href=\"/y\">link</a> text &amp; more</div><!-- c --><span>z</span>";
    TEST_ASSERT_EQUAL_STRING(tokenize(html, 0).c_str(), tokenize(html, 1).c_str());
    TEST_ASSERT_EQUAL_STRING(tokenize(html, 0).c_str(), tokenize(html, 3).c_str());
}

void test_tokenizer_malformed_no_crash(void) {
    // Unbalanced and mis-nested tags must not crash or hang.
    std::string log = tokenize("<div><b>bad</div></p><i>x", 0);
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "T:bad;"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "T:x;"));
}

void test_tokenizer_comment_with_gt(void) {
    // A comment containing '>' must be fully skipped.
    std::string log = tokenize("a<!-- b > c -->d<p>e</p>", 0);
    TEST_ASSERT_NULL(std::strstr(log.c_str(), "T: b > c "));
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "S:p;"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.c_str(), "T:e;"));
}

// ---------------------------------------------------------------------------
// Content extractor
// ---------------------------------------------------------------------------

namespace {
struct Extract {
    std::string out;
    HeadSignals head;
    uint16_t links = 0;
    SourceKind kind = SourceKind::Empty;
    LinkRef linkArr[kMaxLinks];
};

void runExtract(const char* html, const char* base, Extract& r) {
    static char outBuf[70000];
    ExtractConfig cfg;
    ContentExtractor ex(cfg, base, outBuf, sizeof(outBuf), r.linkArr, kMaxLinks);
    HtmlTokenizer tok(ex);
    ex.setTokenizer(&tok);
    tok.feed(html, std::strlen(html));
    tok.finish();
    ex.finalize();
    r.out = std::string(outBuf, ex.sourceLen());
    r.head = ex.head();
    r.links = ex.linkCount();
    r.kind = ex.kind();
}
}  // namespace

void test_extract_article_drops_chrome(void) {
    const char* html =
        "<html><head><title>My Article</title></head><body>"
        "<nav><a href=\"/home\">Home</a> <a href=\"/x\">Contact</a></nav>"
        "<article><h1>My Article</h1>"
        "<p>This is the first paragraph of the article body with more than enough prose "
        "to clear the length threshold so the extractor keeps it as real content.</p>"
        "<p>See <a href=\"/related\">related story</a> for the second paragraph which also "
        "carries a fair amount of genuine text and should be retained as content.</p>"
        "</article>"
        "<footer><a href=\"/tos\">Terms</a></footer></body></html>";
    Extract r;
    runExtract(html, "https://example.com/page", r);
    TEST_ASSERT_EQUAL(SourceKind::MainExtract, r.kind);
    TEST_ASSERT_NOT_NULL(std::strstr(r.out.c_str(), "first paragraph"));
    TEST_ASSERT_NOT_NULL(std::strstr(r.out.c_str(), "second paragraph"));
    TEST_ASSERT_NOT_NULL(std::strstr(r.out.c_str(), "# My Article"));
    TEST_ASSERT_NULL(std::strstr(r.out.c_str(), "Contact"));  // nav dropped
    TEST_ASSERT_NULL(std::strstr(r.out.c_str(), "Terms"));    // footer dropped
    // The article link was collected and resolved absolute.
    TEST_ASSERT_TRUE(r.links >= 1);
    bool found = false;
    for (uint16_t i = 0; i < r.links; ++i) {
        if (std::strcmp(r.linkArr[i].href, "https://example.com/related") == 0) found = true;
    }
    TEST_ASSERT_TRUE(found);
    // The link label is kept inline (delimited by the hidden link markers).
    TEST_ASSERT_NOT_NULL(std::strstr(r.out.c_str(), "related story"));
    TEST_ASSERT_NOT_NULL(std::strstr(r.out.c_str(), "\x10"));  // link-start delimiter present
}

void test_extract_links_match_markers(void) {
    // A link-dense block that is NOT inside <nav> gets dropped by link density.
    // The links it carried must be discarded too, so every collected link keeps a
    // matching [n] marker in the rendered text (in-view selection can find it).
    const char* html =
        "<html><body>"
        "<article><p>Real article paragraph with plenty of genuine prose text so "
        "this block is comfortably kept as the main content by the extractor here.</p>"
        "<p>Body two also has a real inline <a href=\"/inline\">inline link</a> and "
        "enough surrounding prose text to remain classified as readable content.</p>"
        "</article>"
        "<div><a href=\"/m1\">Menu One</a> <a href=\"/m2\">Menu Two</a> "
        "<a href=\"/m3\">Menu Three</a></div>"
        "</body></html>";
    Extract r;
    runExtract(html, "https://example.com/page", r);
    // Only the in-content link survives; the link-dense menu block is dropped.
    TEST_ASSERT_EQUAL_UINT16(1, r.links);
    TEST_ASSERT_EQUAL_STRING("https://example.com/inline", r.linkArr[0].href);
    TEST_ASSERT_NULL(std::strstr(r.out.c_str(), "Menu One"));
    TEST_ASSERT_NOT_NULL(std::strstr(r.out.c_str(), "inline link"));  // kept link label
}

void test_extract_keepall(void) {
    // Full-page mode (keepAll) keeps the link-dense menu block, so its links survive.
    const char* html =
        "<html><body>"
        "<article><p>Real article paragraph with plenty of genuine prose text so "
        "this block is comfortably kept as the main content by the extractor here.</p></article>"
        "<div><a href=\"/m1\">Menu One</a> <a href=\"/m2\">Menu Two</a></div>"
        "</body></html>";
    static char outBuf[70000];
    LinkRef links[kMaxLinks];
    ExtractConfig cfg;
    cfg.keepAll = true;
    ContentExtractor ex(cfg, "https://example.com/page", outBuf, sizeof(outBuf), links, kMaxLinks);
    HtmlTokenizer tok(ex);
    ex.setTokenizer(&tok);
    tok.feed(html, std::strlen(html));
    tok.finish();
    ex.finalize();
    TEST_ASSERT_EQUAL_UINT16(2, ex.linkCount());
    TEST_ASSERT_NOT_NULL(std::strstr(outBuf, "Menu One"));
}

void test_extract_head_signals(void) {
    const char* html =
        "<html><head><title>T</title>"
        "<meta property=\"og:title\" content=\"OG T\">"
        "<meta property=\"og:description\" content=\"OG D\">"
        "<meta name=\"description\" content=\"Meta D\">"
        "<link rel=\"canonical\" href=\"https://example.com/canon\">"
        "<link rel=\"amphtml\" href=\"/amp\">"
        "<link rel=\"alternate\" type=\"application/rss+xml\" href=\"/feed.xml\">"
        "</head><body><p>x</p></body></html>";
    Extract r;
    runExtract(html, "https://example.com/page", r);
    TEST_ASSERT_EQUAL_STRING("T", r.head.title);
    TEST_ASSERT_EQUAL_STRING("OG T", r.head.ogTitle);
    TEST_ASSERT_EQUAL_STRING("OG D", r.head.ogDescription);
    TEST_ASSERT_EQUAL_STRING("Meta D", r.head.metaDescription);
    TEST_ASSERT_EQUAL_STRING("https://example.com/canon", r.head.canonical);
    TEST_ASSERT_EQUAL_STRING("https://example.com/amp", r.head.amphtml);
    TEST_ASSERT_EQUAL_STRING("https://example.com/feed.xml", r.head.feedUrl);
}

void test_extract_og_fallback(void) {
    const char* html =
        "<html><head><title>SPA</title>"
        "<meta property=\"og:title\" content=\"SPA Title\">"
        "<meta property=\"og:description\" content=\"SPA description text here.\">"
        "</head><body><div id=\"root\"></div><script>app()</script></body></html>";
    Extract r;
    runExtract(html, "https://example.com/", r);
    TEST_ASSERT_EQUAL(SourceKind::OgCard, r.kind);
    TEST_ASSERT_NOT_NULL(std::strstr(r.out.c_str(), "SPA Title"));
    TEST_ASSERT_NOT_NULL(std::strstr(r.out.c_str(), "SPA description"));
}

void test_extract_empty(void) {
    const char* html = "<html><head></head><body><div></div></body></html>";
    Extract r;
    runExtract(html, "https://example.com/", r);
    TEST_ASSERT_EQUAL(SourceKind::Empty, r.kind);
}

// ---------------------------------------------------------------------------
// Feed parser
// ---------------------------------------------------------------------------

namespace {
void runFeed(const char* xml, FeedItem& item, uint16_t& count, size_t& bodyLen,
             std::string& body) {
    static char bodyBuf[16000];
    FeedParser fp(bodyBuf, sizeof(bodyBuf));
    fp.feed(xml, std::strlen(xml));
    fp.finish();
    fp.getFirstItem(item);
    count = fp.itemCount();
    bodyLen = fp.bodyLen();
    body = std::string(bodyBuf, fp.bodyLen());
}
}  // namespace

void test_feed_rss_fulltext(void) {
    const char* xml =
        "<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>Feed</title>"
        "<item><title>Item One</title><link>https://ex.com/1</link>"
        "<description>Summary text</description>"
        "<content:encoded><![CDATA[<p>Full body content here.</p>]]></content:encoded></item>"
        "<item><title>Item Two</title><link>https://ex.com/2</link></item>"
        "</channel></rss>";
    FeedItem item;
    uint16_t count;
    size_t bodyLen;
    std::string body;
    runFeed(xml, item, count, bodyLen, body);
    TEST_ASSERT_EQUAL_UINT16(2, count);
    TEST_ASSERT_EQUAL_STRING("Item One", item.title);
    TEST_ASSERT_EQUAL_STRING("https://ex.com/1", item.link);
    TEST_ASSERT_TRUE(item.fullText);
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "Full body content here."));
}

void test_feed_atom(void) {
    const char* xml =
        "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\"><title>AF</title>"
        "<entry><title>Atom Item</title><link href=\"https://ex.com/a1\" rel=\"alternate\"/>"
        "<content type=\"html\">Atom body text</content></entry></feed>";
    FeedItem item;
    uint16_t count;
    size_t bodyLen;
    std::string body;
    runFeed(xml, item, count, bodyLen, body);
    TEST_ASSERT_EQUAL_UINT16(1, count);
    TEST_ASSERT_EQUAL_STRING("Atom Item", item.title);
    TEST_ASSERT_EQUAL_STRING("https://ex.com/a1", item.link);
    TEST_ASSERT_TRUE(item.fullText);
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "Atom body text"));
}

void test_feed_summary_only(void) {
    const char* xml =
        "<rss><channel><item><title>S</title><link>https://ex.com/s</link>"
        "<description>Only summary</description></item></channel></rss>";
    FeedItem item;
    uint16_t count;
    size_t bodyLen;
    std::string body;
    runFeed(xml, item, count, bodyLen, body);
    TEST_ASSERT_EQUAL_UINT16(1, count);
    TEST_ASSERT_FALSE(item.fullText);
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "Only summary"));
}

// ---------------------------------------------------------------------------
// Source selector
// ---------------------------------------------------------------------------

void test_selector_feed(void) {
    HeadSignals h;
    std::strcpy(h.feedUrl, "https://ex.com/feed");
    auto d = selectSource("https://blog.example.org/post", h);
    TEST_ASSERT_EQUAL(Source::Feed, d.source);
    TEST_ASSERT_EQUAL_STRING("https://ex.com/feed", d.fetchUrl);
}

void test_selector_amp(void) {
    HeadSignals h;
    std::strcpy(h.amphtml, "https://ex.com/amp");
    auto d = selectSource("https://blog.example.org/post", h);
    TEST_ASSERT_EQUAL(Source::Amp, d.source);
    TEST_ASSERT_EQUAL_STRING("https://ex.com/amp", d.fetchUrl);
}

void test_selector_main_canonical(void) {
    HeadSignals h;
    std::strcpy(h.canonical, "https://ex.com/canon");
    auto d = selectSource("https://blog.example.org/post", h);
    TEST_ASSERT_EQUAL(Source::MainExtract, d.source);
    TEST_ASSERT_EQUAL_STRING("https://ex.com/canon", d.fetchUrl);
}

void test_selector_text_mirror(void) {
    HeadSignals h;
    auto d = selectSource("https://www.cnn.com/2026/06/18/story/index.html", h);
    TEST_ASSERT_EQUAL(Source::TextMirror, d.source);
    TEST_ASSERT_EQUAL_STRING("https://lite.cnn.com/2026/06/18/story/index.html", d.fetchUrl);
}

// ---------------------------------------------------------------------------
// Form capture
// ---------------------------------------------------------------------------

namespace {
struct ExtractF {
    uint8_t  forms = 0;
    LinkRef  linkArr[kMaxLinks];
    FormSpec formArr[kMaxForms];
};

void runExtractForms(const char* html, const char* base, ExtractF& r) {
    static char outBuf[70000];
    ExtractConfig cfg;
    ContentExtractor ex(cfg, base, outBuf, sizeof(outBuf), r.linkArr, kMaxLinks, r.formArr,
                        kMaxForms);
    HtmlTokenizer tok(ex);
    ex.setTokenizer(&tok);
    tok.feed(html, std::strlen(html));
    tok.finish();
    ex.finalize();
    r.forms = ex.formCount();
}
}  // namespace

void test_form_capture_hidden_checkbox_submit(void) {
    const char* html =
        "<html><body><p>x</p>"
        "<form method=\"post\" action=\"/login\">"
        "<input type=\"hidden\" name=\"sid\" value=\"abc123\">"
        "<input type=\"checkbox\" name=\"agree\" value=\"yes\">"
        "<input type=\"submit\" name=\"action\" value=\"Connect\">"
        "</form></body></html>";
    ExtractF r;
    runExtractForms(html, "http://1.2.3.4/portal", r);
    TEST_ASSERT_EQUAL_UINT8(1, r.forms);
    const FormSpec& f = r.formArr[0];
    TEST_ASSERT_TRUE(f.post);
    TEST_ASSERT_EQUAL_STRING("http://1.2.3.4/login", f.action);
    TEST_ASSERT_EQUAL_UINT8(3, f.fieldCount);
    TEST_ASSERT_EQUAL_INT((int)FormField::Kind::Hidden, (int)f.fields[0].kind);
    TEST_ASSERT_EQUAL_STRING("sid", f.fields[0].name);
    TEST_ASSERT_EQUAL_STRING("abc123", f.fields[0].value);
    TEST_ASSERT_EQUAL_INT((int)FormField::Kind::Checkbox, (int)f.fields[1].kind);
    TEST_ASSERT_EQUAL_STRING("agree", f.fields[1].name);
    TEST_ASSERT_FALSE(f.fields[1].checked);
    TEST_ASSERT_EQUAL_INT((int)FormField::Kind::Submit, (int)f.fields[2].kind);
}

void test_form_checkbox_checked_and_button(void) {
    const char* html =
        "<form action=\"https://gw/connect\">"
        "<input type='checkbox' name='tos' checked>"
        "<button type='submit'>I agree</button>"
        "</form>";
    ExtractF r;
    runExtractForms(html, "http://gw/", r);
    TEST_ASSERT_EQUAL_UINT8(1, r.forms);
    const FormSpec& f = r.formArr[0];
    TEST_ASSERT_FALSE(f.post);  // method defaults to GET
    TEST_ASSERT_EQUAL_STRING("https://gw/connect", f.action);
    TEST_ASSERT_EQUAL_UINT8(2, f.fieldCount);
    TEST_ASSERT_EQUAL_INT((int)FormField::Kind::Checkbox, (int)f.fields[0].kind);
    TEST_ASSERT_TRUE(f.fields[0].checked);
    TEST_ASSERT_EQUAL_INT((int)FormField::Kind::Submit, (int)f.fields[1].kind);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tokenizer_basic);
    RUN_TEST(test_tokenizer_script_suppressed);
    RUN_TEST(test_tokenizer_split_chunk_invariance);
    RUN_TEST(test_tokenizer_malformed_no_crash);
    RUN_TEST(test_tokenizer_comment_with_gt);
    RUN_TEST(test_extract_article_drops_chrome);
    RUN_TEST(test_extract_links_match_markers);
    RUN_TEST(test_extract_keepall);
    RUN_TEST(test_extract_head_signals);
    RUN_TEST(test_extract_og_fallback);
    RUN_TEST(test_extract_empty);
    RUN_TEST(test_feed_rss_fulltext);
    RUN_TEST(test_feed_atom);
    RUN_TEST(test_feed_summary_only);
    RUN_TEST(test_selector_feed);
    RUN_TEST(test_selector_amp);
    RUN_TEST(test_selector_main_canonical);
    RUN_TEST(test_selector_text_mirror);
    RUN_TEST(test_form_capture_hidden_checkbox_submit);
    RUN_TEST(test_form_checkbox_checked_and_button);
    return UNITY_END();
}
