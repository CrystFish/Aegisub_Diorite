#include <main.h>

#include "hardsub_scan.h"

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

/// Build an 8-bit planar YUV 4:2:0 frame. `y_at`/`uv_at` set the luma and
/// (signed) chroma offsets from 128 for each 2x2 block.
VideoFrame MakeYuvFrame(int width, int height,
                        std::function<unsigned char(int, int)> y_at,
                        std::function<int(int, int)> u_at,
                        std::function<int(int, int)> v_at) {
	VideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.pitch = width;
	frame.flipped = false;
	frame.pix_fmt = 1; // yuv420p
	frame.data.assign(width * height + (width / 2) * (height / 2) * 2, 0);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			frame.data[size_t(y) * width + x] = y_at(x, y);
	size_t u_base = size_t(width) * height;
	size_t v_base = u_base + size_t(width / 2) * (height / 2);
	for (int by = 0; by < height / 2; ++by) {
		for (int bx = 0; bx < width / 2; ++bx) {
			frame.data[u_base + size_t(by) * (width / 2) + bx]
				= static_cast<unsigned char>(128 + u_at(bx, by));
			frame.data[v_base + size_t(by) * (width / 2) + bx]
				= static_cast<unsigned char>(128 + v_at(bx, by));
		}
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

TEST(hardsub_scan, crop_region_yuv420p) {
	// White (Y=235, Cb=Cr=128) left half, black (Y=16) right half.
	auto frame = MakeYuvFrame(4, 2,
		[](int x, int) { return x < 2 ? 235 : 16; },
		[](int, int) { return 0; },
		[](int, int) { return 0; });

	auto left = CropRegion(frame, 0, 0, 2, 1);
	auto right = CropRegion(frame, 2, 0, 2, 1);
	ASSERT_TRUE(left.Valid());
	ASSERT_TRUE(right.Valid());
	// BT.709 limited range: white 235 -> (235, 235, 235), black 16 -> (16,16,16)
	EXPECT_NEAR(left.rgb[0], 235, 2);
	EXPECT_NEAR(left.rgb[1], 235, 2);
	EXPECT_NEAR(left.rgb[2], 235, 2);
	EXPECT_NEAR(right.rgb[0], 16, 2);
	EXPECT_NEAR(right.rgb[1], 16, 2);
	EXPECT_NEAR(right.rgb[2], 16, 2);
}

TEST(hardsub_scan, crop_region_nv12) {
	VideoFrame frame = MakeYuvFrame(4, 2,
		[](int x, int) { return x < 2 ? 235 : 16; },
		[](int, int) { return 0; },
		[](int, int) { return 0; });
	// Convert the planar buffer to NV12 layout (interleaved UV).
	frame.pix_fmt = 2;
	size_t y_bytes = size_t(frame.width) * frame.height;
	size_t chroma = size_t(frame.width / 2) * (frame.height / 2);
	std::vector<unsigned char> nv12(y_bytes + chroma * 2);
	std::copy(frame.data.begin(), frame.data.begin() + y_bytes, nv12.begin());
	for (size_t i = 0; i < chroma; ++i) {
		nv12[y_bytes + i * 2] = frame.data[y_bytes + i];          // U
		nv12[y_bytes + i * 2 + 1] = frame.data[y_bytes + chroma + i]; // V
	}
	frame.data = std::move(nv12);

	auto left = CropRegion(frame, 0, 0, 2, 1);
	ASSERT_TRUE(left.Valid());
	EXPECT_NEAR(left.rgb[0], 235, 2);
	EXPECT_NEAR(left.rgb[1], 235, 2);
	EXPECT_NEAR(left.rgb[2], 235, 2);
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

	auto result = ScanBoundaries(CropRegion(frames[150], 0, 0, width, height), 0, 0, 150, 300, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, run_lo);
	EXPECT_EQ(result.end_frame, run_hi);
}

TEST(hardsub_scan, long_run_finds_exact_boundaries) {
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

	auto result = ScanBoundaries(CropRegion(frames[500], 0, 0, width, height), 0, 0, 500, 900, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, run_lo);
	EXPECT_EQ(result.end_frame, run_hi);
}

TEST(hardsub_scan, splits_run_at_interior_gap) {
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

	// The gap is wide enough to split the run; the base frame's run is the
	// second subtitle.
	auto result = ScanBoundaries(CropRegion(frames[30], 0, 0, width, height), 0, 0, 30, 50, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 22);
	EXPECT_EQ(result.end_frame, 40);
}

TEST(hardsub_scan, fills_single_frame_dip) {
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

	auto result = ScanBoundaries(CropRegion(frames[30], 0, 0, width, height), 0, 0, 30, 50, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 10);
	EXPECT_EQ(result.end_frame, 40);
}

TEST(hardsub_scan, finds_run_off_probe_grid) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Subtitle visible on frames 10..60, base frame not near any probe step.
	for (int i = 0; i < 80; ++i)
		frames.push_back(i >= 10 && i <= 60 ? MakeTextFrame(width, height, 10, 200)
		                                    : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 0;

	auto result = ScanBoundaries(CropRegion(frames[35], 0, 0, width, height), 0, 0, 35, 80, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 10);
	EXPECT_EQ(result.end_frame, 60);
}

TEST(hardsub_scan, survives_noisy_boundary_sample) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	// Run 10..50, but frame 45 is a noisy absent frame shorter than the
	// confirm debounce; the run must stay contiguous.
	for (int i = 0; i < 60; ++i)
		frames.push_back(i >= 10 && i <= 50 && i != 45 ? MakeTextFrame(width, height, 10, 200)
		                                               : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;

	auto result = ScanBoundaries(CropRegion(frames[30], 0, 0, width, height), 0, 0, 30, 60, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 10);
	EXPECT_EQ(result.end_frame, 50);
}

TEST(hardsub_scan, long_run_above_dense_cap_still_exact) {
	const int width = 8, height = 8;
	const int run_lo = 100, run_hi = 1500;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 1700; ++i)
		frames.push_back(i >= run_lo && i <= run_hi ? MakeTextFrame(width, height, 10, 200)
		                                            : MakeFrame(width, height, 10, 10, 10));

	auto loader = [&](int f) { return std::make_shared<VideoFrame>(frames[f]); };
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;

	// The span is above the dense-pass cap, so the scan must refine each
	// boundary with binary search and a small frame-by-frame window while
	// still landing on the exact edges.
	auto result = ScanBoundaries(CropRegion(frames[800], 0, 0, width, height), 0, 0, 800, 1700, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, run_lo);
	EXPECT_EQ(result.end_frame, run_hi);
}

TEST(hardsub_scan, sequential_pass_finds_exact_boundaries) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 3200; ++i)
		frames.push_back(i >= 10 && i <= 3000 ? MakeTextFrame(width, height, 10, 200)
		                                      : MakeFrame(width, height, 10, 10, 10));

	int calls = 0;
	auto loader = [&](int f) {
		++calls;
		return std::make_shared<VideoFrame>(frames[f]);
	};
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;

	auto result = ScanBoundaries(CropRegion(frames[1500], 0, 0, width, height), 0, 0, 1500, 3200, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 10);
	EXPECT_EQ(result.end_frame, 3000);
	// The scan decodes one contiguous ascending pass over the run (a design
	// chosen because sequential decoding is cheap while random seeks are
	// expensive on real codecs), so it never evaluates more than the video.
	EXPECT_LE(calls, 3200);
}

