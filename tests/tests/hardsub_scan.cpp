#include <main.h>

#include "hardsub_scan.h"

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

/// Solid `bg` background with a one-pixel-wide vertical bar of `text` color at
/// column `bar_x` (rows 1..height-2), giving the frame a text-like luminance
/// outlier.
VideoFrame MakeTextFrame(int width, int height, unsigned char bg, unsigned char text,
                         int bar_x = 2) {
	VideoFrame frame = MakeFrame(width, height, bg, bg, bg);
	for (int y = 1; y < height - 1; ++y) {
		size_t off = size_t(y) * frame.pitch + size_t(bar_x) * 4;
		frame.data[off] = text;     // B
		frame.data[off + 1] = text; // G
		frame.data[off + 2] = text; // R
	}
	return frame;
}

} // namespace

TEST(hardsub_scan, crop_region_reads_bgra) {
	VideoFrame frame = MakeFrame(4, 2, 10, 20, 30);

	auto crop = CropRegion(frame, 1, 0, 2, 1);
	ASSERT_TRUE(crop.Valid());
	EXPECT_EQ(crop.width, 2);
	EXPECT_EQ(crop.height, 1);
	EXPECT_EQ(crop.rgb[0], 10); // R
	EXPECT_EQ(crop.rgb[1], 20); // G
	EXPECT_EQ(crop.rgb[2], 30); // B
}

TEST(hardsub_scan, crop_region_clamps_to_frame) {
	VideoFrame frame = MakeFrame(4, 2, 5, 5, 5);

	auto crop = CropRegion(frame, -1, -1, 100, 100);
	ASSERT_TRUE(crop.Valid());
	EXPECT_EQ(crop.width, 4);
	EXPECT_EQ(crop.height, 2);
}

TEST(hardsub_scan, mean_abs_diff) {
	auto a = MakeFrame(2, 1, 0, 0, 0);
	auto b = MakeFrame(2, 1, 0, 0, 0);
	auto c = MakeFrame(2, 1, 255, 0, 0);

	EXPECT_EQ(MeanAbsDiff(CropRegion(a, 0, 0, 2, 1), CropRegion(b, 0, 0, 2, 1)), 0.0);
	EXPECT_NEAR(MeanAbsDiff(CropRegion(a, 0, 0, 2, 1), CropRegion(c, 0, 0, 2, 1)), 255.0 / 3.0, 0.001);
	EXPECT_EQ(MeanAbsDiff(CropRegion(a, 0, 0, 2, 1), CropRegion(a, 0, 0, 1, 1)), 255.0);
}

TEST(hardsub_scan, region_changed_ratio) {
	VideoFrame a = MakeFrame(2, 1, 0, 0, 0);
	VideoFrame b = MakeFrame(2, 1, 200, 0, 0);

	EXPECT_EQ(RegionChangedRatio(CropRegion(a, 0, 0, 2, 1), CropRegion(a, 0, 0, 2, 1)), 0.0);
	EXPECT_EQ(RegionChangedRatio(CropRegion(a, 0, 0, 2, 1), CropRegion(b, 0, 0, 2, 1)), 1.0);

	// One changed pixel out of two.
	VideoFrame mixed;
	mixed.width = 2;
	mixed.height = 1;
	mixed.pitch = 8;
	mixed.flipped = false;
	mixed.data.assign(8, 0);
	mixed.data[6] = 200; // second pixel R = 200 (BGRA, pixel 1 at byte 4), first stays 0
	EXPECT_NEAR(RegionChangedRatio(CropRegion(a, 0, 0, 2, 1), CropRegion(mixed, 0, 0, 2, 1)), 0.5, 0.001);
}

TEST(hardsub_scan, template_text_pixels_and_coverage) {
	auto tpl = CropRegion(MakeTextFrame(8, 8, 10, 200), 0, 0, 8, 8);
	auto text_pixels = TemplateTextPixels(tpl);
	ASSERT_FALSE(text_pixels.empty());
	// The bright bar is the only luminance outlier; every text pixel must
	// match the template itself.
	EXPECT_DOUBLE_EQ(TemplateTextCoverage(tpl, tpl, text_pixels), 1.0);
	// A plain background frame must match none of the text pixels.
	auto plain = CropRegion(MakeFrame(8, 8, 10, 10, 10), 0, 0, 8, 8);
	EXPECT_DOUBLE_EQ(TemplateTextCoverage(tpl, plain, text_pixels), 0.0);
}

