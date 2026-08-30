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

#include "hardsub_scan.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <map>

namespace hardsub {

RegionImage CropRegion(VideoFrame const& frame, int x, int y, int width, int height) {
	RegionImage out;
	if (frame.width == 0 || frame.height == 0 || frame.pitch < frame.width)
		return out;
	if (frame.pix_fmt == 0) {
		if (frame.pitch < frame.width * 4
		    || frame.data.size() < frame.pitch * frame.height)
			return out;
	}
	else {
		// YUV 4:2:0: luma plane plus two half-resolution chroma planes.
		size_t chroma_rows = frame.height / 2;
		size_t chroma_stride = frame.pix_fmt == 1 ? frame.pitch / 2 : frame.pitch;
		if (frame.pitch < frame.width
		    || frame.data.size() < frame.pitch * frame.height + chroma_stride * chroma_rows * (frame.pix_fmt == 1 ? 2 : 1))
			return out;
	}

	int x0 = std::max(0, x);
	int y0 = std::max(0, y);
	int x1 = std::min<int>(static_cast<int>(frame.width), x + width);
	int y1 = std::min<int>(static_cast<int>(frame.height), y + height);
	if (x1 <= x0 || y1 <= y0)
		return out;

	out.width = x1 - x0;
	out.height = y1 - y0;
	out.rgb.resize(size_t(out.width) * out.height * 3);

	// The scan decoder can hand us raw YUV 4:2:0 frames (8- or 10-bit) to
	// avoid the full-frame BGRA conversion that dominates the cost of the
	// normal provider path on 4K video. Convert only the requested region.
	if (frame.pix_fmt == 1 || frame.pix_fmt == 2 || frame.pix_fmt == 3) {
		const bool is_planar = frame.pix_fmt == 1;
		const bool is_10bit = frame.pix_fmt == 3;
		const size_t y_plane = 0;
		const size_t u_plane = frame.pitch * frame.height;
		const size_t v_plane = u_plane + (frame.pitch / 2) * (frame.height / 2);
		const size_t y_stride = frame.pitch;
		const size_t uv_stride = is_planar ? frame.pitch / 2 : frame.pitch;

		size_t out_pos = 0;
		for (int row = y0; row < y1; ++row) {
			int src_row = frame.flipped ? int(frame.height) - 1 - row : row;
			const unsigned char *y_row = frame.data.data() + y_plane
				+ size_t(src_row) * y_stride;
			for (int col = x0; col < x1; ++col) {
				int Y;
				if (is_10bit) {
					uint16_t yv = *reinterpret_cast<uint16_t const*>(
						y_row + size_t(col) * 2);
					Y = static_cast<int>((yv >> 6) * 255 / 1023);
				}
				else {
					Y = y_row[col];
				}
				unsigned char U, V;
				if (is_planar) {
					int cu = ((src_row / 2) * static_cast<int>(uv_stride) + (col / 2));
					U = is_10bit
						? static_cast<unsigned char>((*reinterpret_cast<uint16_t const*>(
						    frame.data.data() + u_plane + cu * 2) >> 6) * 255 / 1023)
						: frame.data[u_plane + cu];
					V = is_10bit
						? static_cast<unsigned char>((*reinterpret_cast<uint16_t const*>(
						    frame.data.data() + v_plane + cu * 2) >> 6) * 255 / 1023)
						: frame.data[v_plane + cu];
				}
				else {
					int cu = ((src_row / 2) * static_cast<int>(uv_stride)
					          + (col / 2) * (is_10bit ? 4 : 2));
					const unsigned char *uv_src = frame.data.data() + u_plane + cu;
					U = is_10bit
						? static_cast<unsigned char>((*reinterpret_cast<uint16_t const*>(uv_src) >> 6) * 255 / 1023)
						: uv_src[0];
					V = is_10bit
						? static_cast<unsigned char>((*reinterpret_cast<uint16_t const*>(uv_src + 2) >> 6) * 255 / 1023)
						: uv_src[1];
				}
				// BT.709 limited-range YCbCr -> RGB
				double yf = (Y - 16) * 1.164;
				double cb = U - 128.0;
				double cr = V - 128.0;
				int r = static_cast<int>(yf + 1.575 * cr + 0.5);
				int g = static_cast<int>(yf - 0.187 * cb - 0.468 * cr + 0.5);
				int b = static_cast<int>(yf + 1.793 * cb + 0.5);
				out.rgb[out_pos++] = static_cast<unsigned char>(std::clamp(r, 0, 255));
				out.rgb[out_pos++] = static_cast<unsigned char>(std::clamp(g, 0, 255));
				out.rgb[out_pos++] = static_cast<unsigned char>(std::clamp(b, 0, 255));
			}
		}
		return out;
	}

	size_t out_pos = 0;
	for (int row = y0; row < y1; ++row) {
		int src_row = frame.flipped ? int(frame.height) - 1 - row : row;
		const unsigned char *src = frame.data.data() + size_t(src_row) * frame.pitch + size_t(x0) * 4;
		for (int col = x0; col < x1; ++col) {
			out.rgb[out_pos++] = src[2]; // R
			out.rgb[out_pos++] = src[1]; // G
			out.rgb[out_pos++] = src[0]; // B
			src += 4;
		}
	}
	return out;
}

double MeanAbsDiff(RegionImage const& a, RegionImage const& b) {
	if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height)
		return 255.0;

