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

#pragma once

#include <string>
#include <vector>

namespace localization {

/// A single piece of text found in a localization file.
/// @param key Optional key/id of the entry (empty when the file has no keys)
/// @param text The text content
/// @param file Display name of the source file
struct TextItem {
	std::string key;
	std::string text;
	std::string file;
};

/// A source -> translation pair found inside a single file.
struct BilingualRecord {
	std::string key;
	std::string source;
	std::string translation;
	std::string file;
};

/// The result of loading one localization file.
struct LocalizationFile {
	std::string name;
	std::vector<TextItem> items;
	std::vector<BilingualRecord> pairs;
	bool ok = true;
	std::string error;
};

/// Matching behavior options. Fuzzy matching, tag stripping, punctuation
/// stripping and case folding all default to enabled.
struct MatchOptions {
	bool fuzzy = true;
	bool ignore_tags = true;
	bool ignore_punctuation = true;
	bool ignore_case = true;
	double threshold = 0.7;
};

/// One candidate result for a subtitle line.
struct MatchResult {
	double score = 0.0;
	bool exact = false;
	/// True when the match happened against the entry key rather than its text
	bool from_key = false;
	/// Source file of the matched entry
	std::string file;
	/// Key of the matched entry (may be empty)
	std::string key;
	/// The text that matched (or the key when from_key is true)
	std::string matched;
	/// The localized text to insert/replace with
	std::string replacement;
	/// Human-readable explanation of where the replacement came from
	std::string origin;
};

/// Match a subtitle line against all loaded localization files.
std::vector<MatchResult> Match(std::string const& subtitle_text,
	std::vector<LocalizationFile> const& files, MatchOptions const& options);

} // namespace localization