TEST(hardsub_scan, coverage_survives_background_changes) {
	auto tpl = CropRegion(MakeTextFrame(8, 8, 10, 200), 0, 0, 8, 8);
	auto text_pixels = TemplateTextPixels(tpl);
	ASSERT_FALSE(text_pixels.empty());

	// Same bar over a completely different background must still count as the
	// subtitle being present; the same background without the bar must not.
	auto moved = MakeTextFrame(8, 8, 120, 200);
	EXPECT_GE(TemplateTextCoverage(tpl, CropRegion(moved, 0, 0, 8, 8), text_pixels), 0.9);
	auto empty = MakeFrame(8, 8, 120, 120, 120);
	EXPECT_LT(TemplateTextCoverage(tpl, CropRegion(empty, 0, 0, 8, 8), text_pixels), 0.1);
}

TEST(hardsub_scan, finds_run_around_base_frame) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Subtitle visible on frames 3..7 (template = subtitle), absent elsewhere.
	for (int i = 0; i < 10; ++i)
		frames.push_back(i >= 3 && i <= 7 ? MakeTextFrame(width, height, 10, 200)
		                                  : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 0;

	auto result = ScanBoundaries(CropRegion(frames[5], 0, 0, width, height), 0, 0, 5, 10, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 3);
	EXPECT_EQ(result.end_frame, 7);
}

TEST(hardsub_scan, scan_survives_background_changes) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Subtitle visible 5..9; the background differs on every visible frame.
	for (int i = 0; i < 12; ++i) {
		if (i >= 5 && i <= 9)
			frames.push_back(MakeTextFrame(width, height, 30 + i, 200));
		else
			frames.push_back(MakeFrame(width, height, 10, 10, 10));
	}

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 0;

	auto result = ScanBoundaries(CropRegion(frames[6], 0, 0, width, height), 0, 0, 6, 12, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 5);
	EXPECT_EQ(result.end_frame, 9);
}

TEST(hardsub_scan, debounces_single_frame_flicker) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Run on 5..9 with one missing frame at 7.
	for (int i = 0; i < 12; ++i) {
		bool visible = i >= 5 && i <= 9 && i != 7;
		frames.push_back(visible ? MakeTextFrame(width, height, 10, 200)
		                         : MakeFrame(width, height, 10, 10, 10));
	}

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;

	auto result = ScanBoundaries(CropRegion(frames[6], 0, 0, width, height), 0, 0, 6, 12, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 5);
	EXPECT_EQ(result.end_frame, 9);
}

TEST(hardsub_scan, drops_short_runs) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Only frames 6..9 contain the subtitle.
	for (int i = 0; i < 12; ++i)
		frames.push_back(i >= 6 && i <= 9 ? MakeTextFrame(width, height, 10, 200)
		                                  : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;

	auto result = ScanBoundaries(CropRegion(frames[8], 0, 0, width, height), 0, 0, 8, 12, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 6);
	EXPECT_EQ(result.end_frame, 9);
}

TEST(hardsub_scan, fails_when_base_frame_not_visible) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 10; ++i)
		frames.push_back(i >= 3 && i <= 7 ? MakeTextFrame(width, height, 10, 200)
		                                  : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 0;

	auto result = ScanBoundaries(CropRegion(frames[5], 0, 0, width, height), 0, 0, 1, 10, opts, loader);
	EXPECT_FALSE(result.ok);
}

TEST(hardsub_scan, fails_without_text_like_template) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 10; ++i)
		frames.push_back(MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;

	// A solid template has no luminance outliers, so the scan reports a
	// diagnosable error instead of scanning with an empty text mask.
	auto result = ScanBoundaries(CropRegion(frames[5], 0, 0, width, height), 0, 0, 5, 10, opts, loader);
	EXPECT_FALSE(result.ok);
	EXPECT_FALSE(result.error.empty());
}

TEST(hardsub_scan, respects_max_range) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 20; ++i)
		frames.push_back(i >= 3 && i <= 17 ? MakeTextFrame(width, height, 10, 200)
		                                   : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 1;
	opts.threshold = 0.5;
	opts.confirm_frames = 0;

	// Base frame 10 is visible, but the max range stops the run at 9/11.
	auto result = ScanBoundaries(CropRegion(frames[10], 0, 0, width, height), 0, 0, 10, 20, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 9);
	EXPECT_EQ(result.end_frame, 11);
}

