#include <main.h>

#include "hardsub_timeline_scan.h"

#include <algorithm>
#include <functional>
#include <memory>

namespace {

using namespace hardsub;

VideoFrame MakeFrame(int width, int height, unsigned char r, unsigned char g, unsigned char b) {
	VideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.pitch = width * 4;
	frame.flipped = false;
	frame.data.assign(frame.pitch * height, 0);
	for (size_t i = 0; i < frame.data.size(); i += 4) {
		frame.data[i] = b;
		frame.data[i + 1] = g;
		frame.data[i + 2] = r;
		frame.data[i + 3] = 255;
	}
	return frame;
}

RegionImage MakeBandRegion(int width, int height, unsigned char bg,
                           int band_y0, int band_y1, unsigned char band_lum) {
	auto frame = MakeFrame(width, height, bg, bg, bg);
	for (int y = band_y0; y < band_y1; ++y) {
		for (int x = 0; x < width; ++x) {
			size_t off = size_t(y) * frame.pitch + size_t(x) * 4;
			frame.data[off] = band_lum;
			frame.data[off + 1] = band_lum;
			frame.data[off + 2] = band_lum;
		}
	}
	return CropRegion(frame, 0, 0, width, height);
}

ocr::OCRLine MakeLine(std::string text, int x0, int y0, int x1, int y1) {
	ocr::OCRLine line;
	line.text = std::move(text);
	line.confidence = 0.9;
	line.box = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
	return line;
}

} // namespace

TEST(hardsub_timeline_scan, line_bounding_rect) {
	auto line = MakeLine("hi", 2, 3, 8, 10);
	auto rect = LineBoundingRect(line);
	EXPECT_EQ(rect.x, 2);
	EXPECT_EQ(rect.y, 3);
	EXPECT_EQ(rect.w, 6);
	EXPECT_EQ(rect.h, 7);
	EXPECT_TRUE(rect.Valid());
}

TEST(hardsub_timeline_scan, cluster_rows_separates_stacked_lines) {
	std::vector<ocr::OCRLine> lines = {
		MakeLine("top", 10, 0, 60, 12),
		MakeLine("bottom", 10, 30, 60, 44),
	};
	auto rows = ClusterRows(lines, 60);
	EXPECT_EQ(rows.size(), 2u);
	EXPECT_LT(rows[0].second.y, rows[1].second.y);
}

TEST(hardsub_timeline_scan, cluster_rows_merges_overlapping_fragments) {
	// Two boxes that overlap >50% in height are fragments of one visual row.
	std::vector<ocr::OCRLine> lines = {
		MakeLine("a", 0, 0, 40, 12),
		MakeLine("b", 30, 2, 70, 10),
	};
	auto rows = ClusterRows(lines, 60);
	EXPECT_EQ(rows.size(), 1u);
	EXPECT_GE(rows[0].second.w, 60);
}

TEST(hardsub_timeline_scan, row_center_filter) {
	// Region 100 wide, band 10 => center must be within 5 px of the middle.
	TimelineRect centered{40, 0, 20, 12};
	EXPECT_TRUE(RowPassesCenterFilter(centered, 100, 10));
	TimelineRect off{25, 0, 20, 12};            // center at 35, 15 px off middle
	EXPECT_FALSE(RowPassesCenterFilter(off, 100, 10));
	EXPECT_TRUE(RowPassesCenterFilter(off, 100, 100)); // 100 disables the filter
}

TEST(hardsub_timeline_scan, row_signature_matches_similar_content) {
	auto a = RowSignature(MakeBandRegion(40, 16, 64, 4, 10, 220));
	auto b = RowSignature(MakeBandRegion(40, 16, 70, 4, 10, 220)); // same band, slightly different bg
	EXPECT_LT(SignatureDistance(a, b), 0.2);
	auto c = RowSignature(MakeBandRegion(40, 16, 64, 10, 15, 220)); // band moved
	EXPECT_GT(SignatureDistance(a, c), 0.2);
}

