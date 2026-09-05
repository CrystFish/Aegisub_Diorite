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

#include "hardsub_scan.h"
#include "value_event.h"

#include <libaegisub/fs_fwd.h>
#include <libaegisub/signal.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <wx/dialog.h>
#include <wx/gdicmn.h>

class wxButton;
class wxCheckBox;
class wxGauge;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;
class PersistLocation;
struct OcrWarmState;
namespace agi { struct Context; }
namespace ocr { struct OCROptions; struct OCRResult; class OCRProcess; }

struct HardSubRecognizeOutcome {
	bool ok = false;
	std::string text;
	std::string error;
	/// Recognition generation this result belongs to; stale results from an
	/// older region selection are ignored by the dialog.
	int generation = 0;
};

struct HardSubScanProgress {
	int done = 0;
	int total = 0;
};

struct HardSubScanOutcome {
	bool ok = false;
	bool cancelled = false;
	/// Set on the intermediate pixel-scan result posted before the OCR
	/// verification phases finish. The final event resets it.
	bool preliminary = false;
	/// Set when the strict OCR content check trimmed the range and the final
	/// duration is implausibly long for the reference text (likely merged
	/// consecutive subtitles).
	bool suspect_merge = false;
	std::string error;
	hardsub::ScanResult result;
	int start_ms = 0;
	int end_ms = 0;
};

/// Modeless dialog for hard subtitle scanning: select a region on the video,
/// recognize its text, detect precise start/end frames and insert a new line.
class DialogHardSubScan final : public wxDialog {
	agi::Context *c;

	// Controls
	wxButton *select_button = nullptr;
	wxButton *clear_button = nullptr;
	wxStaticText *region_label = nullptr;
	wxButton *recognize_button = nullptr;
	wxTextCtrl *text_ctrl = nullptr;
	wxSpinCtrl *max_range_spin = nullptr;
	wxSpinCtrl *threshold_spin = nullptr;
	wxSpinCtrl *confirm_spin = nullptr;
	wxSpinCtrl *min_duration_spin = nullptr;
	wxCheckBox *ocr_confirm_check = nullptr;
	wxCheckBox *strict_ocr_check = nullptr;
	wxButton *scan_button = nullptr;
	wxGauge *progress = nullptr;
	wxStaticText *status_label = nullptr;
	wxStaticText *result_label = nullptr;
	wxButton *insert_button = nullptr;
	wxButton *copy_button = nullptr;

	// State
	hardsub::RegionImage region_template_;
	wxRect region_;          ///< Selected region in video pixel coordinates
	int base_frame_ = -1;
	bool scanning_ = false;
	std::atomic<bool> cancel_scan_{false};
	std::atomic<bool> recognize_alive_{true};
	std::thread scan_thread_;
	/// Shared state for the detection-only OCR engine pre-warmed in the
	/// background while the dialog is open. Owned by a shared_ptr so the
	/// warmup task can outlive the dialog safely (no joins, no dangling).
	std::shared_ptr<OcrWarmState> ocr_warm_state_;
	/// Shared state for the full-pipeline (det+rec+cls) OCR engine, used by
	/// both region recognition and the strict scan text check so the model
	/// is loaded only once per dialog lifetime.
	std::shared_ptr<OcrWarmState> ocr_full_state_;
	/// Serializes requests to the shared full-pipeline engine. OCRProcess is
	/// not thread-safe, and the region recognition task and the scan thread
	/// both call into it.
	std::mutex ocr_full_run_mutex_;
	/// Monotonic recognition generation. Bumped on every StartRecognize and
	/// on ClearState so results posted by older tasks are ignored.
	int recognize_generation_ = 0;
	/// True while a recognition for the current region is still running.
	bool recognize_pending_ = false;
	/// True once the latest generation's recognition completed (successfully
	/// or not), i.e. the text box no longer holds an older region's text.
	bool recognized_text_fresh_ = false;
	/// True once the user manually edited the text box for the current
	/// region; lets a hand-typed line be inserted even when OCR produced no
	/// result, while leftover text from an older region stays blocked.
	bool text_edited_ = false;
	/// Playback was running when the scan started; resume it when the scan
	/// finishes so the video worker is not contended during the scan.
	bool resume_playback_ = false;
	hardsub::ScanResult scan_result_;
	int scan_start_ms_ = 0;
	int scan_end_ms_ = 0;
	std::vector<agi::signal::Connection> connections;
	std::unique_ptr<PersistLocation> persist;

	void CreateControls();
	void UpdateControls();
	void SetStatus(wxString const& text);
	void GrabTemplate(int frame);
	void StartRecognize(int frame);
	void StartOcrWarmup();
	void CancelScan();
	void ClearState();
	void OnVideoChanged(class AsyncVideoProvider *provider);

	void OnSelect(wxCommandEvent&);
	void OnClear(wxCommandEvent&);
	void OnRecognize(wxCommandEvent&);
	void OnScan(wxCommandEvent&);
	void OnInsert(wxCommandEvent&);
	void OnCopy(wxCommandEvent&);
	void OnClose(wxCloseEvent&);
	void OnRecognizeDone(ValueEvent<HardSubRecognizeOutcome>& event);
	void OnScanProgress(ValueEvent<HardSubScanProgress>& event);
	void OnScanPixelDone(ValueEvent<HardSubScanOutcome>& event);
	void OnScanDone(ValueEvent<HardSubScanOutcome>& event);

	/// Return the dialog's full-pipeline OCR engine, (re)starting it when it
	/// is not running. Blocks while the model loads; fills diagnostic on
	/// failure.
	std::shared_ptr<ocr::OCRProcess> GetFullOcrProcess(std::string& diagnostic);
	/// Run one image through the shared full-pipeline engine, serialized
	/// against concurrent callers. Falls back to the one-shot engine on
	/// platforms without the persistent runtime.
	ocr::OCRResult RunFullOcr(agi::fs::path const& image_path,
	                          ocr::OCROptions const& options,
	                          std::function<bool()> const& cancel);

public:
	DialogHardSubScan(agi::Context *context);
	~DialogHardSubScan();

	/// Activate the region selection tool on the video display
	void ActivateRegionTool();
	/// Called by the region tool once the user finishes dragging
	void OnRegionSelected(wxRect const& video_region, int frame);
};