TEST(hardsub_scan, far_from_video_edge_stays_bounded) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	frames.reserve(30000);
	for (int i = 0; i < 30000; ++i)
		frames.push_back(i >= 700 && i <= 1000 ? MakeTextFrame(width, height, 10, 200)
		                                       : MakeFrame(width, height, 10, 10, 10));

	int calls = 0;
	auto loader = [&](int f) {
		++calls;
		return std::make_shared<VideoFrame>(frames[f]);
	};
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;

	// The exponential backward probe overshoots the video start here; the
	// scan must not respond by decoding the whole 0..1000 span. It narrows
	// the transition with a binary search and evaluates only the boundary
	// windows plus a sparse interior.
	auto result = ScanBoundaries(CropRegion(frames[1000], 0, 0, width, height), 0, 0, 1000, 30000, opts, loader);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 700);
	EXPECT_EQ(result.end_frame, 1000);
	// The pass starts at the pre-roll before the run and stops shortly after
	// its end; it must not decode the whole 0..1000 span nor scan to the end
	// of the 30000-frame video.
	EXPECT_LE(calls, 500);
}

TEST(hardsub_scan, sequential_pass_with_keyframes) {
	const int width = 8, height = 8;
	std::vector<VideoFrame> frames;
	for (int i = 0; i < 3000; ++i)
		frames.push_back(i >= 700 && i <= 1000 ? MakeTextFrame(width, height, 10, 200)
		                                       : MakeFrame(width, height, 10, 10, 10));

	int calls = 0;
	auto loader = [&](int f) {
		++calls;
		return std::make_shared<VideoFrame>(frames[f]);
	};
	ScanOptions opts;
	opts.max_frames = 0;
	opts.threshold = 0.5;
	opts.confirm_frames = 2;

	// Keyframes every 60 frames; the pass must align its start to a keyframe
	// and still find the exact edges.
	std::vector<int> keyframes;
	for (int f = 0; f < 3000; f += 60)
		keyframes.push_back(f);

	auto result = ScanBoundaries(CropRegion(frames[800], 0, 0, width, height), 0, 0, 800, 3000,
	                             opts, loader, CancelFn(), ProgressFn(), keyframes);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.start_frame, 700);
	EXPECT_EQ(result.end_frame, 1000);
	EXPECT_LE(calls, 500);
}

