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

#include "video_frame.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hardsub {

/// A small RGB image buffer (no wxWidgets dependency so the scan logic can be
/// unit tested without a GUI).
struct RegionImage {
	int width = 0;
	int height = 0;
	/// width * height * 3 bytes in R, G, B order
	std::vector<unsigned char> rgb;

	bool Valid() const {
		return width > 0 && height > 0 && rgb.size() == size_t(width) * height * 3;
	}
};

/// Copy the given rectangle of a video frame into an RGB RegionImage.
/// The rectangle is clamped to the frame bounds. Returns an invalid RegionImage
/// when the clamped rectangle is empty or the frame is malformed.
RegionImage CropRegion(VideoFrame const& frame, int x, int y, int width, int height);

/// Mean absolute difference per channel (0..255) between two equally sized
/// RGB images. Returns 255 for mismatched sizes.
double MeanAbsDiff(RegionImage const& a, RegionImage const& b);

/// Fraction (0..1) of pixels whose maximum channel difference exceeds
/// pixel_threshold. This is more robust than the mean difference for detecting
/// whether a specific subtitle is present, because it does not depend on how
/// much of the region the subtitle ink covers.
double RegionChangedRatio(RegionImage const& a, RegionImage const& b, int pixel_threshold = 12);

/// Pixel indices (into tpl.rgb, pixel index) of the template pixels that look
/// like subtitle ink: their luminance deviates from the region's dominant
/// background luminance by at least `deviation`. Restricting the presence test
/// to these pixels makes it immune to background motion: the subtitle pixels
/// are the same burned-in colors in every frame they are visible, while the
/// background around them may change freely.
std::vector<int> TemplateTextPixels(RegionImage const& tpl, int deviation = 60);

/// Fraction (0..1) of the template text pixels (see TemplateTextPixels) whose
/// maximum channel difference from `frame` does not exceed `match_threshold`.
/// Returns 0 when the images have different sizes or text_pixels is empty.
double TemplateTextCoverage(RegionImage const& tpl, RegionImage const& frame,
                            std::vector<int> const& text_pixels,
                            int match_threshold = 32);

struct ScanOptions {
	/// Optional maximum number of frames to search in each direction from the
	/// base frame; 0 (or negative) means search all the way to the video
	/// edges. The scan normally finds the real boundaries on its own by
	/// walking outward from the base frame, so this is only a safety limit.
	int max_frames = 0;
	/// Fraction of the template's text pixels that must still match for a frame
	/// to count as showing the subtitle (0..1).
	double threshold = 0.40;
	/// Frame step of the outward walk. 1 examines every frame, so any gap at
	/// least `confirm_frames` wide splits the run and the boundaries are exact.
	/// Larger values skip frames between samples: the scan is faster but gaps
	/// narrower than the step can be missed. The boundary is always refined
	/// frame by frame around the transition.
	int stride = 32;
	/// Debounce: present/absent runs shorter than this many frames are removed
	/// (0 disables the filter)
	int confirm_frames = 2;
	/// If the detected run is shorter than this, the scan fails
	int min_duration_frames = 0;
};

struct ScanResult {
	bool ok = false;
	int start_frame = -1;
	int end_frame = -1;
	int base_frame = -1;
	/// Human-readable reason when ok is false and the failure is diagnosable.
	std::string error;
	/// Frame numbers evaluated, for diagnostics
	std::vector<int> frames;
	/// Text coverage for each evaluated frame, aligned with frames
	std::vector<double> diffs;
};

/// Loads a single video frame; returning nullptr aborts the scan
using FrameLoader = std::function<std::shared_ptr<VideoFrame>(int)>;
/// Returning true aborts the scan
using CancelFn = std::function<bool()>;
/// Called with (frames_done, total_frames)
using ProgressFn = std::function<void(int, int)>;

/// Find the contiguous run of frames around base_frame in which the region
/// content matches the template. Starting from base_frame, the scan walks
/// outward at ScanOptions::stride until the run clearly ends, then refines the
/// two boundaries frame by frame; ScanOptions::max_frames optionally caps how
/// far it may search. The template region origin is given by (tpl_x, tpl_y);
/// its size comes from tpl.
ScanResult ScanBoundaries(RegionImage const& tpl, int tpl_x, int tpl_y,
                          int base_frame, int frame_count,
                          ScanOptions const& opts,
                          FrameLoader const& loader,
                          CancelFn const& cancel = CancelFn(),
                          ProgressFn const& progress = ProgressFn());

} // namespace hardsub
