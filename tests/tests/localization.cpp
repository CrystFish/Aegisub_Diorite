// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include <main.h>

#include "localization/localization_loader.h"
#include "localization/localization_matcher.h"

using namespace localization;

namespace {

MatchOptions DefaultOptions() {
	MatchOptions options;
	return options;
}

} // namespace

TEST(lagi_localization, NormalizeStripsTagsAndBreaks) {
	MatchOptions options = DefaultOptions();
	EXPECT_EQ(Normalize("{\\i1}Hello{\\i0} world", options), "hello world");
	EXPECT_EQ(Normalize("Line1\\NLine2", options), "line1 line2");
	EXPECT_EQ(Normalize("<b>Hello</b>", options), "hello");
	EXPECT_EQ(Normalize("  Hello   World  ", options), "hello world");
}

TEST(lagi_localization, NormalizePunctuation) {
	MatchOptions options = DefaultOptions();
	EXPECT_EQ(Normalize("Hello, world!", options), "hello world");
	EXPECT_EQ(Normalize("どうして、なんで？", options), "どうしてなんで");
	EXPECT_EQ(Normalize("Hello!", options), "hello");

	MatchOptions keep_punct = options;
	keep_punct.ignore_punctuation = false;
	EXPECT_EQ(Normalize("Hello!", keep_punct), "hello!");
}

TEST(lagi_localization, NormalizeCase) {
	MatchOptions options = DefaultOptions();
	EXPECT_EQ(Normalize("HELLO World", options), "hello world");

	MatchOptions keep_case = options;
	keep_case.ignore_case = false;
	EXPECT_EQ(Normalize("HELLO", keep_case), "HELLO");
}

TEST(lagi_localization, Similarity) {
	EXPECT_EQ(Similarity("hello", "hello"), 1.0);
	EXPECT_DOUBLE_EQ(Similarity("abc", "abd"), 2.0 / 3.0);
	EXPECT_DOUBLE_EQ(Similarity("hello", "world"), 0.2);
	EXPECT_EQ(Similarity("abcde", "vwxyz"), 0.0);
	EXPECT_EQ(Similarity("", ""), 1.0);
	EXPECT_EQ(Similarity("", "hello"), 0.0);
}

TEST(lagi_localization, SplitSegmentsMarkers) {
	auto segments = SplitSegments("{*1}Hello. {*2}World");
	ASSERT_EQ(segments.size(), 2u);
	EXPECT_EQ(segments[0], "Hello.");
	EXPECT_EQ(segments[1], "World");
}

TEST(lagi_localization, SplitSegmentsRangeMarkers) {
	auto segments = SplitSegments(
		"{*1}Hello. {*2}World. {*4-5}Joined range marker text. {*6,7}Comma range.");
	ASSERT_EQ(segments.size(), 4u);
	EXPECT_EQ(segments[0], "Hello.");
	EXPECT_EQ(segments[1], "World.");
	EXPECT_EQ(segments[2], "Joined range marker text.");
	EXPECT_EQ(segments[3], "Comma range.");
}

TEST(lagi_localization, SplitSegmentsSentences) {
	auto segments = SplitSegments("Hello. World! どうして？ なんで。");
	ASSERT_EQ(segments.size(), 4u);
	EXPECT_EQ(segments[0], "Hello.");
	EXPECT_EQ(segments[1], "World!");
	EXPECT_EQ(segments[2], "どうして？");
	EXPECT_EQ(segments[3], "なんで。");
}

TEST(lagi_localization, SplitSegmentsNoBoundaries) {
	auto segments = SplitSegments("Just one sentence");
	ASSERT_EQ(segments.size(), 1u);
	EXPECT_EQ(segments[0], "Just one sentence");
}

