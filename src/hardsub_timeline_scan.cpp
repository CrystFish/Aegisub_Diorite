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

#include "hardsub_timeline_scan.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <utility>

namespace hardsub {

namespace {

int Clamp(int v, int lo, int hi) {
	return std::max(lo, std::min(hi, v));
}

unsigned char Luma(unsigned char r, unsigned char g, unsigned char b) {
	return static_cast<unsigned char>((static_cast<int>(r) + static_cast<int>(g)
	                                  + static_cast<int>(b)) / 3);
}

RegionImage CropRegionImage(RegionImage const& src, TimelineRect const& rect) {
	RegionImage out;
	if (!src.Valid() || !rect.Valid())
		return out;
	int x0 = Clamp(rect.x, 0, src.width - 1);
	int y0 = Clamp(rect.y, 0, src.height - 1);
	int x1 = Clamp(rect.x + rect.w, x0, src.width);
	int y1 = Clamp(rect.y + rect.h, y0, src.height);
	if (x1 <= x0 || y1 <= y0)
		return out;
	out.width = x1 - x0;
	out.height = y1 - y0;
	out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);
	for (int row = 0; row < out.height; ++row) {
		std::memcpy(out.rgb.data() + static_cast<size_t>(row) * out.width * 3,
		            src.rgb.data() + (static_cast<size_t>(y0 + row) * src.width + x0) * 3,
		            static_cast<size_t>(out.width) * 3);
	}
	return out;
}

// Downsample luminance into a gw x gh grid of cell-average bytes.
std::vector<unsigned char> DownsampleLuma(RegionImage const& im, int gw, int gh) {
	std::vector<unsigned char> grid(
		static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
	if (!im.Valid() || gw <= 0 || gh <= 0)
		return grid;
	for (int cy = 0; cy < gh; ++cy) {
		int ys = cy * im.height / gh;
		int ye = std::min(im.height, (cy + 1) * im.height / gh);
		for (int cx = 0; cx < gw; ++cx) {
			int xs = cx * im.width / gw;
			int xe = std::min(im.width, (cx + 1) * im.width / gw);
			double sum = 0.0;
			int count = 0;
			for (int y = ys; y < ye; ++y) {
				const unsigned char* row = im.rgb.data() + static_cast<size_t>(y) * im.width * 3;
				for (int x = xs; x < xe; ++x) {
					const unsigned char* p = row + static_cast<size_t>(x) * 3;
					sum += Luma(p[0], p[1], p[2]);
					++count;
				}
			}
			if (count > 0)
				grid[static_cast<size_t>(cy) * gw + cx] =
					static_cast<unsigned char>(sum / count);
		}
	}
	return grid;
}

constexpr int kSignatureGridW = 16;
constexpr int kSignatureGridH = 8;

} // namespace

TimelineRect LineBoundingRect(ocr::OCRLine const& line) {
	TimelineRect out;
	if (line.box.empty())
		return out;
	int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
	bool first = true;
	for (auto const& pt : line.box) {
		if (first) {
			min_x = max_x = pt.first;
			min_y = max_y = pt.second;
			first = false;
			continue;
		}
		min_x = std::min(min_x, pt.first);
		max_x = std::max(max_x, pt.first);
		min_y = std::min(min_y, pt.second);
		max_y = std::max(max_y, pt.second);
	}
	out.x = min_x;
	out.y = min_y;
	out.w = max_x - min_x;
	out.h = max_y - min_y;
	return out;
}

std::vector<std::pair<std::vector<ocr::OCRLine>, TimelineRect>>
ClusterRows(std::vector<ocr::OCRLine> const& lines, int region_h) {
	using Cluster = std::pair<std::vector<ocr::OCRLine>, TimelineRect>;
	std::vector<Cluster> out;
	if (lines.empty())
		return out;

	// Sort top-to-bottom, left-to-right.
	std::vector<ocr::OCRLine> sorted = lines;
	std::stable_sort(sorted.begin(), sorted.end(), [](ocr::OCRLine const& a, ocr::OCRLine const& b) {
		auto ra = LineBoundingRect(a);
		auto rb = LineBoundingRect(b);
		if (ra.y != rb.y)
			return ra.y < rb.y;
		return ra.x < rb.x;
	});

	for (auto const& line : sorted) {
		TimelineRect rect = LineBoundingRect(line);
		if (!rect.Valid())
			continue;

		// Try to attach to an existing cluster whose vertical box substantially
		// overlaps: fragments of the same visual line. Distinct rows in the
		// region are assumed not to overlap, so a >50% overlap is a merge.
		bool merged = false;
		for (auto& cluster : out) {
			TimelineRect const& cr = cluster.second;
			int overlap = std::max(0, std::min(rect.y + rect.h, cr.y + cr.h) - std::max(rect.y, cr.y));
			int min_h = std::min(rect.h, cr.h);
			if (min_h > 0 && overlap * 2 >= min_h) {
				cluster.first.push_back(line);
				cluster.second = TimelineRect{
					std::min(rect.x, cr.x),
					std::min(rect.y, cr.y),
					std::max(rect.x + rect.w, cr.x + cr.w) - std::min(rect.x, cr.x),
					std::max(rect.y + rect.h, cr.y + cr.h) - std::min(rect.y, cr.y)};
				merged = true;
				break;
			}
		}
		if (!merged)
			out.emplace_back(std::vector<ocr::OCRLine>{line}, rect);
	}

	// Stable output order: top-to-bottom.
	std::stable_sort(out.begin(), out.end(), [](Cluster const& a, Cluster const& b) {
		return a.second.y < b.second.y;
	});
	(void)region_h;
	return out;
}

unsigned long long RegionSignature(RegionImage const& img) {
	auto grid = DownsampleLuma(img, 8, 8);
	// FNV-1a 64-bit hash of the 8x8 luminance grid.
	unsigned long long h = 1469598103934665603ULL;
	for (unsigned char b : grid) {
		h ^= b;
		h *= 1099511628211ULL;
	}
	return h;
}

std::vector<unsigned char> RowSignature(RegionImage const& img) {
	// Per-cell fraction of text-like (luminance-extreme) pixels. The background
	// pixels sit near the row's median luminance, so a moving background moves
	// the median but not the relative deviation profile of the text strokes.
	std::vector<unsigned char> grid(static_cast<size_t>(kSignatureGridW) * kSignatureGridH, 0);
	if (!img.Valid())
		return grid;

	std::vector<int> lum;
	lum.reserve(img.rgb.size() / 3);
	for (size_t i = 0; i < img.rgb.size(); i += 3)
		lum.push_back(Luma(img.rgb[i], img.rgb[i + 1], img.rgb[i + 2]));
	if (lum.empty())
		return grid;

	std::vector<int> sorted = lum;
	std::sort(sorted.begin(), sorted.end());
	int median = sorted[sorted.size() / 2];
	constexpr int kDeviation = 40;

	for (int cy = 0; cy < kSignatureGridH; ++cy) {
		int ys = cy * img.height / kSignatureGridH;
		int ye = std::min(img.height, (cy + 1) * img.height / kSignatureGridH);
		for (int cx = 0; cx < kSignatureGridW; ++cx) {
			int xs = cx * img.width / kSignatureGridW;
			int xe = std::min(img.width, (cx + 1) * img.width / kSignatureGridW);
			int total = 0, dev = 0;
			for (int y = ys; y < ye; ++y) {
				const unsigned char* row = img.rgb.data() + static_cast<size_t>(y) * img.width * 3;
				for (int x = xs; x < xe; ++x) {
					const unsigned char* p = row + static_cast<size_t>(x) * 3;
					int l = Luma(p[0], p[1], p[2]);
					++total;
					if (std::abs(l - median) >= kDeviation)
						++dev;
				}
			}
			if (total > 0)
				grid[static_cast<size_t>(cy) * kSignatureGridW + cx] =
					static_cast<unsigned char>(dev * 255 / total);
		}
	}
	return grid;
}

double SignatureDistance(std::vector<unsigned char> const& a,
                         std::vector<unsigned char> const& b) {
	if (a.empty() || a.size() != b.size())
		return 1.0;
	unsigned long long sum = 0;
	for (size_t i = 0; i < a.size(); ++i)
		sum += static_cast<unsigned long long>(std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
	return double(sum) / (a.size() * 255.0);
}

bool RowPassesCenterFilter(TimelineRect const& rect, int region_w, double center_band) {
	if (center_band >= 100.0 || region_w <= 0)
		return true;
	double half_band = center_band / 200.0 * region_w;
	return std::abs(static_cast<double>(rect.CenterX()) - region_w / 2.0) <= half_band;
}

namespace {

struct RowTrack {
	int band = 0;
	std::vector<unsigned char> signature;
	int first_seen = -1;
	int last_seen = -1;
	int seen_count = 0;
	int misses = 0;
	bool open = true;
	bool center_ok = true;
	TimelineRect region_rect;
};

} // namespace

TimelineScanResult ScanVideoTimeline(TimelineScanOptions const& opts,
                                     FrameLoader const& loader,
                                     RegionDetector const& detect,
                                     RegionRecognizer const& recognize,
                                     CancelFn const& cancel,
                                     ProgressFn const& progress,
                                     RowCallback const& row_callback) {
	TimelineScanResult out;
	if (!loader) {
		out.error = "No frame loader available.";
		return out;
	}
	if (opts.frame_count <= 0 || opts.region_w <= 0 || opts.region_h <= 0) {
		out.error = "Invalid scan options.";
		return out;
	}

	const int start = Clamp(opts.frame_start, 0, opts.frame_count - 1);
	const int end = opts.frame_end < 0
		? opts.frame_count - 1
		: Clamp(opts.frame_end, start, opts.frame_count - 1);
	const int total = end - start + 1;
	const int detect_every = std::max(1, opts.detect_every);
	const int detect_every_max = std::max(detect_every, opts.detect_every_max);
	const int settle_confirm = std::max(1, opts.confirm_frames);
	const int band_size = std::max(8, opts.region_h / 6);
	constexpr double kChangeHigh = 0.25;
	constexpr double kChangeLow = 0.08;
	constexpr double kSignatureMatch = 0.25;

	std::vector<RowTrack> tracks;
	RegionImage prev;
	bool have_prev = false;
	int stable_run = 0;
	bool settled = false;
	unsigned long long last_hash = 0;
	int last_ocr = -(detect_every_max * 3);
	bool has_ocr = false;

	int done = 0;
	for (int f = start; f <= end; ++f) {
		if (cancel && cancel()) {
			out.cancelled = true;
			break;
		}
		++done;
		if (progress)
			progress(done, total);

		auto frame = loader(f);
		if (!frame)
			continue;
		auto reg = CropRegion(*frame, opts.region_x, opts.region_y, opts.region_w, opts.region_h);
		if (!reg.Valid())
			continue;

		if (have_prev) {
			double change = RegionChangedRatio(prev, reg);
			if (change > kChangeHigh) {
				stable_run = 0;
				settled = false;
			}
			else if (change < kChangeLow) {
				++stable_run;
				if (stable_run >= settle_confirm)
					settled = true;
			}
			else {
				stable_run = 0;
				settled = false;
			}
		}
		else {
			settled = false;
		}

		unsigned long long h = RegionSignature(reg);
		bool content_changed = !has_ocr || h != last_hash;
		int since = f - last_ocr;
		bool do_ocr = !has_ocr
			|| (content_changed && settled && since >= detect_every)
			|| (content_changed && since >= detect_every_max);
		prev = std::move(reg); // reg is no longer needed unless OCR runs
		// Keep have_prev true for every subsequent valid frame so the settle
		// detector can compare the previous frame with the current one. Without
		// this, settled is always false and OCR only ever fires on the
		// detect_every_max hard cap, sampling each subtitle once and missing
		// any whose single sample lands during a fade-in or motion.
		have_prev = true;

		if (!do_ocr)
			continue;

		++out.ocr_samples;
		ocr::OCRResult det;
		try {
			det = detect(prev);
		}
		catch (...) {
			det = ocr::OCRResult();
		}
		std::vector<ocr::OCRLine> lines = det.ok ? det.lines : std::vector<ocr::OCRLine>{};
		auto clusters = ClusterRows(lines, opts.region_h);

		for (auto const& c : clusters) {
			TimelineRect rect = c.second;
			if (!rect.Valid())
				continue;
			bool center_ok = RowPassesCenterFilter(rect, opts.region_w, opts.center_band);
			if (opts.center_only && !center_ok)
				continue;
			auto crop = CropRegionImage(prev, rect);
			if (!crop.Valid())
				continue;
			auto sig = RowSignature(crop);
			int band = rect.CenterY() / band_size;

			int best = -1;
			double best_d = 1.0;
			for (size_t i = 0; i < tracks.size(); ++i) {
				if (tracks[i].open && std::abs(tracks[i].band - band) <= 1) {
					double d = SignatureDistance(tracks[i].signature, sig);
					if (d < best_d) {
						best_d = d;
						best = static_cast<int>(i);
					}
				}
			}
			if (best >= 0 && best_d <= kSignatureMatch) {
				RowTrack& tr = tracks[best];
				tr.last_seen = f;
				tr.seen_count++;
				tr.misses = 0;
				tr.region_rect = rect;
				tr.center_ok = center_ok;
			}
			else {
				tracks.push_back(RowTrack{band, sig, f, f, 1, 0, true, center_ok, rect});
			}
		}

		// Close tracks that were not seen at this sample (their last_seen is not
		// the current frame) for a real gap, not just one miss.
		for (size_t i = 0; i < tracks.size(); ++i) {
			if (tracks[i].open && tracks[i].last_seen != f) {
				int gap = f - tracks[i].last_seen;
				tracks[i].misses++;
				if (gap >= std::max(detect_every * 2, 1))
					tracks[i].open = false;
			}
		}

		last_hash = h;
		last_ocr = f;
		has_ocr = true;
	}
	if (out.cancelled)
		return out;

	for (auto& tr : tracks)
		if (tr.open)
			tr.open = false;

	ScanOptions scan;
	scan.threshold = opts.threshold;
	scan.threshold_exit = opts.threshold_exit;
	scan.confirm_frames = opts.confirm_frames;
	scan.min_duration_frames = opts.min_duration_frames;

	for (auto const& tr : tracks) {
		if (tr.last_seen < tr.first_seen)
			continue;
		int rep = tr.first_seen + (tr.last_seen - tr.first_seen) / 2;

		auto frame = loader(rep);
		if (!frame)
			continue;
		auto region = CropRegion(*frame, opts.region_x, opts.region_y,
		                         opts.region_w, opts.region_h);
		if (!region.Valid())
			continue;

		// The track's stored box was captured at some earlier sample and can
		// drift a few pixels from where the subtitle sits at the representative
		// frame. Re-detect on the representative frame and pick the row nearest
		// the stored position, so the template and recognition crop both align
		// with the text at this exact frame. Falling back to the stored box
		// keeps a row usable when detection is momentarily empty.
		TimelineRect box = tr.region_rect;
		try {
			ocr::OCRResult det = detect(region);
			if (det.ok && !det.lines.empty()) {
				auto clusters = ClusterRows(det.lines, opts.region_h);
				TimelineRect best;
				double best_d = 1e18;
				for (auto const& c : clusters) {
					TimelineRect cr = c.second;
					if (!cr.Valid())
						continue;
					double d = std::abs(static_cast<double>(
						cr.CenterX() - tr.region_rect.CenterX()))
						+ std::abs(static_cast<double>(
						cr.CenterY() - tr.region_rect.CenterY()));
					if (d < best_d) {
						best_d = d;
						best = cr;
					}
				}
				if (best.Valid())
					box = best;
			}
		}
		catch (...) {
			box = tr.region_rect;
		}

		// Pad the box a little so the glyphs never touch the crop edge (which
		// would make the recognizer hallucinate) and so the boundary template
		// gets a bit of text + stable background context.
		int px = std::max(8, box.w / 10);
		int py = std::max(8, box.h / 3);
		TimelineRect row_box;
		row_box.x = std::max(0, box.x - px);
		row_box.y = std::max(0, box.y - py);
		row_box.w = box.w + 2 * px;
		row_box.h = box.h + 2 * py;

		TimelineRect rv;
		rv.x = opts.region_x + row_box.x;
		rv.y = opts.region_y + row_box.y;
		rv.w = row_box.w;
		rv.h = row_box.h;

		auto tpl = CropRegion(*frame, rv.x, rv.y, rv.w, rv.h);
		if (!tpl.Valid())
			continue;

		// Search the exact run without an artificial cap: the template
		// coverage signal only matches this row's own text, so a generous
		// search cannot jump to an unrelated subtitle, and it recovers the
		// true start even when OCR discovered the row late.
		scan.max_frames = 0;
		auto res = ScanBoundaries(tpl, rv.x, rv.y, rep, opts.frame_count, scan,
		                          loader, cancel, ProgressFn(), std::vector<int>{});
		if (!res.ok)
			continue;

		TimelineRowEvent ev;
		ev.start_frame = res.start_frame;
		ev.end_frame = res.end_frame;
		ev.rep_frame = rep;
		ev.center_ok = tr.center_ok;
		ev.row_video = rv;
		ev.row_region = row_box;

		ocr::OCRResult rec;
		try {
			rec = recognize(tpl);
		}
		catch (...) {
			rec = ocr::OCRResult();
		}
		if (rec.ok) {
			// A row is only useful once we can actually read text. PaddleOCR
			// routinely emits a second, low-confidence "line" for a strip of
			// background below the subtitle, and emits garbage on scene texture
			// that merely looks text-like. Keep only confident, non-empty lines
			// so real text survives while noise is dropped.
			std::vector<ocr::OCRLine> kept;
			for (auto const& line : rec.lines) {
				if (!line.text.empty() && line.confidence >= 0.6) {
					// PaddleOCR can return the same line twice (two overlapping
					// det boxes); keep the first occurrence only so the text is
					// not duplicated.
					bool dup = false;
					for (auto const& k : kept)
						if (k.text == line.text) {
							dup = true;
							break;
						}
					if (!dup)
						kept.push_back(line);
				}
			}
			if (!kept.empty()) {
				std::string text;
				double conf = 0.0;
				for (size_t k = 0; k < kept.size(); ++k) {
					if (k) text += "\n";
					text += kept[k].text;
					conf += kept[k].confidence;
				}
				ev.text = text;
				if (opts.replace_newlines)
					for (auto& ch : ev.text)
						if (ch == '\n') ch = ' ';
				ev.confidence = conf / kept.size();
				ev.ocr_verified = true;
			}
		}

		// Drop rows with no readable text: these are the false positives on a
		// dynamic scene or empty background that OCR "detected" but could not
		// read, and they only add blank/garbage entries to the result.
		if (ev.text.empty())
			continue;

		if (row_callback)
			row_callback(ev);
		out.events.push_back(ev);
	}

	// Order by start frame (and vertical position for stable output), then merge
	// rows that share exactly the same start/end into a single multi-line event.
	std::stable_sort(out.events.begin(), out.events.end(),
		[](TimelineRowEvent const& a, TimelineRowEvent const& b) {
			if (a.start_frame != b.start_frame)
				return a.start_frame < b.start_frame;
			if (a.end_frame != b.end_frame)
				return a.end_frame < b.end_frame;
			return a.row_region.y < b.row_region.y;
		});

	if (opts.merge_same_interval) {
		std::vector<TimelineRowEvent> merged;
		for (size_t i = 0; i < out.events.size();) {
			size_t j = i;
			while (j < out.events.size()
			       && out.events[j].start_frame == out.events[i].start_frame
			       && out.events[j].end_frame == out.events[i].end_frame)
				++j;
			if (j - i == 1) {
				merged.push_back(out.events[i]);
			}
			else {
				TimelineRowEvent e = out.events[i];
				std::string text = out.events[i].text;
				std::string last_added = text;
				double conf = 0.0;
				int verified = 0;
				for (size_t k = i; k < j; ++k) {
					if (k != i && !out.events[k].text.empty()
					    && out.events[k].text != last_added) {
						if (!text.empty())
							text += "\n";
						text += out.events[k].text;
						last_added = out.events[k].text;
					}
					conf += out.events[k].confidence;
					if (out.events[k].ocr_verified)
						++verified;
					e.row_region.x = std::min(e.row_region.x, out.events[k].row_region.x);
					e.row_region.y = std::min(e.row_region.y, out.events[k].row_region.y);
					e.row_region.w = std::max(
						e.row_region.x + e.row_region.w,
						out.events[k].row_region.x + out.events[k].row_region.w) - e.row_region.x;
					e.row_region.h = std::max(
						e.row_region.y + e.row_region.h,
						out.events[k].row_region.y + out.events[k].row_region.h) - e.row_region.y;
				}
				e.text = text;
				if (opts.replace_newlines)
					for (auto& ch : e.text)
						if (ch == '\n') ch = ' ';
				e.confidence = conf / (j - i);
				e.ocr_verified = verified == static_cast<int>(j - i);
				merged.push_back(std::move(e));
			}
			i = j;
		}
		out.events = std::move(merged);
	}

	out.ok = true;
	out.frames_evaluated = done;
	return out;
}

} // namespace hardsub