TEST(hardsub_scan, boundary_ambiguous_detects_fades) {
	ScanResult r;
	r.ok = true;
	r.start_frame = 100;
	r.end_frame = 200;
	for (int f = 96; f <= 204; ++f) {
		r.frames.push_back(f);
		r.diffs.push_back(f >= 100 && f <= 200 ? 0.9 : 0.05);
	}

	// Clean edges: nothing outside the run has text-like coverage.
	EXPECT_FALSE(BoundaryAmbiguous(r, 0.3, true));
	EXPECT_FALSE(BoundaryAmbiguous(r, 0.3, false));

	// A fade just before the start makes the start boundary ambiguous.
	r.diffs[100 - 96 - 1] = 0.35;
	EXPECT_TRUE(BoundaryAmbiguous(r, 0.3, true));
	EXPECT_FALSE(BoundaryAmbiguous(r, 0.3, false));

	// A fade just after the end makes the end boundary ambiguous.
	r.diffs[100 - 96 - 1] = 0.05;
	r.diffs[200 - 96 + 1] = 0.35;
	EXPECT_FALSE(BoundaryAmbiguous(r, 0.3, true));
	EXPECT_TRUE(BoundaryAmbiguous(r, 0.3, false));

	// Sparse evidence is treated as ambiguous so OCR is never skipped.
	ScanResult sparse;
	sparse.ok = true;
	sparse.start_frame = sparse.end_frame = 5;
	sparse.frames = {0, 5, 10};
	sparse.diffs = {0.0, 1.0, 0.0};
	EXPECT_TRUE(BoundaryAmbiguous(sparse, 0.3, true));
}

TEST(hardsub_scan, compute_presence_applies_hysteresis) {
	// Coverage in the hysteresis zone keeps the previous state: a fading
	// subtitle stays present while the coverage declines through the zone.
	std::vector<double> coverage = {0.0, 0.10, 0.55, 0.35, 0.32, 0.60, 0.28, 0.05};
	auto present = ComputePresence(coverage, {}, 0.5, 0.3);
	ASSERT_EQ(present.size(), coverage.size());
	EXPECT_FALSE(present[0]);
	EXPECT_FALSE(present[1]);
	EXPECT_TRUE(present[2]);   // above enter
	EXPECT_TRUE(present[3]);   // 0.35 is in the zone: hold present
	EXPECT_TRUE(present[4]);   // 0.32 is in the zone: hold present
	EXPECT_TRUE(present[5]);   // back above enter
	EXPECT_FALSE(present[6]);  // 0.28 < exit: flips absent
	EXPECT_FALSE(present[7]);
}

TEST(hardsub_scan, compute_presence_ocr_vetoes_pixels) {
	std::vector<double> coverage = {0.60, 0.60, 0.60};
	std::vector<bool> ocr = {true, false, true};
	auto present = ComputePresence(coverage, ocr, 0.5, 0.3);
	ASSERT_EQ(present.size(), 3u);
	EXPECT_TRUE(present[0]);
	EXPECT_FALSE(present[1]); // pixel present but OCR missed: vetoed
	EXPECT_TRUE(present[2]);
}

TEST(hardsub_scan, fill_short_dips_keeps_edges) {
	std::vector<bool> present = {false, false, true, false, true, false, false};
	auto filled = FillShortDips(present, 2);
	ASSERT_EQ(filled.size(), present.size());
	// The interior 1-frame dip at index 3 is filled; leading and trailing
	// absent runs are preserved.
	EXPECT_EQ(filled, (std::vector<bool>{false, false, true, true, true, false, false}));

	auto unfilled = FillShortDips(present, 1);
	EXPECT_EQ(unfilled, present);
}

TEST(hardsub_scan, run_start_and_end_follow_filled_series) {
	std::vector<bool> present = {false, true, true, true, true, false, true};
	EXPECT_EQ(RunStart(present, 2), 1);
	EXPECT_EQ(RunEnd(present, 2), 4);
	EXPECT_EQ(RunStart(present, 6), 6);
	EXPECT_EQ(RunEnd(present, 6), 6);
	EXPECT_EQ(RunStart(present, 5), 5); // anchor absent: returns anchor
	EXPECT_EQ(RunEnd(present, 5), 5);
}

TEST(hardsub_scan, snap_boundary_uses_dual_evidence) {
	std::vector<double> coverage = {0.2, 0.35, 0.45, 0.5, 0.5};
	std::vector<bool> ocr = {false, true, false, true, true};

	// Earliest frame with coverage >= exit and an OCR box: index 1.
	EXPECT_EQ(SnapBoundaryFromEvidence(coverage, ocr, 0.3, true, 3), 1);
	// Latest: index 4.
	EXPECT_EQ(SnapBoundaryFromEvidence(coverage, ocr, 0.3, false, 2), 4);
	// Fallback when nothing qualifies.
	EXPECT_EQ(SnapBoundaryFromEvidence(coverage, {false, false, false, false, false}, 0.3, true, 2), 2);
}
