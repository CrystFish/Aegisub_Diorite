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
