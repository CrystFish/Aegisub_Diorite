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

/// @file localization_matcher.cpp
/// @brief Normalization, segmentation and fuzzy matching for localization
/// entries.

#include "localization_matcher.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace localization {
namespace {

std::vector<uint32_t> Utf8ToCodepoints(std::string const& text) {
	std::vector<uint32_t> out;
	size_t i = 0;
	size_t n = text.size();
	while (i < n) {
		unsigned char c = static_cast<unsigned char>(text[i]);
		if (c < 0x80) {
			out.push_back(c);
			++i;
			continue;
		}

		int extra = 0;
		uint32_t cp = 0;
		if ((c & 0xE0) == 0xC0) {
			extra = 1;
			cp = c & 0x1F;
		}
		else if ((c & 0xF0) == 0xE0) {
			extra = 2;
			cp = c & 0x0F;
		}
		else if ((c & 0xF8) == 0xF0) {
			extra = 3;
			cp = c & 0x07;
		}
		else {
			++i;
			continue;
		}

		if (i + extra >= n) break;
		bool valid = true;
		for (int k = 1; k <= extra; ++k) {
			unsigned char cc = static_cast<unsigned char>(text[i + k]);
			if ((cc & 0xC0) != 0x80) {
				valid = false;
				break;
			}
			cp = (cp << 6) | (cc & 0x3F);
		}
		if (!valid) {
			++i;
			continue;
		}
		out.push_back(cp);
		i += extra + 1;
	}
	return out;
}

std::string CodepointsToUtf8(std::vector<uint32_t> const& cps) {
	std::string out;
	for (uint32_t cp : cps) {
		if (cp < 0x80) {
			out += static_cast<char>(cp);
		}
		else if (cp < 0x800) {
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000) {
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else {
			out += static_cast<char>(0xF0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}
	return out;
}

bool IsAsciiSpace(uint32_t cp) {
	return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
		cp == '\f' || cp == '\v';
}

bool IsUnicodeSpace(uint32_t cp) {
	return cp == 0x00A0 || cp == 0x1680 ||
		(cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
		cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool IsZeroWidth(uint32_t cp) {
	return cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 ||
		cp == 0xFEFF;
}

bool IsPunctuation(uint32_t cp) {
	if (cp < 0x80) {
		return (cp >= 0x21 && cp <= 0x2F) ||
			(cp >= 0x3A && cp <= 0x40) ||
			(cp >= 0x5B && cp <= 0x60) ||
			(cp >= 0x7B && cp <= 0x7E);
	}
	return (cp >= 0x2000 && cp <= 0x206F) ||
		(cp >= 0x2E00 && cp <= 0x2E7F) ||
		(cp >= 0x3001 && cp <= 0x3003) ||
		(cp >= 0x3008 && cp <= 0x301F) ||
		cp == 0x3030 ||
		(cp >= 0xFE10 && cp <= 0xFE1F) ||
		(cp >= 0xFE30 && cp <= 0xFE4F) ||
		(cp >= 0xFF01 && cp <= 0xFF0F) ||
		(cp >= 0xFF1A && cp <= 0xFF1F) ||
		(cp >= 0xFF3B && cp <= 0xFF40) ||
		(cp >= 0xFF5B && cp <= 0xFF65) ||
		cp == 0x00A1 || cp == 0x00BF || cp == 0x2026 || cp == 0x2027 ||
		cp == 0x203C || cp == 0x2047;
}

uint32_t ToLower(uint32_t cp) {
	if (cp >= 'A' && cp <= 'Z') return cp + 32;
	if (cp >= 0xC0 && cp <= 0xD6) return cp + 32;
	if (cp >= 0xD8 && cp <= 0xDE) return cp + 32;
	if (cp >= 0x100 && cp <= 0x137 && (cp & 1) == 0) return cp + 1;
	if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 32;
	if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
	if (cp >= 0xFF21 && cp <= 0xFF3A) return cp + 0x20;
	return cp;
}

void ReplaceAll(std::string& s, std::string const& from, std::string const& to) {
	if (from.empty()) return;
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
}

std::string StripBraceTags(std::string const& s) {
	std::string out;
	size_t depth = 0;
	for (char ch : s) {
		if (ch == '{') {
			++depth;
			continue;
		}
		if (ch == '}' && depth > 0) {
			--depth;
			continue;
		}
		if (depth == 0) out += ch;
	}
	return out;
}

std::string StripAngleTags(std::string const& s) {
	std::string out;
	size_t i = 0;
	size_t n = s.size();
	while (i < n) {
		if (s[i] == '<') {
			size_t end = s.find('>', i + 1);
			if (end != std::string::npos) {
				std::string inner = s.substr(i + 1, end - i - 1);
				bool looks_like_tag = !inner.empty() &&
					inner.find_first_of(" \t\r\n") == std::string::npos &&
					inner.find('<') == std::string::npos;
				if (looks_like_tag) {
					i = end + 1;
					continue;
				}
			}
		}
		out += s[i++];
	}
	return out;
}

std::string TrimAscii(std::string s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return {};
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

/// Bounded Levenshtein distance. Returns true when the distance is within
/// max_dist, and stores the exact distance in dist.
bool DistanceWithin(std::vector<uint32_t> const& a, std::vector<uint32_t> const& b,
	size_t max_dist, size_t& dist) {
	size_t n = a.size();
	size_t m = b.size();
	if (n > m + max_dist || m > n + max_dist) return false;

	std::vector<size_t> prev(m + 1);
	std::vector<size_t> cur(m + 1);
	for (size_t j = 0; j <= m; ++j) prev[j] = j;

	for (size_t i = 1; i <= n; ++i) {
		cur[0] = i;
		size_t row_min = cur[0];
		for (size_t j = 1; j <= m; ++j) {
			size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
			cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
			row_min = std::min(row_min, cur[j]);
		}
		if (row_min > max_dist) return false;
		std::swap(prev, cur);
	}
	dist = prev[m];
	return dist <= max_dist;
}

size_t EditDistance(std::vector<uint32_t> const& a, std::vector<uint32_t> const& b) {
	size_t dist = 0;
	DistanceWithin(a, b, std::max(a.size(), b.size()), dist);
	return dist;
}

} // namespace

std::string Normalize(std::string text, MatchOptions const& options) {
	// ASS line break escapes: literal \N and \n in the raw text
	ReplaceAll(text, "\\N", " ");
	ReplaceAll(text, "\\n", " ");

	if (options.ignore_tags) {
		text = StripBraceTags(text);
		text = StripAngleTags(text);
	}

	std::vector<uint32_t> cps = Utf8ToCodepoints(text);
	std::vector<uint32_t> out;
	bool last_space = false;
	for (uint32_t cp : cps) {
		if (IsZeroWidth(cp)) continue;
		if (options.ignore_punctuation && IsPunctuation(cp)) continue;
		if (options.ignore_case) cp = ToLower(cp);

		bool ws = IsAsciiSpace(cp) || IsUnicodeSpace(cp);
		if (ws) {
			if (!last_space && !out.empty()) {
				out.push_back(' ');
				last_space = true;
			}
		}
		else {
			out.push_back(cp);
			last_space = false;
		}
	}
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return CodepointsToUtf8(out);
}

double Similarity(std::string const& a, std::string const& b) {
	auto ca = Utf8ToCodepoints(a);
	auto cb = Utf8ToCodepoints(b);
	if (ca.empty() && cb.empty()) return 1.0;
	if (ca.empty() || cb.empty()) return 0.0;
	size_t maxlen = std::max(ca.size(), cb.size());
	size_t dist = EditDistance(ca, cb);
	return 1.0 - static_cast<double>(dist) / static_cast<double>(maxlen);
}

/// Split a single text at sentence endings (。.!?…) without splitting inside
/// "..." or "……". Whitespace after the ending is consumed.
static std::vector<std::string> SplitBySentences(std::string const& text) {
	std::vector<std::string> out;
	std::vector<uint32_t> cur;
	auto flush = [&] {
		std::string s = TrimAscii(CodepointsToUtf8(cur));
		if (!s.empty()) out.push_back(std::move(s));
		cur.clear();
	};

	auto cps = Utf8ToCodepoints(text);
	bool has_content = false;
	for (size_t k = 0; k < cps.size(); ++k) {
		uint32_t cp = cps[k];
		cur.push_back(cp);

		bool ascii_ender = cp == '.' || cp == '!' || cp == '?';
		bool cjk_ender = cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F;
		bool ellipsis = cp == 0x2026;
		if (!(ascii_ender || cjk_ender || ellipsis)) has_content = true;

		// Don't split inside "..." or "……"
		if (cur.size() >= 2 &&
			((cp == '.' && cur[cur.size() - 2] == '.') ||
			 (cp == 0x2026 && cur[cur.size() - 2] == 0x2026)))
			continue;

		bool next_ws = k + 1 >= cps.size() ||
			IsAsciiSpace(cps[k + 1]) || IsUnicodeSpace(cps[k + 1]);

		// Only split when the accumulated segment has actual content, so a
		// leading ellipsis such as "… this is..." is not split off.
		if (has_content && (cjk_ender || ((ascii_ender || ellipsis) && next_ws))) {
			flush();
			has_content = false;
			while (k + 1 < cps.size() &&
				(IsAsciiSpace(cps[k + 1]) || IsUnicodeSpace(cps[k + 1])))
				++k;
		}
	}
	flush();
	return out;
}

/// Split a text at every match of the regex, removing the matched text.
static std::vector<std::string> SplitByRegex(std::string const& text,
	std::regex const& re) {
	std::vector<std::string> out;
	std::string current;
	auto flush = [&] {
		std::string trimmed = TrimAscii(current);
		if (!trimmed.empty()) out.push_back(std::move(trimmed));
		current.clear();
	};

	size_t pos = 0;
	for (std::sregex_iterator it(text.begin(), text.end(), re), end;
		it != end; ++it) {
		size_t mpos = static_cast<size_t>(it->position());
		size_t mlen = static_cast<size_t>(it->length());
		if (mlen == 0) continue; // ignore zero-length matches
		current += text.substr(pos, mpos - pos);
		flush();
		pos = mpos + mlen;
	}
	current += text.substr(pos);
	flush();
	return out;
}

/// Internal splitter. Pass nullptr to disable regex splitting.
static std::vector<std::string> SplitSegmentsImpl(std::string const& text,
	bool split_sentences, std::regex const* split_regex) {
	std::vector<std::string> segments;
	std::string current;
	auto flush = [&] {
		std::string trimmed = TrimAscii(current);
		if (!trimmed.empty()) segments.push_back(std::move(trimmed));
		current.clear();
	};

	// Pass 1: line breaks/escapes and game-style {*N}/{*4-5} markers
	size_t i = 0;
	size_t n = text.size();
	while (i < n) {
		if (text[i] == '{') {
			size_t j = i + 1;
			if (j < n && text[j] == '*') ++j;
			size_t d = j;
			while (d < n && std::isdigit(static_cast<unsigned char>(text[d]))) ++d;
			// Range markers such as {*4-5} or {*4,5} are also boundaries.
			if (d > j && d < n && (text[d] == '-' || text[d] == ',')) {
				size_t e = d + 1;
				while (e < n && std::isdigit(static_cast<unsigned char>(text[e]))) ++e;
				if (e > d + 1 && e < n && text[e] == '}') {
					flush();
					i = e + 1;
					continue;
				}
			}
			if (d > j && d < n && text[d] == '}') {
				flush();
				i = d + 1;
				continue;
			}
		}
		if (text[i] == '\\' && i + 1 < n &&
			(text[i + 1] == 'N' || text[i + 1] == 'n')) {
			flush();
			i += 2;
			continue;
		}
		if (text[i] == '\n' || text[i] == '\r') {
			flush();
			++i;
			continue;
		}
		current += text[i];
		++i;
	}
	flush();

	// Pass 2: user-defined regex boundaries
	if (split_regex) {
		std::vector<std::string> split;
		for (auto const& seg : segments) {
			auto sub = SplitByRegex(seg, *split_regex);
			split.insert(split.end(), sub.begin(), sub.end());
		}
		segments = std::move(split);
	}

	// Pass 3: sentence boundaries
	if (split_sentences) {
		std::vector<std::string> split;
		for (auto const& seg : segments) {
			auto sub = SplitBySentences(seg);
			split.insert(split.end(), sub.begin(), sub.end());
		}
		segments = std::move(split);
	}

	if (segments.empty()) {
		std::string trimmed = TrimAscii(text);
		if (!trimmed.empty()) segments.push_back(std::move(trimmed));
	}
	return segments;
}

std::vector<std::string> SplitSegments(std::string const& text,
	bool split_sentences, std::string const& split_regex) {
	std::unique_ptr<std::regex> re;
	if (!split_regex.empty()) {
		try {
			re.reset(new std::regex(split_regex));
		}
		catch (std::regex_error const&) {
			// Invalid patterns are ignored.
		}
	}
	return SplitSegmentsImpl(text, split_sentences, re.get());
}

std::vector<MatchResult> Match(std::string const& subtitle_text,
	std::vector<LocalizationFile> const& files, MatchOptions const& options) {
	std::vector<MatchResult> results;
	std::string query = Normalize(subtitle_text, options);
	if (query.empty()) return results;

	std::unique_ptr<std::regex> split_regex;
	if (!options.split_regex.empty()) {
		try {
			split_regex.reset(new std::regex(options.split_regex));
		}
		catch (std::regex_error const&) {
			// Invalid patterns are ignored.
		}
	}

	struct Segment {
		std::string raw;
		std::string norm;
	};
	auto make_segments = [&](std::string const& text) {
		std::vector<Segment> segs;
		for (auto& raw : SplitSegmentsImpl(text, options.split_sentences, split_regex.get())) {
			std::string norm = Normalize(raw, options);
			if (!norm.empty())
				segs.push_back({std::move(raw), std::move(norm)});
		}
		return segs;
	};

	struct NormalizedItem {
		TextItem const* item;
		std::string norm_text;
		std::string norm_key;
		std::vector<Segment> segments;
	};
	struct NormalizedPair {
		BilingualRecord const* record;
		std::string norm_source;
		std::vector<Segment> source_segments;
		std::vector<Segment> translation_segments;
	};
	struct KeyEntry {
		std::string file;
		std::string text;
		std::vector<Segment> segments;
	};

	std::vector<NormalizedItem> items;
	std::vector<NormalizedPair> pairs;
	std::unordered_multimap<std::string, size_t> exact_index;
	// key -> entries in other files with the same key
	std::unordered_map<std::string, std::vector<KeyEntry>> key_map;

	for (auto const& f : files) {
		if (!f.ok) continue;
		for (auto const& it : f.items) {
			size_t idx = items.size();
			items.push_back({&it, Normalize(it.text, options),
				Normalize(it.key, options), make_segments(it.text)});
			if (!items.back().norm_text.empty())
				exact_index.emplace(items.back().norm_text, idx);
			if (!it.key.empty())
				key_map[it.key].push_back({f.name, it.text, make_segments(it.text)});
		}
		for (auto const& p : f.pairs) {
			pairs.push_back({&p, Normalize(p.source, options),
				make_segments(p.source), make_segments(p.translation)});
		}
	}

	auto query_cps = Utf8ToCodepoints(query);
	size_t qlen = query_cps.size();

	// Score a normalized string against the query, or -1 when below threshold.
	auto score_norm = [&](std::string const& norm) -> double {
		if (norm.empty()) return -1.0;
		if (norm == query) return 1.0;
		if (!options.fuzzy || options.threshold >= 1.0) return -1.0;

		auto nc = Utf8ToCodepoints(norm);
		size_t nlen = nc.size();
		size_t maxlen = std::max(qlen, nlen);
		size_t max_dist = static_cast<size_t>(
			std::floor((1.0 - options.threshold) * static_cast<double>(maxlen)));

		// Guard against meaningless matches of very short strings
		bool short_pair = std::min(qlen, nlen) <= 2 && qlen != nlen;
		bool contained = query.find(norm) != std::string::npos ||
			norm.find(query) != std::string::npos;
		if (short_pair && !contained) return -1.0;

		size_t dist = 0;
		if (!DistanceWithin(query_cps, nc, max_dist, dist)) return -1.0;
		double score = 1.0 - static_cast<double>(dist) / static_cast<double>(maxlen);
		return score + 1e-9 >= options.threshold ? score : -1.0;
	};

	std::set<std::string> seen;
	auto emit = [&](MatchResult const& r) {
		if (r.replacement.empty()) return;
		std::string dedup_key = r.file + "\x1F" + r.key + "\x1F" + r.replacement;
		if (!seen.insert(dedup_key).second) return;
		results.push_back(r);
	};

	auto emit_candidates = [&](TextItem const* item, std::string const& matched,
		int seg_index, std::string const& direct_replacement,
		std::string const& direct_origin, bool from_key, double score, bool exact) {
		MatchResult base;
		base.score = score;
		base.exact = exact;
		base.from_key = from_key;
		base.file = item->file;
		base.key = item->key;
		base.matched = matched;

		// The entry itself is only useful when it differs from the line text
		// (e.g. it is a cleaner version); otherwise it is just noise.
		if (direct_replacement != subtitle_text) {
			MatchResult self = base;
			self.replacement = direct_replacement;
			self.origin = direct_origin;
			emit(self);
		}

		if (item->key.empty()) return;
		auto it = key_map.find(item->key);
		if (it == key_map.end()) return;
		for (auto const& entry : it->second) {
			if (entry.file == item->file) continue;

			std::string replacement;
			std::string origin;
			if (seg_index >= 0 &&
				static_cast<size_t>(seg_index) < entry.segments.size()) {
				replacement = entry.segments[seg_index].raw;
				origin = "segment " + std::to_string(seg_index + 1) + " of " +
					entry.file + " (key \"" + item->key + "\")";
			}
			else {
				replacement = entry.text;
				origin = "key \"" + item->key + "\" in " + entry.file;
			}
			if (replacement.empty() || replacement == direct_replacement) continue;

			MatchResult cross = base;
			cross.file = entry.file;
			cross.replacement = replacement;
			cross.origin = origin;
			emit(cross);
		}
	};

	auto add_exact = [&](TextItem const* item) {
		emit_candidates(item, item->text, -1, item->text, item->file, false, 1.0, true);
	};

	auto add_fuzzy = [&](TextItem const* item, double score) {
		emit_candidates(item, item->text, -1, item->text, item->file, false, score, false);
	};

	auto add_key_match = [&](TextItem const* item, double score, bool exact) {
		emit_candidates(item, item->key, -1, item->text,
			"key \"" + item->key + "\" -> value in " + item->file, true, score, exact);
	};

	// Exact text matches
	auto exact_range = exact_index.equal_range(query);
	for (auto it = exact_range.first; it != exact_range.second; ++it)
		add_exact(items[it->second].item);

	// Exact key matches
	for (auto const& ni : items) {
		if (ni.norm_key.size() >= 2 && ni.norm_key == query)
			add_key_match(ni.item, 1.0, true);
	}

	// Fuzzy whole-string matches
	for (auto const& ni : items) {
		if (ni.norm_text.empty() || ni.norm_text == query) continue;
		double score = score_norm(ni.norm_text);
		if (score >= 0)
			add_fuzzy(ni.item, score);
	}

	// Fuzzy key matches (longer keys only)
	for (auto const& ni : items) {
		std::string const& norm_key = ni.norm_key;
		if (norm_key.size() < 4 || norm_key == query) continue;
		double score = score_norm(norm_key);
		if (score >= 0)
			add_key_match(ni.item, score, false);
	}

	// Bilingual record sources (whole string)
	for (auto const& np : pairs) {
		double score = score_norm(np.norm_source);
		if (score < 0) continue;
		MatchResult r;
		r.score = score;
		r.exact = score == 1.0;
		r.file = np.record->file;
		r.key = np.record->key;
		r.matched = np.record->source;
		r.replacement = np.record->translation;
		r.origin = "inline translation in " + np.record->file;
		emit(r);
	}

	// Segment matches on item text; align the matched segment with the same
	// segment of same-key entries in other files.
	for (auto const& ni : items) {
		int best_k = -1;
		double best_score = -1.0;
		bool best_exact = false;
		for (size_t k = 0; k < ni.segments.size(); ++k) {
			double s = score_norm(ni.segments[k].norm);
			if (s < 0) continue;
			if (s > best_score) {
				best_score = s;
				best_k = static_cast<int>(k);
				best_exact = s == 1.0;
			}
		}
		if (best_k < 0) continue;
		auto const& seg = ni.segments[best_k];
		emit_candidates(ni.item, seg.raw, best_k, seg.raw,
			"segment " + std::to_string(best_k + 1) + " of " + ni.item->file,
			false, best_score, best_exact);
	}

	// Segment matches on bilingual record sources; align with the matching
	// segment of the inline translation.
	for (auto const& np : pairs) {
		for (size_t k = 0; k < np.source_segments.size(); ++k) {
			double score = score_norm(np.source_segments[k].norm);
			if (score < 0) continue;

			MatchResult r;
			r.score = score;
			r.exact = score == 1.0;
			r.file = np.record->file;
			r.key = np.record->key;
			r.matched = np.source_segments[k].raw;
			if (k < np.translation_segments.size()) {
				r.replacement = np.translation_segments[k].raw;
				r.origin = "segment " + std::to_string(k + 1) +
					" of inline translation in " + np.record->file;
			}
			else {
				r.replacement = np.record->translation;
				r.origin = "inline translation in " + np.record->file;
			}
			emit(r);
		}
	}

	// Results from files whose detected language matches the preferred
	// language are listed first.
	std::unordered_map<std::string, std::string> file_languages;
	if (!options.preferred_language.empty()) {
		for (auto const& file : files)
			if (!file.name.empty())
				file_languages.emplace(file.name, file.language);
	}

	std::sort(results.begin(), results.end(),
		[&](MatchResult const& a, MatchResult const& b) {
			if (!options.preferred_language.empty()) {
				auto it_a = file_languages.find(a.file);
				auto it_b = file_languages.find(b.file);
				bool pa = it_a != file_languages.end() &&
					it_a->second == options.preferred_language;
				bool pb = it_b != file_languages.end() &&
					it_b->second == options.preferred_language;
				if (pa != pb) return pa;
			}
			if (a.exact != b.exact) return a.exact;
			if (a.score != b.score) return a.score > b.score;
			if (a.from_key != b.from_key) return !a.from_key;
			if (a.file != b.file) return a.file < b.file;
			if (a.key != b.key) return a.key < b.key;
			return a.replacement < b.replacement;
		});

	constexpr size_t max_results = 300;
	if (results.size() > max_results) results.resize(max_results);
	return results;
}

} // namespace localization
