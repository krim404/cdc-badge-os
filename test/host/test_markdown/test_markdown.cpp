// Host unit tests for the Markdown parser: block classification, dynamic
// heading-font mapping (smallest-first), degrade-to-plain, truncation.
// Run with: pio test -e native

#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "../../../components/cdc_views/src/MarkdownParser.cpp"

using namespace cdc::ui;

namespace {
struct VecSink : StyledLineSink {
    std::vector<StyledLine> lines;
    void emit(const StyledLine& l) override { lines.push_back(l); }
};

std::string textOf(const StyledLine& l) { return std::string(l.text, l.text + l.len); }
}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_heading_single_level_not_largest(void) {
    const char* src = "# Title\n\nbody text\n";
    VecSink s;
    auto r = parseMarkdown(src, std::strlen(src), s, 1u << 20);
    const StyledLine* h = nullptr;
    for (auto& l : s.lines) {
        if (l.kind == MdLineKind::Heading) { h = &l; break; }
    }
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_STRING("Title", textOf(*h).c_str());
    TEST_ASSERT_EQUAL_UINT8(mdfont::Bold9pt, h->font);  // single level -> smallest
    TEST_ASSERT_FALSE(r.truncated);
}

void test_heading_three_levels_ascending(void) {
    const char* src = "# A\n## B\n### C\n";
    VecSink s;
    parseMarkdown(src, std::strlen(src), s, 1u << 20);
    uint8_t fA = 0, fB = 0, fC = 0;
    for (auto& l : s.lines) {
        if (l.kind != MdLineKind::Heading) continue;
        auto t = textOf(l);
        if (t == "A") fA = l.font;
        else if (t == "B") fB = l.font;
        else if (t == "C") fC = l.font;
    }
    TEST_ASSERT_EQUAL_UINT8(mdfont::Bold9pt, fC);   // least prominent -> smallest
    TEST_ASSERT_EQUAL_UINT8(mdfont::Bold12pt, fB);
    TEST_ASSERT_EQUAL_UINT8(mdfont::Bold18pt, fA);  // most prominent -> largest used
}

void test_heading_gap_levels(void) {
    const char* src = "## Top\n#### Sub\n";
    VecSink s;
    parseMarkdown(src, std::strlen(src), s, 1u << 20);
    uint8_t fTop = 0, fSub = 0;
    for (auto& l : s.lines) {
        if (l.kind != MdLineKind::Heading) continue;
        auto t = textOf(l);
        if (t == "Top") fTop = l.font;
        else if (t == "Sub") fSub = l.font;
    }
    TEST_ASSERT_EQUAL_UINT8(mdfont::Bold9pt, fSub);   // only two distinct levels
    TEST_ASSERT_EQUAL_UINT8(mdfont::Bold12pt, fTop);
}

void test_blocks_classified(void) {
    const char* src =
        "- item one\n"
        "1. first\n"
        "> quoted\n"
        "---\n"
        "```\n"
        "code line\n"
        "```\n"
        "plain para\n";
    VecSink s;
    parseMarkdown(src, std::strlen(src), s, 1u << 20);
    bool bullet = false, ordered = false, quote = false, rule = false, code = false, para = false;
    for (auto& l : s.lines) {
        switch (l.kind) {
            case MdLineKind::Bullet: bullet = (textOf(l) == "item one"); break;
            case MdLineKind::Ordered: ordered = (textOf(l) == "first"); break;
            case MdLineKind::Quote: quote = (textOf(l) == "quoted"); break;
            case MdLineKind::Rule: rule = true; break;
            case MdLineKind::Code:
                if (textOf(l) == "code line") { code = true; TEST_ASSERT_TRUE(l.inverted); }
                break;
            case MdLineKind::Paragraph: if (textOf(l) == "plain para") para = true; break;
            default: break;
        }
    }
    TEST_ASSERT_TRUE(bullet);
    TEST_ASSERT_TRUE(ordered);
    TEST_ASSERT_TRUE(quote);
    TEST_ASSERT_TRUE(rule);
    TEST_ASSERT_TRUE(code);
    TEST_ASSERT_TRUE(para);
}

void test_unsupported_degrades_to_plain(void) {
    const char* src = "| a | b |\n|---|---|\n| 1 | 2 |\n";
    VecSink s;
    parseMarkdown(src, std::strlen(src), s, 1u << 20);
    TEST_ASSERT_TRUE(s.lines.size() >= 1);
    bool anyPara = false;
    for (auto& l : s.lines) {
        if (l.kind == MdLineKind::Paragraph) anyPara = true;
    }
    TEST_ASSERT_TRUE(anyPara);
}

void test_truncation(void) {
    std::string big = "# H\n";
    big.append(100000, 'x');
    VecSink s;
    auto r = parseMarkdown(big.c_str(), big.size(), s, 64u * 1024u);
    TEST_ASSERT_TRUE(r.truncated);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_heading_single_level_not_largest);
    RUN_TEST(test_heading_three_levels_ascending);
    RUN_TEST(test_heading_gap_levels);
    RUN_TEST(test_blocks_classified);
    RUN_TEST(test_unsupported_degrades_to_plain);
    RUN_TEST(test_truncation);
    return UNITY_END();
}