	unsigned long long total = 0;
	for (size_t i = 0; i < a.rgb.size(); ++i) {
		int d = int(a.rgb[i]) - int(b.rgb[i]);
		if (d < 0) d = -d;
		total += d;
	}
	return double(total) / (a.rgb.size() * 255.0) * 255.0;
}

double RegionChangedRatio(RegionImage const& a, RegionImage const& b, int pixel_threshold) {
	if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height)
		return 1.0;

	size_t changed = 0;
	for (size_t i = 0; i < a.rgb.size(); i += 3) {
		int dr = int(a.rgb[i]) - int(b.rgb[i]);
		int dg = int(a.rgb[i + 1]) - int(b.rgb[i + 1]);
		int db = int(a.rgb[i + 2]) - int(b.rgb[i + 2]);
		int d = std::max({std::abs(dr), std::abs(dg), std::abs(db)});
		if (d > pixel_threshold)
			++changed;
	}
	return double(changed) / (a.rgb.size() / 3);
}

std::vector<int> TemplateTextPixels(RegionImage const& tpl, int deviation) {
	std::vector<int> out;
	if (!tpl.Valid())
		return out;

	auto luminance = [](unsigned char r, unsigned char g, unsigned char b) {
		return (static_cast<int>(r) + static_cast<int>(g) + static_cast<int>(b)) / 3;
	};

	// Dominant background luminance: the mode of a coarse histogram.
	constexpr int kNumBins = 32;
	constexpr int kBinWidth = 256 / kNumBins;
	std::vector<int> bins(kNumBins, 0);
	for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
		int l = luminance(tpl.rgb[i], tpl.rgb[i + 1], tpl.rgb[i + 2]);
		++bins[std::min(kNumBins - 1, l / kBinWidth)];
	}

	int mode_bin = 0;
	for (int b = 1; b < kNumBins; ++b) {
		if (bins[b] > bins[mode_bin])
			mode_bin = b;
	}

	// Estimate the background color from the pixels in the dominant luminance
	// bin, then keep every pixel that differs from it strongly in any channel.
	// A per-channel test catches saturated text (e.g. yellow) that a pure
	// luminance test would miss.
	int bg_r = 0, bg_g = 0, bg_b = 0, bg_count = 0;
	int bin_lo = mode_bin * kBinWidth;
	int bin_hi = std::min(255, bin_lo + kBinWidth);
	for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
		int l = luminance(tpl.rgb[i], tpl.rgb[i + 1], tpl.rgb[i + 2]);
		if (l >= bin_lo && l < bin_hi) {
			bg_r += tpl.rgb[i];
			bg_g += tpl.rgb[i + 1];
			bg_b += tpl.rgb[i + 2];
			++bg_count;
		}
	}
	if (bg_count > 0) {
		bg_r /= bg_count;
		bg_g /= bg_count;
		bg_b /= bg_count;
	}

	// Subtitle ink is usually near-white or near-black, standing out from the
	// scene. Selecting pixels by luminance extremes (relative to the
	// background) instead of "differs from the background" avoids picking up
	// scene texture, which would make the coverage unstable under camera
	// motion: scene pixels move with the camera, while burned-in text stays
	// put. A moving scene can otherwise make the coverage fluctuate so much
	// that high text-match thresholds split the run.
	int bg_lum = bg_count > 0 ? (bg_r + bg_g + bg_b) / 3 : 0;
	int bright_thresh = std::clamp(bg_lum + 80, 195, 235);
	int dark_thresh = std::clamp(bg_lum - 80, 25, 60);
	bool use_bright = bg_lum < 140;
	bool use_dark = bg_lum > 200;

	for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
		int l = luminance(tpl.rgb[i], tpl.rgb[i + 1], tpl.rgb[i + 2]);
		bool text = false;
		if (use_bright)
			text = l >= bright_thresh;
		else if (use_dark)
			text = l <= dark_thresh;
		else
			text = l >= bright_thresh || l <= dark_thresh;
		if (text)
			out.push_back(static_cast<int>(i / 3));
	}

	// Colored text on a mid-luminance scene may not reach the luminance
	// extremes; fall back to the background-deviation test in that case.
	if (out.size() < tpl.rgb.size() / 3 / 64) {
		out.clear();
		for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
			int dr = std::abs(static_cast<int>(tpl.rgb[i]) - bg_r);
			int dg = std::abs(static_cast<int>(tpl.rgb[i + 1]) - bg_g);
			int db = std::abs(static_cast<int>(tpl.rgb[i + 2]) - bg_b);
			if (std::max({dr, dg, db}) >= deviation)
				out.push_back(static_cast<int>(i / 3));
		}
	}
	return out;
}