TEST(lagi_localization, SplitSegmentsSentenceToggle) {
	EXPECT_EQ(SplitSegments("Hello. World!", true).size(), 2u);
	EXPECT_EQ(SplitSegments("Hello. World!", false).size(), 1u);
	EXPECT_EQ(SplitSegments("こんにちは。さようなら。", true).size(), 2u);
	EXPECT_EQ(SplitSegments("こんにちは。さようなら。", false).size(), 1u);
}

TEST(lagi_localization, SplitSegmentsLeadingEllipsis) {
	// A leading ellipsis must stay attached to the sentence instead of
	// becoming a spurious one-character segment.
	auto segments = SplitSegments("… this is the very latest model, the LT600.");
	ASSERT_EQ(segments.size(), 1u);
	EXPECT_EQ(segments[0], "… this is the very latest model, the LT600.");
}

TEST(lagi_localization, SplitSegmentsRegexTags) {
	auto segments = SplitSegments("{TA7}Hello world. {*1}Second line. Third line.",
		true, "\\{[^}]*\\}");
	ASSERT_EQ(segments.size(), 3u);
	EXPECT_EQ(segments[0], "Hello world.");
	EXPECT_EQ(segments[1], "Second line.");
	EXPECT_EQ(segments[2], "Third line.");

	// Sentence splitting off: only regex boundaries apply.
	segments = SplitSegments("{TA7}Hello world. {*1}Second line. Third line.",
		false, "\\{[^}]*\\}");
	ASSERT_EQ(segments.size(), 2u);
	EXPECT_EQ(segments[0], "Hello world.");
	EXPECT_EQ(segments[1], "Second line. Third line.");
}

TEST(lagi_localization, MatchExactAndCrossFileKeyPair) {
	std::vector<LocalizationFile> files(2);
	files[0].name = "en.json";
	files[0].ok = true;
	files[0].items.push_back({"greeting", "Hello", "en.json"});
	files[1].name = "zh.json";
	files[1].ok = true;
	files[1].items.push_back({"greeting", "你好", "zh.json"});

	auto results = Match("Hello", files, DefaultOptions());
	ASSERT_EQ(results.size(), 1u);
	EXPECT_TRUE(results[0].exact);
	// The entry itself matches the line text exactly, so only the cross-file
	// localized result remains.
	EXPECT_EQ(results[0].file, "zh.json");
	EXPECT_EQ(results[0].replacement, "你好");
	EXPECT_EQ(results[0].origin, "key \"greeting\" in zh.json");
}

TEST(lagi_localization, MatchSegmentAcrossFiles) {
	// A subtitle that is one sentence of a multi-sentence localization entry
	// should match that sentence and offer the aligned sentence of the
	// same-key entry in the other file.
	std::vector<LocalizationFile> files(2);
	files[0].name = "en.json";
	files[0].ok = true;
	files[0].items.push_back({
		"X0101X_TERRACE_PARTI_PC_X01CONNOR_EMMAANDYOU01",
		"{*1}I know you and Emma were very close. {*2}You think she betrayed you - but she’s done nothing wrong.",
		"en.json"
	});
	files[1].name = "zh.json";
	files[1].ok = true;
	files[1].items.push_back({
		"X0101X_TERRACE_PARTI_PC_X01CONNOR_EMMAANDYOU01",
		"{*1}我知道你跟艾玛感情很好。{*2}你觉得她背叛了你……但她并没有做错什么。",
		"zh.json"
	});

	auto results = Match("I know you and Emma were very close.",
		files, DefaultOptions());
	ASSERT_FALSE(results.empty());

	bool found = false;
	for (auto const& r : results) {
		if (r.replacement == "我知道你跟艾玛感情很好。") {
			found = true;
			EXPECT_TRUE(r.exact);
			EXPECT_NE(r.origin.find("segment"), std::string::npos);
			EXPECT_EQ(r.file, "zh.json");
		}
	}
	EXPECT_TRUE(found);
}

