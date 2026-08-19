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

#include "ocr_result.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>
#include <string>

namespace ocr {

/// A persistent PaddleOCR-json child process in anonymous pipe mode.
///
/// The engine is started once without an image argument, then each request is
/// sent as one JSON line on stdin and the response is read back as one JSON
/// line on stdout. This avoids the per-image process startup cost of the
/// one-shot CLI mode. The runtime is only bundled on Windows; on other
/// platforms Start() fails gracefully and callers should fall back to the
/// one-shot engine.
///
/// Start() blocks until the engine reports that the models are loaded, so a
/// failure surfaces there instead of on the first request. The engine does
/// not support toggling det/rec per request; detection-only callers use
/// RunImage()'s detect_only flag, which strips recognition text from the
/// response after the engine has run its full pipeline.
///
/// Instances are not thread-safe: all calls should come from a single thread
/// (typically the scan worker), and Stop() must not race other calls.
class OCRProcess {
public:
	OCRProcess();
	~OCRProcess();

	OCRProcess(OCRProcess const&) = delete;
	OCRProcess& operator=(OCRProcess const&) = delete;

	/// Launch the pipe-mode engine. Returns false and fills diagnostic on
	/// failure or when the platform does not support the persistent process.
	bool Start(agi::fs::path const& executable, agi::fs::path const& models_dir,
	           agi::fs::path const& config_path, std::string& diagnostic);

	/// Locate the bundled OCR runtime the one-shot engine would use. Returns
	/// false and fills diagnostic when the runtime is not installed or is
	/// incomplete.
	static bool FindRuntime(agi::fs::path& executable, agi::fs::path& models_dir,
	                        agi::fs::path& config_path, std::string& diagnostic);

	/// Run one image through the running process. Synchronous; cancel is
	/// polled while waiting for the response.
	OCRResult RunImage(agi::fs::path const& image_path, OCROptions const& options,
	                   bool detect_only, std::function<bool()> const& cancel = {});

	/// Send the exit command and terminate the child if needed.
	void Stop();
	bool IsRunning() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace ocr