double TemplateTextCoverage(RegionImage const& tpl, RegionImage const& frame,
                            std::vector<int> const& text_pixels, int match_threshold) {
	if (!tpl.Valid() || !frame.Valid() || tpl.width != frame.width || tpl.height != frame.height
	    || text_pixels.empty())
		return 0.0;

	int matched = 0;
	for (int px : text_pixels) {
		size_t off = static_cast<size_t>(px) * 3;
		int d = 0;
		for (int c = 0; c < 3; ++c) {
			int diff = std::abs(static_cast<int>(tpl.rgb[off + c]) - static_cast<int>(frame.rgb[off + c]));
			d = std::max(d, diff);
		}
		if (d <= match_threshold)
			++matched;
	}
	return double(matched) / double(text_pixels.size());
}

std::vector<bool> ComputePresence(std::vector<double> const& coverage,
                                  std::vector<bool> const& ocr_box,
                                  double threshold, double threshold_exit) {
	size_t n = coverage.size();
	if (!ocr_box.empty() && ocr_box.size() != n)
		return {};

	std::vector<bool> presence(n, false);
	bool state = false;
	for (size_t i = 0; i < n; ++i) {
		double cov = coverage[i];
		if (cov < 0.0) {
			// Unevaluated frame: keep the previous state.
			presence[i] = state;
			continue;
		}
		bool cov_present;
		if (cov >= threshold)
			cov_present = true;
		else if (cov < threshold_exit)
			cov_present = false;
		else
			cov_present = state; // hysteresis zone: hold the current state
		state = cov_present && (ocr_box.empty() || ocr_box[i]);
		presence[i] = state;
	}
	return presence;
}

std::vector<bool> FillShortDips(std::vector<bool> const& present, int confirm_frames) {
	if (confirm_frames <= 0)
		return present;

	size_t n = present.size();
	std::vector<bool> filled = present;
	size_t i = 0;
	while (i < n) {
		if (filled[i]) {
			++i;
			continue;
		}
		size_t j = i;
		while (j < n && !filled[j])
			++j;
		// Only interior runs are filled; the window edges stay absent so a
		// run can never extend past the evaluated evidence.
		if (i > 0 && j < n && j - i < size_t(confirm_frames))
			std::fill(filled.begin() + i, filled.begin() + j, true);
		i = j;
	}
	return filled;
}

int RunStart(std::vector<bool> const& present, int anchor) {
	if (anchor < 0 || anchor >= static_cast<int>(present.size()) || !present[anchor])
		return anchor;
	int start = anchor;
	while (start > 0 && present[start - 1])
		--start;
	return start;
}