TEST(lagi_localization, MatchPartialSegmentWithRangeMarker) {
	// A subtitle truncated mid-sentence must still match the aligned segment
	// when the entry uses range markers such as {*4-5}.
	std::vector<LocalizationFile> files(2);
	files[0].name = "en.json";
	files[0].ok = true;
	files[0].items.push_back({
		"X0101K_INTRO_LIME_PC_X01KSELLER01_MODELS01",
		"{*1}… this is the very latest model, the LT600. {*2}This is the top of the range household assistant. {*3}It cooks 10,000 different dishes, speaks 200 languages and dialects {*4-5}and handles the kids' homework from elementary school up to university level. {*6}At the moment we're doing a special promotion on this entire range at $7999, with a 48-months interest free credit. {*7}And it comes with a two-year warranty for parts and labor. {*8}An excellent choice, sir. {*9}If you'll just follow me, we'll process the order.",
		"en.json"
	});
	files[1].name = "zh.json";
	files[1].ok = true;
	files[1].items.push_back({
		"X0101K_INTRO_LIME_PC_X01KSELLER01_MODELS01",
		"{*1}……这是最新的型号，LT600。{*2}属于家务助理最顶级的旗舰系列。{*3}可以煮一万道菜色，说两百种语言和方言。{*4-5}有能力协助从小学到大学阶段的孩子完成学校作业。{*6}现在全系列正在做特别促销，只要$7999美金，可以使用48个月无息分期付款。{*7}附有两年零件更换与保修服务。{*8}您眼光真好，先生。{*9}请跟我来，我们将立即处理您的订单。",
		"zh.json"
	});

	auto results = Match("And handles the kids' homework from elementary school up to",
		files, DefaultOptions());
	ASSERT_FALSE(results.empty());

	bool found = false;
	for (auto const& r : results) {
		if (r.replacement == "有能力协助从小学到大学阶段的孩子完成学校作业。") {
			found = true;
			EXPECT_EQ(r.file, "zh.json");
			EXPECT_NE(r.origin.find("segment"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}

TEST(lagi_localization, MatchSegmentInline) {
	std::vector<LocalizationFile> files(1);
	files[0].name = "bilingual.json";
	files[0].ok = true;
	files[0].pairs.push_back({"",
		"{*1}Hello. {*2}Bye.", "{*1}你好。{*2}再见。", "bilingual.json"});

	auto results = Match("Hello", files, DefaultOptions());
	ASSERT_FALSE(results.empty());
	bool found = false;
	for (auto const& r : results) {
		if (r.replacement == "你好。") {
			found = true;
			EXPECT_NE(r.origin.find("segment"), std::string::npos);
		}
	}
	EXPECT_TRUE(found);
}

TEST(lagi_localization, MatchFuzzy) {
	std::vector<LocalizationFile> files(1);
	files[0].name = "text.txt";
	files[0].ok = true;
	files[0].items.push_back({"", "Hello world", "text.txt"});

	auto results = Match("Hello word", files, DefaultOptions());
	ASSERT_FALSE(results.empty());
	EXPECT_GE(results[0].score, 0.7);

	// Below the threshold
	MatchOptions strict = DefaultOptions();
	strict.threshold = 0.99;
	EXPECT_TRUE(Match("Hello word", files, strict).empty());
}

TEST(lagi_localization, MatchKeyAsText) {
	std::vector<LocalizationFile> files(1);
	files[0].name = "strings.json";
	files[0].ok = true;
	files[0].items.push_back({"Hello", "你好", "strings.json"});

	auto results = Match("Hello", files, DefaultOptions());
	ASSERT_FALSE(results.empty());
	bool found = false;
	for (auto const& r : results) {
		if (r.from_key && r.replacement == "你好") found = true;
	}
	EXPECT_TRUE(found);
}

TEST(lagi_localization, MatchBilingualSameFile) {
	std::vector<LocalizationFile> files(1);
	files[0].name = "bilingual.json";
	files[0].ok = true;
	files[0].pairs.push_back({"", "Hello", "你好", "bilingual.json"});

	auto results = Match("Hello", files, DefaultOptions());
	ASSERT_FALSE(results.empty());
	bool found = false;
	for (auto const& r : results) {
		if (r.replacement == "你好") found = true;
	}
	EXPECT_TRUE(found);
}

TEST(lagi_localization, MatchRespectsIgnorePunctuation) {
	std::vector<LocalizationFile> files(1);
	files[0].name = "text.txt";
	files[0].ok = true;
	files[0].items.push_back({"", "Hello!", "text.txt"});

	MatchOptions options = DefaultOptions();
	EXPECT_FALSE(Match("Hello", files, options).empty());

	options.ignore_punctuation = false;
	options.fuzzy = false;
	EXPECT_TRUE(Match("Hello", files, options).empty());
}

TEST(lagi_localization, LoadJsonFlat) {
	auto file = LoadContent(R"({"greeting": "Hello", "farewell": "Bye"})",
		"strings.json", "json");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.items.size(), 2u);
	bool found_greeting = false;
	bool found_farewell = false;
	for (auto const& item : file.items) {
		if (item.key == "greeting" && item.text == "Hello") found_greeting = true;
		if (item.key == "farewell" && item.text == "Bye") found_farewell = true;
	}
	EXPECT_TRUE(found_greeting);
	EXPECT_TRUE(found_farewell);
}

TEST(lagi_localization, LoadJsonBilingual) {
	auto file = LoadContent(R"({"source": "Hello", "translation": "你好"})",
		"bilingual.json", "json");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.pairs.size(), 1u);
	EXPECT_EQ(file.pairs[0].source, "Hello");
	EXPECT_EQ(file.pairs[0].translation, "你好");
}

TEST(lagi_localization, LoadJsonKeyedRecord) {
	auto file = LoadContent(R"({"key": "greet", "value": "Hi"})",
		"records.json", "json");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.items.size(), 1u);
	EXPECT_EQ(file.items[0].key, "greet");
	EXPECT_EQ(file.items[0].text, "Hi");
}

TEST(lagi_localization, LoadJsonLanguagePair) {
	auto file = LoadContent(R"({"en": "Hello", "zh": "你好"})",
		"lang.json", "json");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.pairs.size(), 1u);
	EXPECT_EQ(file.pairs[0].source, "Hello");
	EXPECT_EQ(file.pairs[0].translation, "你好");
}

