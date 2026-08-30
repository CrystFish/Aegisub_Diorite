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

#include "scan_video_decoder.h"

#ifdef WITH_FFMS2

#include "ffmpegsource_common.h"
#include "options.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <cstring>

namespace hardsub {

ScanVideoDecoder::ScanVideoDecoder(agi::fs::path const& filename, std::string& error) {
	error.clear();

	char errbuf[1024];
	FFMS_ErrorInfo err;
	err.Buffer = errbuf;
	err.BufferSize = sizeof(errbuf);
	err.ErrorType = FFMS_ERROR_SUCCESS;
	err.SubType = FFMS_ERROR_SUCCESS;

	// FFMS_Init is idempotent; the main provider has already called it.
	FFMS_SetHardwareDecoding(OPT_GET("Provider/Video/FFmpegSource/Hardware Decoding")->GetBool());

	std::string narrow = filename.string();
	FFMS_Index *idx = nullptr;

	// Reuse the provider's on-disk index cache when it is fresh.
	FFmpegSourceProvider cache_source(nullptr);
	auto cache_name = cache_source.GetCacheFilename(filename);
	idx = FFMS_ReadIndex(cache_name.string().c_str(), &err);
	if (idx && !FFMS_IndexBelongsToFile(idx, narrow.c_str(), &err)) {
		FFMS_DestroyIndex(idx);
		idx = nullptr;
	}

	if (!idx) {
		FFMS_Indexer *ix = FFMS_CreateIndexer(narrow.c_str(), &err);
		if (!ix) {
			error = std::string("Failed to open video for scanning: ") + errbuf;
			return;
		}
		FFMS_TrackTypeIndexSettings(ix, FFMS_TYPE_VIDEO, 1, 0);
		idx = FFMS_DoIndexing2(ix, cache_source.GetErrorHandlingMode(), &err);
		if (!idx) {
			error = std::string("Failed to index video for scanning: ") + errbuf;
			return;
		}
	}
	index_ = idx;

	int track = FFMS_GetFirstIndexedTrackOfType(idx, FFMS_TYPE_VIDEO, &err);
	if (track < 0) {
		error = "The video has no indexed video track.";
		return;
	}

	int threads = OPT_GET("Provider/Video/FFmpegSource/Decoding Threads")->GetInt();
	int seek_mode = FFMS_SEEK_NORMAL;
	if (OPT_GET("Provider/Video/FFmpegSource/Unsafe Seeking")->GetBool())
		seek_mode = FFMS_SEEK_UNSAFE;

	source_ = FFMS_CreateVideoSource(narrow.c_str(), track, idx, threads, seek_mode, &err);
	if (!source_) {
		error = std::string("Failed to open video source for scanning: ") + errbuf;
		return;
	}

	const FFMS_VideoProperties *vp = FFMS_GetVideoProperties(static_cast<FFMS_VideoSource*>(source_));
	if (!vp) {
		error = "Failed to read video properties for scanning.";
		return;
	}
	frame_count_ = vp->NumFrames;

	// Decode the first frame to learn the native pixel format. The scan
	// decoder deliberately does not call FFMS_SetOutputFormatV2: keeping the
	// native YUV output avoids the full-frame conversion.
	const FFMS_Frame *fr = FFMS_GetFrame(static_cast<FFMS_VideoSource*>(source_), 0, &err);
	if (!fr) {
		error = std::string("Failed to decode the first frame for scanning: ") + errbuf;
		return;
	}
	width_ = fr->EncodedWidth;
	height_ = fr->EncodedHeight;

	int yuv420 = FFMS_GetPixFmt("yuv420p");
	int nv12 = FFMS_GetPixFmt("nv12");
	int p010 = FFMS_GetPixFmt("p010");
	if (fr->ConvertedPixelFormat == yuv420)
		pix_fmt_ = 1;
	else if (fr->ConvertedPixelFormat == nv12)
		pix_fmt_ = 2;
	else if (fr->ConvertedPixelFormat == p010)
		pix_fmt_ = 3;
	else {
		// Unsupported native format: fall back to the caller's normal path
		// instead of silently producing wrong colors.
		error = "The video's pixel format is not supported by the fast scan path.";
		return;
	}
}

ScanVideoDecoder::~ScanVideoDecoder() {
	if (source_)
		FFMS_DestroyVideoSource(static_cast<FFMS_VideoSource*>(source_));
	if (index_)
		FFMS_DestroyIndex(static_cast<FFMS_Index*>(index_));
}

std::vector<int> ScanVideoDecoder::GetKeyFrames() const {
	std::vector<int> keyframes;
	if (!source_)
		return keyframes;
	FFMS_Track *track = FFMS_GetTrackFromVideo(static_cast<FFMS_VideoSource*>(source_));
	for (int f = 0; f < frame_count_; ++f) {
		const FFMS_FrameInfo *fi = FFMS_GetFrameInfo(track, f);
		if (fi && fi->KeyFrame)
			keyframes.push_back(f);
	}
	return keyframes;
}

std::shared_ptr<VideoFrame> ScanVideoDecoder::GetFrame(int n) const {
	if (!source_ || n < 0 || n >= frame_count_)
		return {};

	char errbuf[1024];
	FFMS_ErrorInfo err;
	err.Buffer = errbuf;
	err.BufferSize = sizeof(errbuf);
	err.ErrorType = FFMS_ERROR_SUCCESS;
	err.SubType = FFMS_ERROR_SUCCESS;

	const FFMS_Frame *fr = FFMS_GetFrame(static_cast<FFMS_VideoSource*>(source_), n, &err);
	if (!fr)
		return {};

	// Copy the luma plane and the chroma planes into the reusable buffer.
	// The Y plane stride is always Linesize[0]; 10-bit formats store two
	// bytes per sample so the total byte count is scaled accordingly.
	int bytes_per_sample = pix_fmt_ == 3 ? 2 : 1;
	size_t y_bytes = static_cast<size_t>(fr->Linesize[0]) * height_ * bytes_per_sample;
	size_t chroma_rows = height_ / 2;
	size_t chroma_stride = pix_fmt_ == 1
		? static_cast<size_t>(fr->Linesize[0]) / 2
		: static_cast<size_t>(fr->Linesize[0]);
	size_t one_chroma = chroma_stride * chroma_rows * bytes_per_sample;
	size_t chroma_bytes = one_chroma * (pix_fmt_ == 1 ? 2 : 1);

	frame_store_.data.resize(y_bytes + chroma_bytes);
	std::memcpy(frame_store_.data.data(), fr->Data[0], y_bytes);
	std::memcpy(frame_store_.data.data() + y_bytes, fr->Data[1], one_chroma);
	if (pix_fmt_ == 1)
		std::memcpy(frame_store_.data.data() + y_bytes + one_chroma, fr->Data[2], one_chroma);
	frame_store_.width = width_;
	frame_store_.height = height_;
	frame_store_.pitch = fr->Linesize[0] * bytes_per_sample;
	frame_store_.flipped = false;
	frame_store_.pix_fmt = pix_fmt_;

	// The chroma planes immediately follow the luma plane in the copy.
	return std::shared_ptr<VideoFrame>(&frame_store_, [](VideoFrame*) {});
}

} // namespace hardsub

#else

namespace hardsub {

ScanVideoDecoder::ScanVideoDecoder(agi::fs::path const&, std::string& error) {
	error = "FFMS2 support is not available in this build.";
}

ScanVideoDecoder::~ScanVideoDecoder() = default;

std::vector<int> ScanVideoDecoder::GetKeyFrames() const {
	return {};
}

std::shared_ptr<VideoFrame> ScanVideoDecoder::GetFrame(int) const {
	return {};
}

} // namespace hardsub

#endif // WITH_FFMS2
