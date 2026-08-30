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

#include <libaegisub/signal.h>

#include <atomic>
#include <memory>
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

struct HardSubRecognizeOutcome {
	bool ok = false;
	std::string text;
	std::string error;
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

public:
	DialogHardSubScan(agi::Context *context);
	~DialogHardSubScan();

	/// Activate the region selection tool on the video display
	void ActivateRegionTool();
	/// Called by the region tool once the user finishes dragging
	void OnRegionSelected(wxRect const& video_region, int frame);
};
