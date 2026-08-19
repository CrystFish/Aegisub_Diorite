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
#include <map>

namespace hardsub {

RegionImage CropRegion(VideoFrame const& frame, int x, int y, int width, int height) {
	RegionImage out;
	if (frame.width == 0 || frame.height == 0 || frame.pitch < frame.width * 4 ||
		frame.data.size() < frame.pitch * frame.height)
		return out;

	int x0 = std::max(0, x);
	int y0 = std::max(0, y);
	int x1 = std::min<int>(static_cast<int>(frame.width), x + width);
	int y1 = std::min<int>(static_cast<int>(frame.height), y + height);
	if (x1 <= x0 || y1 <= y0)
		return out;

	out.width = x1 - x0;
	out.height = y1 - y0;
	out.rgb.resize(size_t(out.width) * out.height * 3);

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

	// Dominant background luminance: the mode of a coarse histogram. Text is
	// normally a minority of the region, so the mode is the background and the
	// outliers are the subtitle ink (plus its outline).
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

	for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
		int dr = std::abs(static_cast<int>(tpl.rgb[i]) - bg_r);
		int dg = std::abs(static_cast<int>(tpl.rgb[i + 1]) - bg_g);
		int db = std::abs(static_cast<int>(tpl.rgb[i + 2]) - bg_b);
		if (std::max({dr, dg, db}) >= deviation)
			out.push_back(static_cast<int>(i / 3));
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

ScanResult ScanBoundaries(RegionImage const& tpl, int tpl_x, int tpl_y,
                          int base_frame, int frame_count,
                          ScanOptions const& opts,
                          FrameLoader const& loader,
                          CancelFn const& cancel,
                          ProgressFn const& progress) {
	ScanResult out;
	if (!tpl.Valid() || frame_count <= 0 || base_frame < 0 || base_frame >= frame_count || !loader)
		return out;

	auto text_pixels = TemplateTextPixels(tpl);
	if (text_pixels.empty()) {
		out.error = "The selected region has no text-like pixels. Draw the box tighter around the subtitle.";
		return out;
	}

	out.base_frame = base_frame;
	int stride = std::max(1, opts.stride);
	// max_frames is an optional safety cap; 0 means search to the video edges.
	int lo_bound = opts.max_frames > 0 ? std::max(0, base_frame - opts.max_frames) : 0;
	int hi_bound = opts.max_frames > 0 ? std::min(frame_count - 1, base_frame + opts.max_frames) : frame_count - 1;
	if (hi_bound < lo_bound)
		return out;

	int confirm = std::max(0, opts.confirm_frames);
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

	// The template comes from the base frame; if the subtitle is no longer
	// visible there (e.g. the video moved between selection and scan), there is
	// nothing to scan.
	if (frame_coverage(base_frame) < opts.threshold)
		return out;

	int evaluated = 0;
	auto progress_step = [&] {
		++evaluated;
		if (progress)
			progress(evaluated, evaluated);
	};

	// ---- Backward side -----------------------------------------------------
	// Walk outward at `stride` until the run clearly ends. With stride 1 every
	// frame is examined, so any gap at least `confirm` frames wide splits the
	// run and the boundary is exact; larger strides skip frames between
	// samples (faster, but miss gaps narrower than the step). The frame-by-frame
	// refinement below restores exact boundary precision around the transition.
	int anchor_back = base_frame; // last sampled frame still present
	int absent_back = -1;         // first sampled frame of the confirmed absent run
	int absent_run = 0;
	for (int f = base_frame - stride; f >= lo_bound; f -= stride) {
		if (cancel && cancel())
			return out;
		double cov = frame_coverage(f);
		if (cov < 0.0)
			return out;
		progress_step();
		if (cov >= opts.threshold) {
			anchor_back = f;
			absent_run = 0;
			continue;
		}
		if (++absent_run >= confirm) {
			absent_back = f;
			break;
		}
	}

	if (absent_back < 0) {
		if (anchor_back != base_frame) {
			// Every sample down to the bound was present: the run reaches (or
			// extends past) the bound. Probe the bound itself so a run that
			// actually ends within one stride of it is not reported as
			// reaching the bound.
			double cov_bound = frame_coverage(lo_bound);
			if (cov_bound < 0.0)
				return out;
			progress_step();
			if (cov_bound < opts.threshold)
				absent_back = lo_bound;
			else
				anchor_back = lo_bound;
		}
		else {
			// The first stride step lands outside the search bound, so the run
			// on this side is shorter than one stride. Scan frame by frame.
			for (int f = base_frame - 1; f >= lo_bound; --f) {
				if (cancel && cancel())
					return out;
				double cov = frame_coverage(f);
				if (cov < 0.0)
					return out;
				progress_step();
				if (cov >= opts.threshold) {
					anchor_back = f;
					absent_run = 0;
					continue;
				}
				if (++absent_run >= confirm) {
					absent_back = f;
					break;
				}
			}
		}
	}

	// ---- Forward side ------------------------------------------------------
	int anchor_forward = base_frame;
	int absent_forward = -1;
	absent_run = 0;
	for (int f = base_frame + stride; f <= hi_bound; f += stride) {
		if (cancel && cancel())
			return out;
		double cov = frame_coverage(f);
		if (cov < 0.0)
			return out;
		progress_step();
		if (cov >= opts.threshold) {
			anchor_forward = f;
			absent_run = 0;
			continue;
		}
		if (++absent_run >= confirm) {
			absent_forward = f;
			break;
		}
	}

	if (absent_forward < 0) {
		if (anchor_forward != base_frame) {
			double cov_bound = frame_coverage(hi_bound);
			if (cov_bound < 0.0)
				return out;
			progress_step();
			if (cov_bound < opts.threshold)
				absent_forward = hi_bound;
			else
				anchor_forward = hi_bound;
		}
		else {
			for (int f = base_frame + 1; f <= hi_bound; ++f) {
				if (cancel && cancel())
					return out;
				double cov = frame_coverage(f);
				if (cov < 0.0)
					return out;
				progress_step();
				if (cov >= opts.threshold) {
					anchor_forward = f;
					absent_run = 0;
					continue;
				}
				if (++absent_run >= confirm) {
					absent_forward = f;
					break;
				}
			}
		}
	}

	// ---- Exact boundary refinement -----------------------------------------
	// Walk frame by frame from the last confirmed present sample into the
	// absent run, tolerating dips shorter than `confirm` frames inside the run.
	int start = anchor_back;
	if (absent_back >= 0) {
		int dip_run = 0;
		int floor = std::max(lo_bound, absent_back - stride);
		for (int f = anchor_back - 1; f >= floor; --f) {
			if (cancel && cancel())
				return out;
			double cov = frame_coverage(f);
			if (cov < 0.0)
				return out;
			progress_step();
			if (cov >= opts.threshold) {
				start = f;
				dip_run = 0;
			}
			else if (++dip_run >= confirm) {
				break;
			}
		}
	}

	int end = anchor_forward;
	if (absent_forward >= 0) {
		int dip_run = 0;
		int ceil = std::min(hi_bound, absent_forward + stride);
		for (int f = anchor_forward + 1; f <= ceil; ++f) {
			if (cancel && cancel())
				return out;
			double cov = frame_coverage(f);
			if (cov < 0.0)
				return out;
			progress_step();
			if (cov >= opts.threshold) {
				end = f;
				dip_run = 0;
			}
			else if (++dip_run >= confirm) {
				break;
			}
		}
	}

	if (start < lo_bound || end > hi_bound || start > end || base_frame < start || base_frame > end)
		return out;

	int duration = end - start + 1;
	if (opts.min_duration_frames > 0 && duration < opts.min_duration_frames)
		return out;

	out.ok = true;
	out.start_frame = start;
	out.end_frame = end;
	out.frames.reserve(coverage_cache.size());
	out.diffs.reserve(coverage_cache.size());
	for (auto const& entry : coverage_cache) {
		out.frames.push_back(entry.first);
		out.diffs.push_back(entry.second);
	}
	return out;
}

} // namespace hardsub