int RunEnd(std::vector<bool> const& present, int anchor) {
	if (anchor < 0 || anchor >= static_cast<int>(present.size()) || !present[anchor])
		return anchor;
	int end = anchor;
	while (end + 1 < static_cast<int>(present.size()) && present[end + 1])
		++end;
	return end;
}

int SnapBoundaryFromEvidence(std::vector<double> const& coverage,
                             std::vector<bool> const& ocr_box,
                             double threshold_exit, bool find_earliest, int fallback) {
	if (coverage.size() != ocr_box.size() || coverage.empty())
		return fallback;

	if (find_earliest) {
		for (size_t i = 0; i < coverage.size(); ++i) {
			if (coverage[i] >= threshold_exit && ocr_box[i])
				return static_cast<int>(i);
		}
	}
	else {
		for (size_t i = coverage.size(); i-- > 0;) {
			if (coverage[i] >= threshold_exit && ocr_box[i])
				return static_cast<int>(i);
		}
	}
	return fallback;
}

bool BoundaryAmbiguous(ScanResult const& result, double threshold_exit, bool start) {
	if (!result.ok)
		return true;

	int boundary = start ? result.start_frame : result.end_frame;
	auto it = std::lower_bound(result.frames.begin(), result.frames.end(), boundary);
	if (it == result.frames.end() || *it != boundary)
		return true; // the series is not dense enough to tell

	size_t idx = static_cast<size_t>(it - result.frames.begin());
	for (size_t off = 1; off <= 2; ++off) {
		size_t at = start ? idx - off : idx + off;
		if (at >= result.frames.size())
			break;
		int f = start ? boundary - static_cast<int>(off) : boundary + static_cast<int>(off);
		if (result.frames[at] == f && result.diffs[at] >= threshold_exit)
			return true;
	}
	return false;
}

