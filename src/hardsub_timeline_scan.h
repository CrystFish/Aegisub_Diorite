// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include "hardsub_scan.h"
#include "ocr/ocr_result.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hardsub {

/// Options for scanning an entire video for hard subtitles inside a configurable
/// region. The true (frame-exact) start/end of each detected row is recovered by
/// the same template-coverage scan used by the single-subtitle dialog; OCR is
/// only used here to discover rows and to read their text.
struct TimelineScanOptions {
	/// Region to scan, in video pixel coordinates.
	int region_x = 0;
	int region_y = 0;
	int region_w = 0;
	int region_h = 0;

	int frame_count = 0;
	int frame_start = 0;
	/// Inclusive; a negative value means the last frame.
	int frame_end = -1;

	/// Only keep rows whose horizontal center is inside the middle `center_band`
	/// percent of the region. 100 disables the filter.
	bool center_only = true;
	double center_band = 10.0;

	/// Drops detected runs shorter than this many frames.
	int min_duration_frames = 12;
	/// Debounce frames used both for OCR settle detection and the boundary scan.
	int confirm_frames = 2;

	/// Minimum number of frames between two OCR detection samples while the
	/// region is stable. Lower values are more responsive but slower.
	int detect_every = 16;
	/// Hard cap on frames between two OCR samples even while the region is
	/// never stable (e.g. continuous camera motion).
	int detect_every_max = 24;

	/// When two or more rows share exactly the same start/end frame, merge them
	/// into a single event joined with a line break; otherwise keep them separate.
	bool merge_same_interval = true;

	/// Replace the in-text line breaks ("\N") with a single space when building
	/// the final event text. True collapses multi-line OCR rows into one line.
	bool replace_newlines = true;

	/// Template-coverage thresholds forwarded to ScanBoundaries.
	double threshold = 0.40;
	double threshold_exit = 0.20;
};

/// A bounding rectangle in region-relative pixel coordinates.
struct TimelineRect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;

	bool Valid() const { return w > 0 && h > 0; }
	int CenterX() const { return x + w / 2; }
	int CenterY() const { return y + h / 2; }
};

/// One detected hard-subtitle row (may represent one visual line, or a merged
/// cluster of several simultaneously visible lines).
struct TimelineRowEvent {
	int start_frame = -1;
	int end_frame = -1;
	/// Frame used as the template/recognition anchor; lies inside [start,end].
	int rep_frame = -1;
	/// A double-checked full-region (not per-row) OCR pass ran for this row.
	bool ocr_verified = false;
	/// Text recognized for the row (possibly with embedded newlines if merged).
	std::string text;
	/// Mean confidence of the recognized lines (0..1).
	double confidence = 0.0;
	/// Row rectangle in video pixel coordinates.
	TimelineRect row_video;
	/// Row rectangle in region-relative pixel coordinates (as returned by OCR).
	TimelineRect row_region;
	/// True when this row's horizontal center passed the center filter.
	bool center_ok = false;
};

struct TimelineScanResult {
	bool ok = false;
	bool cancelled = false;
	std::string error;
	std::vector<TimelineRowEvent> events;
	int frames_evaluated = 0;
	int ocr_samples = 0;
};

/// Loads one frame by number; returning nullptr aborts the scan.
/// Runs text detection over a region image and returns recognised boxes (in the
/// image's own pixel coordinates; text is optional).
using RegionDetector = std::function<ocr::OCRResult(RegionImage const&)>;
/// Runs text recognition over a region image and returns the recognised text.
using RegionRecognizer = std::function<ocr::OCRResult(RegionImage const&)>;
/// Invoked once for every fully recognized row as soon as its text and timing
/// are resolved, so the UI can show it while the rest of the video is scanned.
using RowCallback = std::function<void(TimelineRowEvent const&)>;

/// Scan every frame in [frame_start, frame_end] of the given region, discover the
/// hard-subtitle rows with sparse OCR, then recover each row's exact start/end
/// with the template-coverage scan. Returns the discovered events in time order.
TimelineScanResult ScanVideoTimeline(TimelineScanOptions const& opts,
                                     FrameLoader const& loader,
                                     RegionDetector const& detect,
                                     RegionRecognizer const& recognize,
                                     CancelFn const& cancel = CancelFn(),
                                     ProgressFn const& progress = ProgressFn(),
                                     RowCallback const& row_callback = RowCallback());

// ---- Pure helpers (unit-testable without a decoder or OCR runtime) -----------

/// Convert an OCR line's polygon box into a bounding rectangle.
TimelineRect LineBoundingRect(ocr::OCRLine const& line);

/// Group OCR lines into visual rows. Lines whose vertical boxes substantially
/// overlap are treated as fragments of the same row (the scan region is assumed
/// to contain no overlapping subtitles). Rows are returned top-to-bottom.
std::vector<std::pair<std::vector<ocr::OCRLine>, TimelineRect>>
ClusterRows(std::vector<ocr::OCRLine> const& lines, int region_h);

/// Coarse luminance signature of a region image (16x8 downsampled luminance).
/// Used to decide whether a region has "settled" its content between OCR runs.
unsigned long long RegionSignature(RegionImage const& img);

/// Per-row signature from very small text-like (luminance-deviation) pixels.
std::vector<unsigned char> RowSignature(RegionImage const& img);

/// Normalised (0..1) distance between two row signatures; smaller means more alike.
double SignatureDistance(std::vector<unsigned char> const& a,
                         std::vector<unsigned char> const& b);

/// True when the horizontal centre of `rect` (a region-relative box) lies inside
/// the middle `center_band` percent of a region of width `region_w`.
bool RowPassesCenterFilter(TimelineRect const& rect, int region_w, double center_band);

} // namespace hardsub
