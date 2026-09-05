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

#include "hardsub_timeline_scan.h"
#include "value_event.h"

#include <libaegisub/fs_fwd.h>
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
class wxListCtrl;
class wxSpinCtrl;
class wxSpinEvent;
class wxStaticText;
class PersistLocation;
struct OcrWarmState;
namespace agi { struct Context; }
namespace ocr { struct OCROptions; }

struct HardSubScanVideoProgress {
	int done = 0;
	int total = 0;
	int ocr_samples = 0;
	long long elapsed_ms = 0;
};

struct HardSubScanVideoOutcome {
	bool ok = false;
	bool cancelled = false;
	std::string error;
	std::vector<hardsub::TimelineRowEvent> events;
};

/// Modeless dialog for scanning an entire video for burned-in subtitles inside a
/// chosen region, with per-row timing and text, and a review list.
class DialogHardSubScanVideo final : public wxDialog {
	agi::Context *c;

	// Controls
	wxButton *select_button = nullptr;
	wxStaticText *region_label = nullptr;
	wxCheckBox *center_only_check = nullptr;
	wxSpinCtrl *center_band_spin = nullptr;
	wxSpinCtrl *min_duration_spin = nullptr;
	wxSpinCtrl *detect_every_spin = nullptr;
	wxCheckBox *merge_check = nullptr;
	wxCheckBox *replace_newlines_check = nullptr;
	wxSpinCtrl *region_x_spin = nullptr;
	wxSpinCtrl *region_y_spin = nullptr;
	wxSpinCtrl *region_w_spin = nullptr;
	wxSpinCtrl *region_h_spin = nullptr;
	wxSpinCtrl *pct_top_spin = nullptr;
	wxSpinCtrl *pct_right_spin = nullptr;
	wxSpinCtrl *pct_bottom_spin = nullptr;
	wxSpinCtrl *pct_left_spin = nullptr;
	wxButton *scan_button = nullptr;
	wxGauge *progress = nullptr;
	wxStaticText *status_label = nullptr;
	wxListCtrl *result_list = nullptr;
	wxButton *copy_button = nullptr;
	wxButton *insert_button = nullptr;
	wxButton *export_button = nullptr;
	wxButton *clear_button = nullptr;

	// State
	wxRect region_;              ///< Region in video pixel coordinates
	bool updating_region_spins_ = false;
	bool scanning_ = false;
	std::atomic<bool> cancel_scan_{false};
	std::atomic<int> scan_ocr_samples_{0};
	std::thread scan_thread_;
	std::shared_ptr<OcrWarmState> ocr_full_state_;
	std::vector<hardsub::TimelineRowEvent> events_;
	bool resume_playback_ = false;
	std::vector<agi::signal::Connection> connections;
	std::unique_ptr<PersistLocation> persist;

	void CreateControls();
	void UpdateControls();
	void SetStatus(wxString const& text);
	void UpdateRegionLabel();
	void GetVideoSize(int& w, int& h) const;
	void SyncRegionSpins();
	void PopulateResults();
	void AppendResultRow(hardsub::TimelineRowEvent const& ev);
	std::vector<size_t> ResultIndices() const;
	void ActivateRegionTool();
	void StartOcrWarmup();

	void OnSelect(wxCommandEvent&);
	void OnRegionPixelChanged(wxSpinEvent&);
	void OnRegionPercentChanged(wxSpinEvent&);
	void OnScan(wxCommandEvent&);
	void OnCopy(wxCommandEvent&);
	void OnInsert(wxCommandEvent&);
	void OnExport(wxCommandEvent&);
	void OnClear(wxCommandEvent&);
	void OnClose(wxCloseEvent&);
	void OnRegionSelected(wxRect const& video_region);
	void OnScanProgress(ValueEvent<HardSubScanVideoProgress>& event);
	void OnScanRow(ValueEvent<hardsub::TimelineRowEvent>& event);
	void OnScanDone(ValueEvent<HardSubScanVideoOutcome>& event);
	void OnVideoChanged(class AsyncVideoProvider *provider);

	void CancelScan();
	void ClearResults();

public:
	explicit DialogHardSubScanVideo(agi::Context *context);
	~DialogHardSubScanVideo();
};