ScanResult ScanBoundaries(RegionImage const& tpl, int tpl_x, int tpl_y,
                          int base_frame, int frame_count,
                          ScanOptions const& opts,
                          FrameLoader const& loader,
                          CancelFn const& cancel,
                          ProgressFn const& progress,
                          std::vector<int> const& keyframes) {
	ScanResult out;
	if (!tpl.Valid() || frame_count <= 0 || base_frame < 0 || base_frame >= frame_count || !loader)
		return out;

	auto text_pixels = TemplateTextPixels(tpl);
	if (text_pixels.empty()) {
		out.error = "The selected region has no text-like pixels. Draw the box tighter around the subtitle.";
		return out;
	}

	double enter = std::clamp(opts.threshold, 0.0, 1.0);
	double exit = opts.threshold_exit > 0.0
		? std::clamp(opts.threshold_exit, 0.0, enter)
		: enter * 0.75;
	int confirm = std::max(0, opts.confirm_frames);

	out.base_frame = base_frame;
	int lo_bound = opts.max_frames > 0 ? std::max(0, base_frame - opts.max_frames) : 0;
	int hi_bound = opts.max_frames > 0 ? std::min(frame_count - 1, base_frame + opts.max_frames) : frame_count - 1;
	if (hi_bound < lo_bound)
		return out;

	std::map<int, double> coverage_cache;
	auto frame_coverage = [&](int f) -> double {
		auto it = coverage_cache.find(f);
		if (it != coverage_cache.end())
			return it->second;
		auto frame = loader(f);
		if (!frame)
			return -1.0;
		auto crop = CropRegion(*frame, tpl_x, tpl_y, tpl.width, tpl.height);
		if (!crop.Valid())
			return -1.0;
		double coverage = TemplateTextCoverage(tpl, crop, text_pixels);
		coverage_cache.emplace(f, coverage);
		return coverage;
	};

	// ---- Locate a pass start before the run ---------------------------------
	// Random frame access on modern codecs (e.g. 4K/AV1) can cost about a
	// second per seek because a whole GOP must be decoded, while sequential
	// decoding is a few milliseconds per frame. The scan therefore avoids
	// arbitrary-frame probing entirely: it starts one contiguous ascending
	// pass at a keyframe before the run and extends that start backward
	// through keyframes only while the coverage there still looks like the
	// subtitle (a fade-in, or a run that began before the initial start).
	constexpr int kFallbackPreRoll = 120; // frames, when no keyframe list is given
	int pass_start;
	if (!keyframes.empty()) {
		auto it = std::upper_bound(keyframes.begin(), keyframes.end(), base_frame);
		if (it == keyframes.begin())
			pass_start = base_frame < keyframes.front() ? lo_bound : keyframes.front();
		else
			pass_start = *std::prev(it);
	}
	else {
		pass_start = std::max(lo_bound, base_frame - kFallbackPreRoll);
	}
	if (pass_start > base_frame)
		pass_start = base_frame;

	int evaluated = 0;
	auto progress_step = [&] {
		++evaluated;
		// Throttle UI progress events; a sequential pass evaluates frames
		// far too quickly to post one event per frame.
		if (progress && (evaluated % 16) == 0)
			progress(evaluated, evaluated);
	};

	// Extend the pass start backward while the coverage there is still
	// subtitle-like. Every probe lands on a keyframe, which is a cheap seek
	// (decode one frame) rather than a full GOP decode.
	for (;;) {
		if (cancel && cancel())
			return out;
		double cov = frame_coverage(pass_start);
		if (cov < 0.0)
			return out;
		progress_step();
		if (cov < exit)
			break; // clearly absent: the run starts after this frame
		int next;
		if (!keyframes.empty()) {
			auto it = std::lower_bound(keyframes.begin(), keyframes.end(), pass_start);
			next = it == keyframes.begin() ? -1 : *std::prev(it);
		}
		else {
			next = std::max(lo_bound, pass_start - kFallbackPreRoll);
		}
		if (next < 0 || next >= pass_start) {
			pass_start = lo_bound;
			break;
		}
		pass_start = next;
	}

	// ---- Sequential pass over the run ---------------------------------------
	// Decode forward from the pass start, recording coverage for every frame,
	// until the run has clearly ended after the base frame. The dense series
	// feeds the state machine below, which yields exact boundaries and splits
	// the run at interior gaps.
	const int kConfirmTail = std::max(4, confirm + 2);
	int absent_run = 0;
	int pass_end = pass_start;
	for (int f = pass_start; f <= hi_bound; ++f) {
		if (cancel && cancel())
			return out;
		double cov = frame_coverage(f);
		if (cov < 0.0)
			return out;
		progress_step();
		pass_end = f;
		if (cov < exit) {
			++absent_run;
			if (f > base_frame && absent_run >= kConfirmTail)
				break;
		}
		else {
			// Present or in the hysteresis zone: not a confirmed gap, so the
			// pass keeps going (a fade-out tail is not cut short, and a brief
			// occlusion does not end the run).
			absent_run = 0;
		}
	}

	// ---- Boundaries from the pass series ------------------------------------
	std::vector<double> cov;
	cov.reserve(pass_end - pass_start + 1);
	for (int f = pass_start; f <= pass_end; ++f)
		cov.push_back(coverage_cache.at(f));
	auto present = ComputePresence(cov, {}, enter, exit);
	auto filled = FillShortDips(present, confirm);
	int anchor = base_frame - pass_start;
	if (anchor < 0 || anchor >= static_cast<int>(filled.size()) || !filled[anchor])
		return out;
	int start = pass_start + RunStart(filled, anchor);
	int end = pass_start + RunEnd(filled, anchor);

	if (start < lo_bound || end > hi_bound || start > end || base_frame < start || base_frame > end)
		return out;

	int duration = end - start + 1;
	if (opts.min_duration_frames > 0 && duration < opts.min_duration_frames)
		return out;

	out.ok = true;
	out.start_frame = start;
	out.end_frame = end;
	// Report only the dense pass range so callers can rely on a contiguous
	// evidence series (interior-dip detection, boundary ambiguity checks).
	out.frames.reserve(pass_end - pass_start + 1);
	out.diffs.reserve(pass_end - pass_start + 1);
	for (int f = pass_start; f <= pass_end; ++f) {
		out.frames.push_back(f);
		out.diffs.push_back(coverage_cache.at(f));
	}
	return out;
}

} // namespace hardsub