TEST(lagi_localization, LoadJsonArray) {
	auto file = LoadContent(R"(["Alpha", "Beta"])", "list.json", "json");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.items.size(), 2u);
	EXPECT_EQ(file.items[0].text, "Alpha");
	EXPECT_EQ(file.items[1].text, "Beta");
}

TEST(lagi_localization, LoadCsvWithHeader) {
	auto file = LoadContent("key,source,translation\ng1,Hello,你好\n",
		"pairs.csv", "csv");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.pairs.size(), 1u);
	EXPECT_EQ(file.pairs[0].key, "g1");
	EXPECT_EQ(file.pairs[0].source, "Hello");
	EXPECT_EQ(file.pairs[0].translation, "你好");
}

TEST(lagi_localization, LoadCsvNoHeader) {
	auto file = LoadContent("greeting,Hello\nfarewell,Bye\n", "simple.csv", "csv");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.items.size(), 2u);
	EXPECT_EQ(file.items[0].key, "greeting");
	EXPECT_EQ(file.items[0].text, "Hello");
}

TEST(lagi_localization, LoadIni) {
	auto file = LoadContent("# comment\ngreeting = Hello\nfarewell: Bye\n",
		"game.ini", "ini");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.items.size(), 2u);
	EXPECT_EQ(file.items[0].key, "greeting");
	EXPECT_EQ(file.items[0].text, "Hello");
	EXPECT_EQ(file.items[1].key, "farewell");
	EXPECT_EQ(file.items[1].text, "Bye");
}

