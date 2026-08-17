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

/// @file localization_loader.cpp
/// @brief Loaders for common game localization file formats.
///
/// Supported formats:
/// - JSON: flat key/value objects, keyed records, bilingual records
///   (source/translation or language pairs), nested objects and arrays
/// - CSV/TSV: with or without a header row
/// - INI/plain text: key=value, key: value, or plain text lists

#include "localization_loader.h"

#include <libaegisub/json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace localization {
namespace {

std::string Trim(std::string s) {
	auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
	s.erase(s.begin(), std::find_if(s.begin(), s.end(),
		[&](unsigned char c) { return !is_space(c); }));
	s.erase(std::find_if(s.rbegin(), s.rend(),
		[&](unsigned char c) { return !is_space(c); }).base(), s.end());
	return s;
}

std::string ToLowerAscii(std::string s) {
	for (char& c : s)
		if (c >= 'A' && c <= 'Z') c += 32;
	return s;
}

bool ContainsCjk(std::string const& s) {
	// CJK characters are 3-byte UTF-8 sequences in the E4..E9 byte range
	for (unsigned char c : s)
		if (c >= 0xE4 && c <= 0xE9) return true;
	return false;
}

/// Guess the dominant language of a UTF-8 text by script frequency.
/// Returns a stable display name ("中文", "English", ...) or an empty string
/// when no supported script is found.
std::string DetectLanguage(std::string const& text) {
	size_t han = 0;
	size_t kana = 0;
	size_t hangul = 0;
	size_t cyrillic = 0;
	size_t latin = 0;

	for (size_t i = 0; i < text.size();) {
		unsigned char c = static_cast<unsigned char>(text[i]);
		uint32_t cp = 0;
		int len = 0;
		if (c < 0x80) {
			cp = c;
			len = 1;
		}
		else if ((c >> 5) == 0x6) {
			cp = c & 0x1F;
			len = 2;
		}
		else if ((c >> 4) == 0xE) {
			cp = c & 0x0F;
			len = 3;
		}
		else if ((c >> 3) == 0x1E) {
			cp = c & 0x07;
			len = 4;
		}
		else {
			++i;
			continue;
		}
		if (i + static_cast<size_t>(len) > text.size()) break;
		bool valid = true;
		for (int k = 1; k < len; ++k) {
			unsigned char cc = static_cast<unsigned char>(text[i + k]);
			if ((cc & 0xC0) != 0x80) {
				valid = false;
				break;
			}
			cp = (cp << 6) | (cc & 0x3F);
		}
		i += static_cast<size_t>(len);
		if (!valid) continue;

		if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF))
			++han;
		else if ((cp >= 0x3040 && cp <= 0x30FF) ||
			(cp >= 0x31F0 && cp <= 0x31FF) ||
			(cp >= 0xFF66 && cp <= 0xFF9F))
			++kana;
		else if ((cp >= 0xAC00 && cp <= 0xD7AF) ||
			(cp >= 0x1100 && cp <= 0x11FF))
			++hangul;
		else if (cp >= 0x0400 && cp <= 0x04FF)
			++cyrillic;
		else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
			(cp >= 0x00C0 && cp <= 0x024F))
			++latin;
	}

	if (kana > 0) return "日本語";
	if (han > 0) return "中文";
	if (hangul > 0) return "한국어";
	if (cyrillic > 0) return "Русский";
	if (latin > 0) return "English";
	return {};
}

std::string StripQuotes(std::string s) {
	if (s.size() >= 2) {
		char f = s.front();
		char b = s.back();
		if ((f == '"' && b == '"') || (f == '\'' && b == '\''))
			return s.substr(1, s.size() - 2);
	}
	return s;
}

void AddItem(LocalizationFile& f, std::string key, std::string text) {
	key = Trim(std::move(key));
	text = Trim(std::move(text));
	if (text.empty()) return;
	f.items.push_back({std::move(key), std::move(text), f.name});
}

void AddPair(LocalizationFile& f, std::string key, std::string source,
	std::string translation) {
	key = Trim(std::move(key));
	source = Trim(std::move(source));
	translation = Trim(std::move(translation));
	if (source.empty() || translation.empty() || source == translation) return;
	f.pairs.push_back({std::move(key), source, translation, f.name});
	// Also expose both sides as plain items so text/key matching is uniform
	AddItem(f, f.pairs.back().key, source);
	AddItem(f, f.pairs.back().key, translation);
}

template<typename T>
bool TryCast(json::UnknownElement const& el, T& out) {
	try {
		out = static_cast<T const&>(el);
		return true;
	}
	catch (...) {
		return false;
	}
}

// json::Array/Object contain move-only json::UnknownElement values, so they
// cannot be copy-assigned. Return a const reference instead and let the caller
// iterate it in place.
template<typename T>
T const* TryCastRef(json::UnknownElement const& el) {
	try {
		return &static_cast<T const&>(el);
	}
	catch (...) {
		return nullptr;
	}
}

