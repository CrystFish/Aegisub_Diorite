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
	/// to count as showing the subtitle (0..1). This is the "enter" threshold:
	/// coverage at or above it flips the state machine to present.
	double threshold = 0.40;
	/// Hysteresis "exit" threshold (0..threshold). Coverage below this flips
	/// the state machine to absent; between `threshold_exit` and `threshold`
	/// the state is held, which keeps a slowly fading subtitle from flickering.
	/// 0 means exit = threshold * 0.75.
	double threshold_exit = 0.30;
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
/// content matches the template.
///
/// Random frame access is far more expensive than sequential decoding on
/// modern codecs (a 4K/AV1 seek decodes a whole GOP, ~1 s, while a sequential
/// frame costs a few ms), so the scan avoids arbitrary-frame probes entirely:
/// it starts one contiguous ascending pass at a keyframe before the run
/// (extending the start backward through keyframes only while the coverage
/// there still looks like the subtitle), then decodes forward until the run
/// has clearly ended after the base frame. Both boundaries are read off the
/// resulting dense presence series, which is exact and also splits the run at
/// interior gaps. `keyframes` should be the provider's keyframe list so the
/// pass start can be aligned to cheap seeks; when empty a fixed pre-roll is
/// used. ScanOptions::max_frames optionally caps how far it may search. The
/// template region origin is given by (tpl_x, tpl_y); its size comes from tpl.
ScanResult ScanBoundaries(RegionImage const& tpl, int tpl_x, int tpl_y,
                          int base_frame, int frame_count,
                          ScanOptions const& opts,
                          FrameLoader const& loader,
                          CancelFn const& cancel = CancelFn(),
                          ProgressFn const& progress = ProgressFn(),
                          std::vector<int> const& keyframes = {});

// ---- Pure helpers shared by the scan and the dialog's OCR confirmation -----
// These operate on dense, ascending per-frame evidence so they can be unit
// tested without a GUI or a decoder.

/// Per-frame presence after hysteresis. When `ocr_box` is non-empty it must
/// have the same size as `coverage`, and a frame is only present when the OCR
/// evidence agrees (a missing box vetoes the frame even when the coverage is
/// high). Frames with coverage below zero are treated as unevaluated and keep
/// the previous state.
std::vector<bool> ComputePresence(std::vector<double> const& coverage,
                                  std::vector<bool> const& ocr_box,
                                  double threshold, double threshold_exit);

/// Fill interior absent runs shorter than `confirm_frames` (0 disables the
/// filter). Leading and trailing absent runs are preserved, so the run does
/// not extend past the evaluated window.
std::vector<bool> FillShortDips(std::vector<bool> const& present, int confirm_frames);

/// Index of the first frame of the contiguous present run containing `anchor`.
/// Returns `anchor` when the anchor frame is not present.
int RunStart(std::vector<bool> const& present, int anchor);
/// Index of the last frame of the contiguous present run containing `anchor`.
/// Returns `anchor` when the anchor frame is not present.
int RunEnd(std::vector<bool> const& present, int anchor);

/// Snap a boundary within a window of ascending evidence: `find_earliest`
/// picks the first frame whose pixel coverage is at least `threshold_exit` and
/// whose OCR evidence shows a box; otherwise the last such frame. Returns
/// `fallback` when no frame qualifies. Used to move a pixel boundary by at
/// most the evaluated window, and only toward frames both signals agree on.
int SnapBoundaryFromEvidence(std::vector<double> const& coverage,
                             std::vector<bool> const& ocr_box,
                             double threshold_exit, bool find_earliest, int fallback);

/// True when the pixel evidence around a boundary is ambiguous (frames just
/// outside the run still have coverage at or above `threshold_exit`, e.g. a
/// fade or partial occlusion), meaning OCR confirmation is worth running.
/// Also returns true when the evidence series is not dense enough to tell, so
/// callers never skip OCR on insufficient data.
bool BoundaryAmbiguous(ScanResult const& result, double threshold_exit, bool start);

} // namespace hardsub