TEST(lagi_localization, LoadPlainTextList) {
	auto file = LoadContent("First line\nSecond line\n", "dialogue.txt", "txt");
	ASSERT_TRUE(file.ok);
	ASSERT_EQ(file.items.size(), 2u);
	EXPECT_TRUE(file.items[0].key.empty());
	EXPECT_EQ(file.items[0].text, "First line");
}

TEST(lagi_localization, LoadDetectsLanguage) {
	auto zh = LoadContent("{\"k\":\"你好世界\"}", "zh.json", "json");
	ASSERT_TRUE(zh.ok);
	EXPECT_EQ(zh.language, "中文");

	auto en = LoadContent("{\"k\":\"Hello world\"}", "en.json", "json");
	ASSERT_TRUE(en.ok);
	EXPECT_EQ(en.language, "English");

	auto ja = LoadContent("{\"k\":\"こんにちは世界\"}", "ja.json", "json");
	ASSERT_TRUE(ja.ok);
	EXPECT_EQ(ja.language, "日本語");

	auto ko = LoadContent("{\"k\":\"안녕하세요\"}", "ko.json", "json");
	ASSERT_TRUE(ko.ok);
	EXPECT_EQ(ko.language, "한국어");

	// Bilingual records are classified by their translation side, so a long
	// English source column must not dominate the detection.
	auto bilingual = LoadContent(
		"{\"k\":{\"source\":\"This is a fairly long English sentence that would "
		"dominate a naive character count\",\"translation\":\"这是一条中文译文\"}}",
		"pairs.json", "json");
	ASSERT_TRUE(bilingual.ok);
	EXPECT_EQ(bilingual.language, "中文");
}

TEST(lagi_localization, MatchPrefersPreferredLanguage) {
	std::vector<LocalizationFile> files(2);
	files[0].name = "en.json";
	files[0].ok = true;
	files[0].language = "English";
	files[0].pairs.push_back({"", "Hello", "Bonjour", "en.json"});
	files[1].name = "zh.json";
	files[1].ok = true;
	files[1].language = "中文";
	files[1].pairs.push_back({"", "Hello", "你好", "zh.json"});

	MatchOptions options = DefaultOptions();
	options.preferred_language = "中文";
	auto results = Match("Hello", files, options);
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].file, "zh.json");

	options.preferred_language = "English";
	results = Match("Hello", files, options);
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].file, "en.json");

	options.preferred_language = "日本語";
	results = Match("Hello", files, options);
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].file, "en.json");
}

TEST(lagi_localization, MatchPrefersWithoutSplitRegexText) {
	EXPECT_TRUE(DefaultOptions().prefer_without_split_regex);

	std::vector<LocalizationFile> files(2);
	files[0].name = "en.json";
	files[0].ok = true;
	files[0].items.push_back({"K", "{TA7}Hello world.", "en.json"});
	files[1].name = "zh.json";
	files[1].ok = true;
	files[1].items.push_back({"K", "{TA7}你好世界。", "zh.json"});

	MatchOptions options = DefaultOptions();
	options.preferred_language = "";
	options.split_regex = "\\{[^}]*\\}";

	// The segment cleaned by the split regex is preferred over raw entry text
	// that still carries the {TA7} tag.
	auto results = Match("Hello world.", files, options);
	ASSERT_FALSE(results.empty());
	EXPECT_EQ(results[0].replacement, "你好世界。");

	options.prefer_without_split_regex = false;
	results = Match("Hello world.", files, options);
	ASSERT_FALSE(results.empty());
	EXPECT_EQ(results[0].replacement, "{TA7}Hello world.");
}

TEST(lagi_localization, LoadBinaryRejected) {
	auto file = LoadContent(std::string("abc\0def", 7), "bad.json", "json");
	EXPECT_FALSE(file.ok);
	EXPECT_FALSE(file.error.empty());
}

TEST(lagi_localization, LoadInvalidJson) {
	auto file = LoadContent("{not json", "bad.json", "json");
	EXPECT_FALSE(file.ok);
	EXPECT_FALSE(file.error.empty());
}