bool IsString(json::UnknownElement const& el) {
	json::String s;
	return TryCast(el, s);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

std::string FindStringField(json::Object const& obj,
	std::initializer_list<const char*> names) {
	for (auto const& name : names) {
		auto it = obj.find(name);
		if (it == obj.end()) continue;
		std::string value;
		if (TryCast(it->second, value)) return value;
	}
	return {};
}

bool TryParseBilingualObject(LocalizationFile& f, json::Object const& obj,
	std::string const& key_hint) {
	std::string source = FindStringField(obj, {"source", "original", "src"});
	std::string translation = FindStringField(obj,
		{"translation", "target", "localized", "localization", "tl"});

	if (!source.empty() || !translation.empty()) {
		if (source.empty())
			source = FindStringField(obj, {"text", "value", "message", "string"});
		if (translation.empty())
			translation = FindStringField(obj, {"text", "value", "message", "string"});
	}
	else {
		static const char* languages[] = {
			"en", "ja", "zh", "ko", "ru", "fr", "de", "es", "it", "pt",
			"zh_cn", "zh_tw"
		};
		std::vector<std::string> values;
		for (auto const* lang : languages) {
			auto it = obj.find(lang);
			if (it == obj.end()) continue;
			std::string value;
			if (TryCast(it->second, value) && !value.empty())
				values.push_back(value);
		}
		if (values.size() >= 2) {
			source = values[0];
			translation = values[1];
		}
	}

	if (source.empty() || translation.empty() || source == translation)
		return false;

	AddPair(f, key_hint, source, translation);
	return true;
}

void WalkJson(LocalizationFile& f, json::UnknownElement const& el,
	std::string const& path, int depth) {
	if (depth > 64) return;

	json::String str;
	if (TryCast(el, str)) {
		AddItem(f, path, str);
		return;
	}

	if (auto const* array = TryCastRef<json::Array>(el)) {
		for (auto const& child : *array)
			WalkJson(f, child, path, depth + 1);
		return;
	}

	auto const* object_ptr = TryCastRef<json::Object>(el);
	if (!object_ptr) return;
	auto const& object = *object_ptr;

	std::string record_key = FindStringField(object, {"key", "id"});

	bool all_strings = true;
	for (auto const& kv : object) {
		if (!IsString(kv.second)) {
			all_strings = false;
			break;
		}
	}
	if (all_strings && object.size() >= 2 &&
		TryParseBilingualObject(f, object, record_key))
		return;

	if (!record_key.empty()) {
		std::string value = FindStringField(object,
			{"value", "text", "message", "string", "translation", "target", "localized"});
		if (!value.empty()) {
			AddItem(f, record_key, value);
			return;
		}
		for (auto const& kv : object) {
			if (kv.first == "key" || kv.first == "id") continue;
			json::String v;
			if (TryCast(kv.second, v))
				AddItem(f, record_key, v);
		}
		return;
	}

	for (auto const& kv : object) {
		std::string child_path =
			path.empty() ? kv.first : path + "/" + kv.first;
		WalkJson(f, kv.second, child_path, depth + 1);
	}
}

void ParseJson(LocalizationFile& f, std::string const& content) {
	std::istringstream stream(content);
	json::UnknownElement root = agi::json_util::parse(stream);
	WalkJson(f, root, "", 0);
}

// ---------------------------------------------------------------------------
// CSV / TSV
// ---------------------------------------------------------------------------

std::vector<std::vector<std::string>> ParseDelimitedRows(
	std::string const& content, char delim) {
	std::vector<std::vector<std::string>> rows;
	std::vector<std::string> row;
	std::string cell;
	bool in_quotes = false;

	auto end_cell = [&] {
		row.push_back(cell);
		cell.clear();
	};
	auto end_row = [&] {
		end_cell();
		rows.push_back(std::move(row));
		row.clear();
	};

	size_t i = 0;
	while (i < content.size()) {
		char ch = content[i];
		if (in_quotes) {
			if (ch == '"') {
				if (i + 1 < content.size() && content[i + 1] == '"') {
					cell += '"';
					++i;
				}
				else {
					in_quotes = false;
				}
			}
			else {
				cell += ch;
			}
		}
		else if (ch == '"' && cell.empty()) {
			in_quotes = true;
		}
		else if (ch == delim) {
			end_cell();
		}
		else if (ch == '\n') {
			end_row();
		}
		else if (ch != '\r') {
			cell += ch;
		}
		++i;
	}
	if (!cell.empty() || !row.empty()) end_row();
	return rows;
}

int FindColumn(std::vector<std::string> const& header,
	std::initializer_list<const char*> names) {
	for (size_t i = 0; i < header.size(); ++i) {
		std::string cell = ToLowerAscii(Trim(header[i]));
		for (auto const* name : names)
			if (cell == name) return static_cast<int>(i);
	}
	return -1;
}

void ParseDelimited(LocalizationFile& f, std::string const& content, char delim) {
	auto rows = ParseDelimitedRows(content, delim);
	if (rows.empty()) return;

	bool has_header = false;
	auto const& first = rows[0];
	if (first.size() >= 2) {
		for (auto const& cell : first) {
			std::string low = ToLowerAscii(Trim(cell));
			if (low == "key" || low == "id" || low == "source" ||
				low == "original" || low == "text" || low == "value" ||
				low == "message" || low == "translation" || low == "target" ||
				low == "localized" || low == "en" || low == "ja" ||
				low == "zh" || low == "ko") {
				has_header = true;
				break;
			}
		}
	}

	size_t start = 0;
	int src_col = -1;
	int tgt_col = -1;
	int key_col = -1;
	if (has_header) {
		start = 1;
		src_col = FindColumn(first, {"source", "original", "text", "value",
			"message", "en", "ja", "zh", "ko"});
		tgt_col = FindColumn(first, {"translation", "target", "localized",
			"zh", "en", "ja", "ko", "text", "value", "message"});
		key_col = FindColumn(first, {"key", "id"});
		if (src_col == tgt_col) tgt_col = -1;
	}

	auto cell = [&](std::vector<std::string> const& row, int col) {
		if (col < 0 || static_cast<size_t>(col) >= row.size()) return std::string();
		return Trim(row[static_cast<size_t>(col)]);
	};

	for (size_t r = start; r < rows.size(); ++r) {
		auto const& row = rows[r];
		if (row.empty() || (row.size() == 1 && Trim(row[0]).empty())) continue;

		if (has_header) {
			std::string source = cell(row, src_col);
			std::string translation = cell(row, tgt_col);
			std::string key = cell(row, key_col);
			if (!source.empty() && !translation.empty())
				AddPair(f, key, source, translation);
			else if (!source.empty())
				AddItem(f, key, source);
			else if (!translation.empty())
				AddItem(f, key, translation);
		}
		else if (row.size() == 1) {
			AddItem(f, "", row[0]);
		}
		else if (row.size() == 2) {
			AddItem(f, row[0], row[1]);
		}
		else {
			AddPair(f, row[0], row[1], row[2]);
		}
	}
}

// ---------------------------------------------------------------------------
// Key/value and plain text
// ---------------------------------------------------------------------------

void ParseKeyValue(LocalizationFile& f, std::string const& content) {
	std::istringstream in(content);
	std::string line;
	while (std::getline(in, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		if (line.size() >= 2 && line[0] == '/' && line[1] == '/') continue;

		std::string key;
		std::string value;
		size_t eq = line.find('=');
		if (eq != std::string::npos) {
			key = Trim(line.substr(0, eq));
			value = Trim(line.substr(eq + 1));
		}
		else {
			size_t colon = line.find(':');
			if (colon != std::string::npos) {
				std::string prefix = line.substr(0, colon);
				if (!prefix.empty() && prefix.size() <= 64 &&
					!ContainsCjk(prefix) &&
					prefix.find_first_of("\"')") == std::string::npos) {
					key = Trim(prefix);
					value = Trim(line.substr(colon + 1));
				}
			}
		}

		if (key.empty() && value.empty()) {
			AddItem(f, "", line);
			continue;
		}
		AddItem(f, key, StripQuotes(value));
	}
}

} // namespace

LocalizationFile LoadContent(std::string const& content,
	std::string const& name, std::string const& ext) {
	LocalizationFile f;
	f.name = name;

	std::string body = content;
	if (body.size() >= 3 &&
		static_cast<unsigned char>(body[0]) == 0xEF &&
		static_cast<unsigned char>(body[1]) == 0xBB &&
		static_cast<unsigned char>(body[2]) == 0xBF) {
		body = body.substr(3);
	}

	if (body.find('\0') != std::string::npos) {
		f.ok = false;
		f.error = "The file appears to be binary or use an unsupported encoding.";
		return f;
	}

	std::string format = ToLowerAscii(Trim(ext));
	if (!format.empty() && format[0] == '.') format = format.substr(1);

	try {
		if (format == "json") {
			ParseJson(f, body);
		}
		else if (format == "csv" || format == "tsv") {
			char delim = '\t';
			if (format == "csv") {
				size_t comma = body.find(',');
				size_t tab = body.find('\t');
				if (tab != std::string::npos &&
					(comma == std::string::npos || tab < comma))
					delim = '\t';
				else
					delim = ',';
			}
			ParseDelimited(f, body, delim);
		}
		else {
			ParseKeyValue(f, body);
		}
	}
	catch (std::exception const& ex) {
		f.ok = false;
		f.error = ex.what();
		f.items.clear();
		f.pairs.clear();
		return f;
	}

	// Classify bilingual files by their localized (translation) side and
	// plain entries by their own text, so an English source column does not
	// pull a Chinese/Japanese file toward "English".
	std::unordered_set<std::string> pair_texts;
	for (auto const& pair : f.pairs) {
		pair_texts.insert(pair.source);
		pair_texts.insert(pair.translation);
	}
	std::string sample;
	for (auto const& item : f.items)
		if (pair_texts.find(item.text) == pair_texts.end())
			sample += item.text + "\n";
	for (auto const& pair : f.pairs)
		sample += pair.translation + "\n";
	f.language = DetectLanguage(sample);

	if (f.items.empty() && f.pairs.empty())
		f.error = "No text entries were found in this file.";
	return f;
}

} // namespace localization
