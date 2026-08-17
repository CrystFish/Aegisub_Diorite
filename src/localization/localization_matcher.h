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

#include "localization_types.h"

namespace localization {

/// Normalize text for comparison: strips ASS/HTML-like tags, line break
/// escapes, whitespace, and optionally punctuation and case.
std::string Normalize(std::string text, MatchOptions const& options);

/// Similarity between two already-normalized strings, 0..1
/// (1 - normalized Levenshtein distance).
double Similarity(std::string const& a, std::string const& b);

/// Split text into segments at line breaks, game-style {*N} markers, optional
/// regex matches, and (when enabled) sentence boundaries. Returns at least one
/// segment; a single segment is returned when no reliable boundaries are
/// found.
/// @param split_sentences Also split at sentence endings (。.!?…)
/// @param split_regex     Optional regex; every match becomes a boundary and
///                        the matched text is removed from the segments
std::vector<std::string> SplitSegments(std::string const& text,
	bool split_sentences = true, std::string const& split_regex = {});

} // namespace localization
