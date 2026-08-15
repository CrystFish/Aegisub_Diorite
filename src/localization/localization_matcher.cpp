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
/// @brief Normalization and fuzzy matching for localization entries.

#include "localization_matcher.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

std::vector<MatchResult> Match(std::string const& subtitle_text,
	std::vector<LocalizationFile> const& files, MatchOptions const& options) {
	std::vector<MatchResult> results;
	std::string query = Normalize(subtitle_text, options);
	if (query.empty()) return results;

	struct NormalizedItem {
		TextItem const* item;
		std::string norm_text;
		std::string norm_key;
	};
	struct NormalizedPair {
		BilingualRecord const* record;
		std::string norm_source;
	};

	std::vector<NormalizedItem> items;
	std::vector<NormalizedPair> pairs;
	std::unordered_multimap<std::string, size_t> exact_index;
	// key -> entries in other files with the same key
	std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> key_map;

	for (auto const& f : files) {
		if (!f.ok) continue;
		for (auto const& it : f.items) {
			size_t idx = items.size();
			items.push_back({&it, Normalize(it.text, options), Normalize(it.key, options)});
			if (!items.back().norm_text.empty())
				exact_index.emplace(items.back().norm_text, idx);
			if (!it.key.empty())
				key_map[it.key].emplace_back(f.name, it.text);
		}
		for (auto const& p : f.pairs)
			pairs.push_back({&p, Normalize(p.source, options)});
	}

	auto query_cps = Utf8ToCodepoints(query);
	size_t qlen = query_cps.size();

	std::set<std::string> seen;
	auto emit = [&](MatchResult const& r) {
		if (r.replacement.empty()) return;
		std::string dedup_key = r.file + "\x1F" + r.key + "\x1F" + r.replacement +
			"\x1F" + r.origin;
		if (!seen.insert(dedup_key).second) return;
		results.push_back(r);
	};

	auto emit_candidates = [&](TextItem const* item, std::string const& matched,
		std::string const& direct_replacement, std::string const& direct_origin,
		bool from_key, double score, bool exact) {
		MatchResult base;
		base.score = score;
		base.exact = exact;
		base.from_key = from_key;
		base.file = item->file;
		base.key = item->key;
		base.matched = matched;

		MatchResult self = base;
		self.replacement = direct_replacement;
		self.origin = direct_origin;
		// The entry itself is only useful when it differs from the line text
		// (e.g. it is a cleaner version); otherwise it is just noise.
		if (direct_replacement != subtitle_text)
			emit(self);

		if (!item->key.empty()) {
			auto it = key_map.find(item->key);
			if (it != key_map.end()) {
				for (auto const& entry : it->second) {
					if (entry.first == item->file || entry.second == direct_replacement)
						continue;
					MatchResult cross = base;
					cross.replacement = entry.second;
					cross.origin = "key \"" + item->key + "\" in " + entry.first;
					emit(cross);
				}
			}
		}
	};

	auto add_exact = [&](TextItem const* item) {
		emit_candidates(item, item->text, item->text, item->file, false, 1.0, true);
	};

	auto add_fuzzy = [&](TextItem const* item, double score) {
		emit_candidates(item, item->text, item->text, item->file, false, score, false);
	};

	auto add_key_match = [&](TextItem const* item, double score, bool exact) {
		emit_candidates(item, item->key, item->text,
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

	// Fuzzy matches
	if (options.fuzzy && options.threshold < 1.0) {
		for (auto const& ni : items) {
			std::string const& norm_text = ni.norm_text;
			if (norm_text.empty() || norm_text == query) continue;

			auto ntc = Utf8ToCodepoints(norm_text);
			size_t nlen = ntc.size();
			size_t maxlen = std::max(qlen, nlen);
			size_t max_dist = static_cast<size_t>(
				std::floor((1.0 - options.threshold) * static_cast<double>(maxlen)));

			// Guard against meaningless matches of very short strings
			bool short_pair = std::min(qlen, nlen) <= 2 && qlen != nlen;
			bool contained = query.find(norm_text) != std::string::npos ||
				norm_text.find(query) != std::string::npos;
			if (short_pair && !contained) continue;

			size_t dist = 0;
			if (!DistanceWithin(query_cps, ntc, max_dist, dist)) continue;
			double score = 1.0 - static_cast<double>(dist) / static_cast<double>(maxlen);
			if (score + 1e-9 >= options.threshold)
				add_fuzzy(ni.item, score);
		}

		// Fuzzy key matches (longer keys only)
		for (auto const& ni : items) {
			std::string const& norm_key = ni.norm_key;
			if (norm_key.size() < 4 || norm_key == query) continue;
			auto nkc = Utf8ToCodepoints(norm_key);
			size_t maxlen = std::max(qlen, nkc.size());
			size_t max_dist = static_cast<size_t>(
				std::floor((1.0 - options.threshold) * static_cast<double>(maxlen)));
			size_t dist = 0;
			if (!DistanceWithin(query_cps, nkc, max_dist, dist)) continue;
			double score = 1.0 - static_cast<double>(dist) / static_cast<double>(maxlen);
			if (score + 1e-9 >= options.threshold)
				add_key_match(ni.item, score, false);
		}

		// Bilingual record sources
		for (auto const& np : pairs) {
			if (np.norm_source.empty()) continue;
			auto nsc = Utf8ToCodepoints(np.norm_source);
			size_t maxlen = std::max(qlen, nsc.size());
			size_t max_dist = static_cast<size_t>(
				std::floor((1.0 - options.threshold) * static_cast<double>(maxlen)));
			size_t dist = 0;
			if (!DistanceWithin(query_cps, nsc, max_dist, dist)) continue;
			double score = 1.0 - static_cast<double>(dist) / static_cast<double>(maxlen);
			if (score + 1e-9 < options.threshold) continue;

			MatchResult r;
			r.score = score;
			r.exact = dist == 0;
			r.file = np.record->file;
			r.key = np.record->key;
			r.matched = np.record->source;
			r.replacement = np.record->translation;
			r.origin = "inline translation in " + np.record->file;
			emit(r);
		}
	}
	else {
		// Exact-only bilingual sources
		for (auto const& np : pairs) {
			if (np.norm_source == query) {
				MatchResult r;
				r.score = 1.0;
				r.exact = true;
				r.file = np.record->file;
				r.key = np.record->key;
				r.matched = np.record->source;
				r.replacement = np.record->translation;
				r.origin = "inline translation in " + np.record->file;
				emit(r);
			}
		}
	}

	std::sort(results.begin(), results.end(), [](MatchResult const& a, MatchResult const& b) {
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