TEST(hardsub_timeline_scan, scan_timeline_no_ocr_finds_nothing) {
	TimelineScanOptions opts;
	opts.region_x = 0;
	opts.region_y = 0;
	opts.region_w = 64;
	opts.region_h = 64;
	opts.frame_count = 30;
	opts.frame_start = 0;
	opts.frame_end = -1;
	opts.detect_every = 1;

	auto loader = FrameLoader([](int) -> std::shared_ptr<VideoFrame> {
		return std::make_shared<VideoFrame>(MakeFrame(64, 64, 120, 120, 120));
	});
	auto detect = RegionDetector([](RegionImage const&) -> ocr::OCRResult { return ocr::OCRResult(); });
	auto recognize = RegionRecognizer([](RegionImage const&) -> ocr::OCRResult { return ocr::OCRResult(); });

	auto result = ScanVideoTimeline(opts, loader, detect, recognize);
	ASSERT_TRUE(result.ok);
	EXPECT_TRUE(result.events.empty());
	EXPECT_GT(result.frames_evaluated, 0);
}

// A subtitle-like bright band appears on a static background for frames
// [40,60]. A "perfect" detector reports a box around the band exactly when it
// is present. This exercises the whole discovery + boundary-scan pipeline and
// pins down whether the scan logic recovers a known subtitle at all.
TEST(hardsub_timeline_scan, scan_timeline_finds_known_subtitle) {
	TimelineScanOptions opts;
	opts.region_x = 0;
	opts.region_y = 0;
	opts.region_w = 256;
	opts.region_h = 40;
	opts.frame_count = 100;
	opts.frame_start = 0;
	opts.frame_end = -1;
	opts.center_only = false;
	opts.center_band = 100;
	opts.detect_every = 4;
	opts.detect_every_max = 24;
	opts.confirm_frames = 2;
	opts.min_duration_frames = 2;

	// Dark background with a bright text band in rows [20,30), often a small
	// fraction of the region so it does not trip the "content changed" gate.
	auto loader = FrameLoader([opts](int f) -> std::shared_ptr<VideoFrame> {
		bool sub = f >= 40 && f <= 60;
		auto frame = MakeFrame(opts.region_w, opts.region_h, 35, 35, 35);
		if (sub) {
			// Stroke-dash "text": bright glyphs separated by dark gaps so the
			// detected box contains both text and background pixels (like a
			// real burned-in subtitle), which TemplateTextPixels needs.
			for (int y = 20; y < 30; ++y) {
				for (int x = 128; x < 192; ++x) {
					if ((x & 7) < 4) continue; // dark gap
					size_t off = size_t(y) * frame.pitch + size_t(x) * 4;
					frame.data[off] = 215;
					frame.data[off + 1] = 215;
					frame.data[off + 2] = 215;
				}
			}
		}
		return std::make_shared<VideoFrame>(std::move(frame));
	});

	auto detect = RegionDetector([](RegionImage const& r) -> ocr::OCRResult {
		ocr::OCRResult out;
		int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
		for (int y = 0; y < r.height; ++y) {
			for (int x = 0; x < r.width; ++x) {
				const unsigned char* p = r.rgb.data() + (size_t(y) * r.width + x) * 3;
				int l = (p[0] + p[1] + p[2]) / 3;
				if (l > 180) {
					minx = std::min(minx, x);
					miny = std::min(miny, y);
					maxx = std::max(maxx, x);
					maxy = std::max(maxy, y);
				}
			}
		}
		if (maxx >= 0) {
			out.ok = true;
			// The detector box is deliberately a little larger than the glyphs
			// so it always includes a few rows/cols of background.
			out.lines.push_back(MakeLine("marker",
				std::max(0, minx - 2), std::max(0, miny - 1),
				std::min(r.width - 1, maxx + 2), std::min(r.height - 1, maxy + 1)));
		}
		return out;
	});
	auto recognize = RegionRecognizer([](RegionImage const&) -> ocr::OCRResult {
		ocr::OCRResult out;
		out.ok = true;
		ocr::OCRLine line;
		line.text = "known";
		line.confidence = 0.9;
		out.lines.push_back(line);
		return out;
	});

	auto result = ScanVideoTimeline(opts, loader, detect, recognize);
	ASSERT_TRUE(result.ok);
	ASSERT_FALSE(result.events.empty());
	EXPECT_EQ(result.events.size(), 1u);
	// The recovered timing must lie inside the subtitle's true window.
	EXPECT_GE(result.events[0].start_frame, 38);
	EXPECT_LE(result.events[0].end_frame, 62);
	EXPECT_EQ(result.events[0].text, "known");
}
