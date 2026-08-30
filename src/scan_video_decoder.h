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

#include <libaegisub/fs_fwd.h>

#include <memory>
#include <string>
#include <vector>

namespace hardsub {

/// Lightweight FFMS2 decoder used by the hard subtitle scan.
///
/// Decodes frames in their native YUV format instead of the full-frame BGRA
/// conversion the normal video provider performs. On 4K video that conversion
/// dominates the per-frame cost (about 100 ms on this machine versus a few
/// milliseconds for the decode itself), which is what made the dense
/// per-frame scan of a long subtitle take close to a minute. The scan only
/// ever reads a small region of each frame, so callers convert just that
/// region with CropRegion (which understands the YUV formats below).
///
/// Not thread-safe: the caller must use it from a single thread (the scan
/// thread). It shares nothing with the main video provider.
class ScanVideoDecoder {
public:
	/// Opens `filename` with FFMS2, reusing the provider's index cache when
	/// possible. `error` receives a human-readable message on failure.
	explicit ScanVideoDecoder(agi::fs::path const& filename, std::string& error);
	~ScanVideoDecoder();

	ScanVideoDecoder(ScanVideoDecoder const&) = delete;
	ScanVideoDecoder& operator=(ScanVideoDecoder const&) = delete;

	bool Valid() const { return source_ != nullptr; }

	int GetFrameCount() const { return frame_count_; }
	int GetWidth() const { return width_; }
	int GetHeight() const { return height_; }
	std::vector<int> GetKeyFrames() const;

	/// Returns a native-format frame (VideoFrame::pix_fmt set) or nullptr on
	/// failure. The returned frame is only valid until the next call; the
	/// scan uses it immediately for a region crop.
	std::shared_ptr<VideoFrame> GetFrame(int n) const;

private:
	void *source_ = nullptr; // FFMS_VideoSource*
	void *index_ = nullptr;  // FFMS_Index*
	int width_ = 0;
	int height_ = 0;
	int frame_count_ = 0;
	int pix_fmt_ = 0;
	mutable VideoFrame frame_store_;
};

} // namespace hardsub