TEST(hardsub_scan, adaptive_walk_finds_long_run_without_range) {
	const int width = 8, height = 8;
	const int run_lo = 50, run_hi = 250;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 300; ++i)
		frames.push_back(i >= run_lo && i <= run_hi ? MakeTextFrame(width, height, 10, 200)
		                                            : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0; // no explicit range: the walk must find the edges itself
	opts.threshold = 0.5;
	opts.confirm_frames = 2;
	opts.stride = 15;

	auto result = ScanBoundaries(CropRegion(frames[150], 0, 0, width, height), 0, 0, 150, 300, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, run_lo);
	EXPECT_EQ(result.end_frame, run_hi);
}

TEST(hardsub_scan, large_stride_still_finds_exact_boundaries) {
	const int width = 8, height = 8;
	const int run_lo = 10, run_hi = 830;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 900; ++i)
		frames.push_back(i >= run_lo && i <= run_hi ? MakeTextFrame(width, height, 10, 200)
		                                            : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;
	opts.stride = 32;

	// With stride 32 the coarse samples land on 20 and 820; the frame-by-frame
	// refinement (plus the bound probe) must still pin the exact 10/830 edges.
	auto result = ScanBoundaries(CropRegion(frames[500], 0, 0, width, height), 0, 0, 500, 900, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, run_lo);
	EXPECT_EQ(result.end_frame, run_hi);
}

TEST(hardsub_scan, stride_one_catches_mid_run_gap) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Subtitle 10..19 and 22..40, with a 2-frame gap at 20..21 that splits the
	// run (confirm = 2). The base frame 30 is in the second half.
	for (int i = 0; i < 50; ++i) {
		bool visible = (i >= 10 && i <= 19) || (i >= 22 && i <= 40);
		frames.push_back(visible ? MakeTextFrame(width, height, 10, 200)
		                         : MakeFrame(width, height, 10, 10, 10));
	}

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;
	opts.stride = 1;

	auto result = ScanBoundaries(CropRegion(frames[30], 0, 0, width, height), 0, 0, 30, 50, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 22);
	EXPECT_EQ(result.end_frame, 40);
}

TEST(hardsub_scan, stride_one_fills_single_frame_gap) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Subtitle 10..40 with a 1-frame dip at 20: shorter than confirm, so it is
	// filled and the whole run stays contiguous.
	for (int i = 0; i < 50; ++i) {
		bool visible = i >= 10 && i <= 40 && i != 20;
		frames.push_back(visible ? MakeTextFrame(width, height, 10, 200)
		                         : MakeFrame(width, height, 10, 10, 10));
	}

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;
	opts.stride = 1;

	auto result = ScanBoundaries(CropRegion(frames[30], 0, 0, width, height), 0, 0, 30, 50, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 10);
	EXPECT_EQ(result.end_frame, 40);
}

TEST(hardsub_scan, stride_sampling_finds_long_run) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Subtitle visible on frames 10..60, base frame not on the stride grid.
	for (int i = 0; i < 80; ++i)
		frames.push_back(i >= 10 && i <= 60 ? MakeTextFrame(width, height, 10, 200)
		                                    : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 0;
	opts.stride = 15;

	auto result = ScanBoundaries(CropRegion(frames[35], 0, 0, width, height), 0, 0, 35, 80, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 10);
	EXPECT_EQ(result.end_frame, 60);
}

TEST(hardsub_scan, stride_window_survives_noisy_boundary_sample) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Run 10..50, but frame 45 is a noisy absent frame; 45 also happens to be
	// on the stride grid. The refinement window must extend past it.
	for (int i = 0; i < 60; ++i)
		frames.push_back(i >= 10 && i <= 50 && i != 45 ? MakeTextFrame(width, height, 10, 200)
		                                               : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;
	opts.stride = 15;

	auto result = ScanBoundaries(CropRegion(frames[30], 0, 0, width, height), 0, 0, 30, 60, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 10);
	EXPECT_EQ(result.end_frame, 50);
}

TEST(hardsub_scan, stride_limits_loader_calls) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 200; ++i)
		frames.push_back(i >= 60 && i <= 120 ? MakeTextFrame(width, height, 10, 200)
		                                     : MakeFrame(width, height, 10, 10, 10));

	int calls = 0;
	auto loader = [&](int f) {
		++calls;
		return std::make_shared<VideoFrame>(frames[f]);
	};
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 0;
	opts.stride = 15;

	auto result = ScanBoundaries(CropRegion(frames[90], 0, 0, width, height), 0, 0, 90, 200, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 60);
	EXPECT_EQ(result.end_frame, 120);
	// Exponential probes plus narrow boundary refinements: roughly 30 frames
	// per side. Far below the 200 frames of a full scan.
	EXPECT_LE(calls, 80);
}
