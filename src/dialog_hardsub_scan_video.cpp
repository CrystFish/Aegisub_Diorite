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

#include "dialog_hardsub_scan_video.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "async_video_provider.h"
#include "compat.h"
#include "command/command.h"
#include "format.h"
#include "hardsub_region_tool.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "ocr/ocr_engine.h"
#include "ocr/ocr_process.h"
#include "options.h"
#include "persist_location.h"
#include "project.h"
#include "scan_video_decoder.h"
#include "selection_controller.h"
#include "utils.h"
#include "video_controller.h"
#include "video_display.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>

#include <boost/algorithm/string/replace.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <typeinfo>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/image.h>
#include <wx/intl.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

/// Shared state for a background-pre-warmed OCR engine (detection-only or full
/// pipeline). Owned by the dialog via shared_ptr so a warmup task can safely
/// outlive it without joining.
struct OcrWarmState {
	std::mutex mutex;
	std::shared_ptr<ocr::OCRProcess> process;
	std::atomic<bool> ready{false};
	ocr::OCRProcessConfig config{true, false, false};
};

namespace {

wxDEFINE_EVENT(EVT_HARDSUB_SCANV_PROGRESS, ValueEvent<HardSubScanVideoProgress>);
wxDEFINE_EVENT(EVT_HARDSUB_SCANV_DONE, ValueEvent<HardSubScanVideoOutcome>);
wxDEFINE_EVENT(EVT_HARDSUB_SCANV_ROW, ValueEvent<hardsub::TimelineRowEvent>);

std::string LanguageOption() {
	std::string language = OPT_GET("Tool/OCR/Language")->GetString();
	if (language.empty())
		language = "japanese";
	return language;
}

int ReadIntOption(std::string const& name, int def) {
	try { return OPT_GET(name)->GetInt(); }
	catch (...) { return def; }
}

bool ReadBoolOption(std::string const& name, bool def) {
	try { return OPT_GET(name)->GetBool(); }
	catch (...) { return def; }
}

void SaveIntOption(std::string const& name, int value) {
	try { OPT_SET(name)->SetInt(value); } catch (...) { }
}

void SaveBoolOption(std::string const& name, bool value) {
	try { OPT_SET(name)->SetBool(value); } catch (...) { }
}

/// Removes the temporary PNG when this task returns, on any path.
struct TempPngGuard {
	wxString &path;
	explicit TempPngGuard(wxString& p) : path(p) {}
	~TempPngGuard() {
		if (!path.empty())
			agi::fs::Remove(agi::fs::path(std::wstring(path.wc_str())));
	}
};

wxImage BuildImage(hardsub::RegionImage const& crop, int scale) {
	if (!crop.Valid())
		return wxImage();
	wxImage img(crop.width, crop.height);
	std::memcpy(img.GetData(), crop.rgb.data(), crop.rgb.size());
	if (scale > 1)
		img = img.Scale(crop.width * scale, crop.height * scale, wxIMAGE_QUALITY_BICUBIC);
	return img;
}

/// Scales an image so its longest side is at most `max_side` (only downscales).
/// The whole-video scan feeds a large region (e.g. 3840x432) to OCR on every
/// sample; bounding it avoids the big-image codec/OCR edge cases and speeds the
/// scan up. The caller maps any OCR box coordinates back by the ratio of the
/// original to the returned image's size.
wxImage FitToMax(wxImage img, int max_side) {
	if (!img.IsOk())
		return img;
	int w = img.GetWidth();
	int h = img.GetHeight();
	int longest = std::max(w, h);
	if (longest > max_side) {
		double s = static_cast<double>(max_side) / longest;
		img = img.Scale(std::max(1, static_cast<int>(w * s)),
		                std::max(1, static_cast<int>(h * s)), wxIMAGE_QUALITY_BICUBIC);
	}
	return img;
}

/// Blocks until the pre-warmed OCR engine is ready (or `timeout_ms` elapses) and
/// returns it. The scan thread must NOT start its own engine: the dialog already
/// warms one at open, and starting a second concurrently (then stopping the
/// half-loaded duplicate) is what can crash inside the process/pipe runtime.
std::shared_ptr<ocr::OCRProcess> WaitForEngine(std::shared_ptr<OcrWarmState> const& state,
                                               int timeout_ms) {
	if (!state)
		return {};
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!state->ready.load()) {
		if (std::chrono::steady_clock::now() >= deadline)
			return {};
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	std::lock_guard<std::mutex> lock(state->mutex);
	return state->process;
}

/// Calls `fn` under a SEH guard. Returns 0 on success, or the SEH exception code
/// if `fn` hit a structured exception (e.g. an access violation in a third-party
/// library). Lets the scan survive a crashing OCR/encode call instead of taking
/// the whole process down.
int GuardSEH(std::function<void()> const& fn) {
	__try {
		fn();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return static_cast<int>(GetExceptionCode());
	}
}

// The OCR runtime is bundled only on Windows; the one-shot engine is the
// fallback elsewhere. Returning false means OCR is unavailable.
bool StartDetector(agi::fs::path const& image_path, ocr::OCROptions const& options,
                   std::shared_ptr<ocr::OCRProcess> const& proc,
                   std::function<bool()> const& cancel, ocr::OCRResult& out) {
	if (proc)
		out = proc->RunImage(image_path, options, true, cancel);
	else {
		ocr::OCREngine engine;
		if (engine.GetDetectionDiagnostic(options).empty())
			out = engine.DetectTextRegions(image_path, options);
		else
			return false;
	}
	return out.ok;
}

bool RunRecognizer(agi::fs::path const& image_path, ocr::OCROptions const& options,
                   std::shared_ptr<ocr::OCRProcess> const& proc,
                   std::function<bool()> const& cancel, ocr::OCRResult& out) {
	if (proc)
		out = proc->RunImage(image_path, options, false, cancel);
	else {
		ocr::OCREngine engine;
		if (engine.GetDiagnostic(options).empty())
			out = engine.RecognizeImage(image_path, options);
		else
			return false;
	}
	return out.ok;
}

double AverageConfidence(ocr::OCRResult const& result) {
	if (result.lines.empty())
		return 0.0;
	double sum = 0.0;
	for (auto const& line : result.lines)
		sum += line.confidence;
	return sum / result.lines.size();
}

std::string ToAssTime(int ms) {
	return agi::Time(ms).GetAssFormatted(true);
}

} // namespace

DialogHardSubScanVideo::DialogHardSubScanVideo(agi::Context *context)
: wxDialog(context->parent, -1, _("Hard Subtitle Scan (Whole Video)"),
           wxDefaultPosition, wxDefaultSize,
           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMINIMIZE_BOX)
, c(context)
{
	SetIcon(GETICON(button_motion_track_16));

	CreateControls();
	ocr_full_state_ = std::make_shared<OcrWarmState>();
	ocr_full_state_->config = ocr::OCRProcessConfig{true, true, true};
	StartOcrWarmup();

	// Default region: the bottom fifth of the video where dialogue subtitles
	// usually sit. Restore a previously saved region when available.
	if (auto provider = c->project->VideoProvider()) {
		int w = provider->GetWidth();
		int h = provider->GetHeight();
		region_ = wxRect(0, h * 4 / 5, w, std::max(1, h - h * 4 / 5));
	}
	int rx = ReadIntOption("Tool/HardSubScanVideo/Region X", -1);
	int ry = ReadIntOption("Tool/HardSubScanVideo/Region Y", -1);
	int rw = ReadIntOption("Tool/HardSubScanVideo/Region W", -1);
	int rh = ReadIntOption("Tool/HardSubScanVideo/Region H", -1);
	if (rx >= 0 && ry >= 0 && rw > 0 && rh > 0)
		region_ = wxRect(rx, ry, rw, rh);

	UpdateControls();
	persist = agi::make_unique<PersistLocation>(this, "Tool/HardSubScanVideo");

	Bind(wxEVT_CLOSE_WINDOW, &DialogHardSubScanVideo::OnClose, this);
	connections = agi::signal::make_vector({
		c->project->AddVideoProviderListener(&DialogHardSubScanVideo::OnVideoChanged, this)
	});
	SyncRegionSpins();
	UpdateRegionLabel();
}

DialogHardSubScanVideo::~DialogHardSubScanVideo() {
	CancelScan();
	auto stop = [](std::shared_ptr<OcrWarmState> state) {
		if (!state) return;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->process)
				state->process->Stop();
		}
		state.reset();
	};
	stop(ocr_full_state_);
}

void DialogHardSubScanVideo::UpdateRegionLabel() {
	if (region_.GetWidth() > 0 && region_.GetHeight() > 0)
		region_label->SetLabelText(wxString::Format(_("Region: %d,%d %dx%d"),
		                                            region_.GetX(), region_.GetY(),
		                                            region_.GetWidth(), region_.GetHeight()));
	else
		region_label->SetLabelText(_("No region selected"));
	Layout();
}

void DialogHardSubScanVideo::GetVideoSize(int& w, int& h) const {
	w = 0;
	h = 0;
	if (auto provider = c->project->VideoProvider()) {
		w = provider->GetWidth();
		h = provider->GetHeight();
	}
}

void DialogHardSubScanVideo::SyncRegionSpins() {
	if (updating_region_spins_)
		return;
	updating_region_spins_ = true;

	region_x_spin->SetValue(region_.GetX());
	region_y_spin->SetValue(region_.GetY());
	region_w_spin->SetValue(region_.GetWidth());
	region_h_spin->SetValue(region_.GetHeight());

	int vw = 0;
	int vh = 0;
	GetVideoSize(vw, vh);
	bool have = vw > 0 && vh > 0;
	pct_top_spin->Enable(have);
	pct_right_spin->Enable(have);
	pct_bottom_spin->Enable(have);
	pct_left_spin->Enable(have);
	if (have) {
		int left = region_.GetX();
		int top = region_.GetY();
		int right = vw - (region_.GetX() + region_.GetWidth());
		int bottom = vh - (region_.GetY() + region_.GetHeight());
		pct_left_spin->SetValue(static_cast<int>(std::lround(left * 100.0 / vw)));
		pct_top_spin->SetValue(static_cast<int>(std::lround(top * 100.0 / vh)));
		pct_right_spin->SetValue(static_cast<int>(std::lround(right * 100.0 / vw)));
		pct_bottom_spin->SetValue(static_cast<int>(std::lround(bottom * 100.0 / vh)));
	}
	else {
		pct_left_spin->SetValue(0);
		pct_top_spin->SetValue(0);
		pct_right_spin->SetValue(0);
		pct_bottom_spin->SetValue(0);
	}

	updating_region_spins_ = false;
}

void DialogHardSubScanVideo::CreateControls() {
	auto main_sizer = new wxBoxSizer(wxVERTICAL);

	auto region_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Scan Region"));
	{
		auto row = new wxBoxSizer(wxHORIZONTAL);
		select_button = new wxButton(this, -1, _("Select Region on Video"));
		row->Add(select_button, 1, wxEXPAND | wxRIGHT, 4);
		region_box->Add(row, 0, wxEXPAND | wxALL, 4);
		region_label = new wxStaticText(this, -1, _("No region selected"));
		region_box->Add(region_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		auto manual_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Region Size"));
		auto px_row = new wxBoxSizer(wxHORIZONTAL);
		px_row->Add(new wxStaticText(this, -1, _("Coordinate (px)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		auto add_px_pair = [&](wxString label, wxSpinCtrl*& spin, int max) {
			auto col = new wxBoxSizer(wxVERTICAL);
			col->Add(new wxStaticText(this, -1, label), 0, wxALIGN_LEFT);
			spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(66, -1),
			                      wxSP_ARROW_KEYS, 0, max, spin ? spin->GetValue() : 0);
			col->Add(spin, 0, wxEXPAND);
			px_row->Add(col, 0, wxRIGHT, 4);
		};
		add_px_pair(_("X"), region_x_spin, 100000);
		add_px_pair(_("Y"), region_y_spin, 100000);
		add_px_pair(_("W"), region_w_spin, 100000);
		add_px_pair(_("H"), region_h_spin, 100000);
		manual_box->Add(px_row, 0, wxEXPAND | wxALL, 4);

		auto pct_row = new wxBoxSizer(wxHORIZONTAL);
		pct_row->Add(new wxStaticText(this, -1, _("Margin (%)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		auto add_pct_pair = [&](wxString label, wxSpinCtrl*& spin) {
			auto col = new wxBoxSizer(wxVERTICAL);
			col->Add(new wxStaticText(this, -1, label), 0, wxALIGN_LEFT);
			spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(66, -1),
			                      wxSP_ARROW_KEYS, 0, 100, 0);
			col->Add(spin, 0, wxEXPAND);
			pct_row->Add(col, 0, wxRIGHT, 4);
		};
		add_pct_pair(_("Top"), pct_top_spin);
		add_pct_pair(_("Right"), pct_right_spin);
		add_pct_pair(_("Bottom"), pct_bottom_spin);
		add_pct_pair(_("Left"), pct_left_spin);
		manual_box->Add(pct_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
		region_box->Add(manual_box, 0, wxEXPAND | wxALL, 4);
	}
	main_sizer->Add(region_box, 0, wxEXPAND | wxALL, 5);

	auto opt_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Scan Options"));
	{
		auto grid = new wxFlexGridSizer(2, 5, 4);
		grid->AddGrowableCol(1);
		grid->Add(new wxStaticText(this, -1, _("Only centered subtitles")), 0, wxALIGN_CENTER_VERTICAL);
		center_only_check = new wxCheckBox(this, -1, _("Enable"));
		grid->Add(center_only_check, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, -1, _("Center band (%)")), 0, wxALIGN_CENTER_VERTICAL);
		center_band_spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, -1),
		                                 wxSP_ARROW_KEYS, 0, 100, 10);
		center_band_spin->SetToolTip(_("Rows whose horizontal center lies inside the middle this many percent of the region are kept; 100 disables the filter."));
		grid->Add(center_band_spin, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, -1, _("Min duration (frames)")), 0, wxALIGN_CENTER_VERTICAL);
		min_duration_spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, -1),
		                                   wxSP_ARROW_KEYS, 0, 2000, 12);
		grid->Add(min_duration_spin, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, -1, _("OCR every (frames)")), 0, wxALIGN_CENTER_VERTICAL);
		detect_every_spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, -1),
		                                   wxSP_ARROW_KEYS, 1, 60, 16);
		detect_every_spin->SetToolTip(_("Minimum gap between OCR samples while the region is stable. Smaller values are more accurate but slower."));
		grid->Add(detect_every_spin, 1, wxEXPAND);

		opt_box->Add(grid, 0, wxEXPAND | wxALL, 4);
		merge_check = new wxCheckBox(this, -1, _("Merge rows with identical start/end into one line"));
		merge_check->SetValue(true);
		opt_box->Add(merge_check, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
		replace_newlines_check = new wxCheckBox(this, -1, _("Replace line breaks with spaces"));
		replace_newlines_check->SetValue(true);
		replace_newlines_check->SetToolTip(_("Turn the in-text line breaks (\\N) produced for multi-line OCR rows into a single space. Leave off to keep the subtitles on multiple lines."));
		opt_box->Add(replace_newlines_check, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
	}
	main_sizer->Add(opt_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	scan_button = new wxButton(this, -1, _("Scan Whole Video"));
	main_sizer->Add(scan_button, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
	progress = new wxGauge(this, -1, 100, wxDefaultPosition, wxSize(-1, 14));
	progress->Hide();
	main_sizer->Add(progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
	status_label = new wxStaticText(this, -1, _("Ready"));
	main_sizer->Add(status_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

	result_list = new wxListCtrl(this, -1, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
	// Column widths are ~25% narrower so the dialog fits in a smaller window.
	result_list->InsertColumn(0, _("Start"), wxLIST_FORMAT_LEFT, 70);
	result_list->InsertColumn(1, _("End"), wxLIST_FORMAT_LEFT, 70);
	result_list->InsertColumn(2, _("Frames"), wxLIST_FORMAT_RIGHT, 50);
	result_list->InsertColumn(3, _("Text"), wxLIST_FORMAT_LEFT, 200);
	result_list->InsertColumn(4, _("Conf"), wxLIST_FORMAT_RIGHT, 45);
	result_list->SetMinSize(wxSize(430, 220));
	main_sizer->Add(result_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

	auto action_row = new wxBoxSizer(wxHORIZONTAL);
	copy_button = new wxButton(this, -1, _("Copy Text"));
	insert_button = new wxButton(this, -1, _("Insert Lines"));
	export_button = new wxButton(this, -1, _("Export .ass"));
	clear_button = new wxButton(this, -1, _("Clear"));
	action_row->Add(copy_button, 1, wxEXPAND | wxRIGHT, 4);
	action_row->Add(insert_button, 1, wxEXPAND | wxRIGHT, 4);
	action_row->Add(export_button, 1, wxEXPAND | wxRIGHT, 4);
	action_row->Add(clear_button, 1, wxEXPAND);
	main_sizer->Add(action_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

	auto close_row = new wxBoxSizer(wxHORIZONTAL);
	auto close_button = new wxButton(this, wxID_CANCEL, _("Close"));
	close_row->AddStretchSpacer();
	close_row->Add(close_button, 0, wxEXPAND);
	main_sizer->Add(close_row, 0, wxEXPAND | wxALL, 4);

	SetSizerAndFit(main_sizer);
	Layout();
	// Open ~25% narrower (450 px instead of the original 600) with an even
	// smaller minimum so users can still drag the window narrower by hand.
	const int fitted_height = GetSize().GetHeight();
	SetSize(450, fitted_height);
	SetMinSize(wxSize(420, 470));

	center_only_check->SetValue(ReadBoolOption("Tool/HardSubScanVideo/Center Only", true));
	center_band_spin->SetValue(ReadIntOption("Tool/HardSubScanVideo/Center Band", 10));
	min_duration_spin->SetValue(ReadIntOption("Tool/HardSubScanVideo/Min Duration Frames", 12));
	detect_every_spin->SetValue(ReadIntOption("Tool/HardSubScanVideo/Detect Every", 16));
	merge_check->SetValue(ReadBoolOption("Tool/HardSubScanVideo/Merge Same Interval", true));
	replace_newlines_check->SetValue(ReadBoolOption("Tool/HardSubScanVideo/Replace Newlines", true));

	select_button->Bind(wxEVT_BUTTON, &DialogHardSubScanVideo::OnSelect, this);
	region_x_spin->Bind(wxEVT_SPINCTRL, &DialogHardSubScanVideo::OnRegionPixelChanged, this);
	region_y_spin->Bind(wxEVT_SPINCTRL, &DialogHardSubScanVideo::OnRegionPixelChanged, this);
	region_w_spin->Bind(wxEVT_SPINCTRL, &DialogHardSubScanVideo::OnRegionPixelChanged, this);
	region_h_spin->Bind(wxEVT_SPINCTRL, &DialogHardSubScanVideo::OnRegionPixelChanged, this);
	pct_top_spin->Bind(wxEVT_SPINCTRL, &DialogHardSubScanVideo::OnRegionPercentChanged, this);
	pct_right_spin->Bind(wxEVT_SPINCTRL, &DialogHardSubScanVideo::OnRegionPercentChanged, this);
	pct_bottom_spin->Bind(wxEVT_SPINCTRL, &DialogHardSubScanVideo::OnRegionPercentChanged, this);
	pct_left_spin->Bind(wxEVT_SPINCTRL, &DialogHardSubScanVideo::OnRegionPercentChanged, this);
	scan_button->Bind(wxEVT_BUTTON, &DialogHardSubScanVideo::OnScan, this);
	copy_button->Bind(wxEVT_BUTTON, &DialogHardSubScanVideo::OnCopy, this);
	insert_button->Bind(wxEVT_BUTTON, &DialogHardSubScanVideo::OnInsert, this);
	export_button->Bind(wxEVT_BUTTON, &DialogHardSubScanVideo::OnExport, this);
	clear_button->Bind(wxEVT_BUTTON, &DialogHardSubScanVideo::OnClear, this);
	Bind(EVT_HARDSUB_SCANV_PROGRESS, &DialogHardSubScanVideo::OnScanProgress, this);
	Bind(EVT_HARDSUB_SCANV_ROW, &DialogHardSubScanVideo::OnScanRow, this);
	Bind(EVT_HARDSUB_SCANV_DONE, &DialogHardSubScanVideo::OnScanDone, this);
}

void DialogHardSubScanVideo::UpdateControls() {
	bool has_video = c->project->VideoProvider() != nullptr;
	bool has_region = region_.GetWidth() > 0 && region_.GetHeight() > 0;
	select_button->Enable(has_video && !scanning_);
	scan_button->Enable(has_video && has_region && !scanning_);
	copy_button->Enable(!scanning_ && !events_.empty());
	insert_button->Enable(!scanning_ && !events_.empty());
	export_button->Enable(!scanning_ && !events_.empty());
	clear_button->Enable(!scanning_ && !events_.empty());
}

void DialogHardSubScanVideo::SetStatus(wxString const& text) {
	status_label->SetLabelText(text);
}

void DialogHardSubScanVideo::PopulateResults() {
	result_list->DeleteAllItems();
	for (size_t i = 0; i < events_.size(); ++i) {
		auto const& ev = events_[i];
		wxString frame_label = wxString::Format(_("%d"), ev.start_frame);
		result_list->InsertItem(static_cast<long>(i), frame_label);
		result_list->SetItem(static_cast<long>(i), 1, wxString::Format(_("%d"), ev.end_frame));
		result_list->SetItem(static_cast<long>(i), 2, wxString::Format(_("%d"), ev.end_frame - ev.start_frame + 1));
		wxString text = to_wx(ev.text);
		if (text.length() > 80) {
			text = text.Left(77) + wxString("...");
			result_list->SetItem(static_cast<long>(i), 3, text);
		}
		else
			result_list->SetItem(static_cast<long>(i), 3, text);
		result_list->SetItem(static_cast<long>(i), 4, wxString::Format(_("%d%%"),
			static_cast<int>(ev.confidence * 100)));
	}
	UpdateControls();
}

void DialogHardSubScanVideo::AppendResultRow(hardsub::TimelineRowEvent const& ev) {
	long i = result_list->GetItemCount();
	result_list->InsertItem(i, wxString::Format(_("%d"), ev.start_frame));
	result_list->SetItem(i, 1, wxString::Format(_("%d"), ev.end_frame));
	result_list->SetItem(i, 2, wxString::Format(_("%d"), ev.end_frame - ev.start_frame + 1));
	wxString text = to_wx(ev.text);
	if (text.length() > 80)
		text = text.Left(77) + wxString("...");
	result_list->SetItem(i, 3, text);
	result_list->SetItem(i, 4, wxString::Format(_("%d%%"),
		static_cast<int>(ev.confidence * 100)));
	result_list->EnsureVisible(i);
}

std::vector<size_t> DialogHardSubScanVideo::ResultIndices() const {
	std::vector<size_t> indices;
	for (long item = -1;
	     (item = result_list->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1;)
		indices.push_back(static_cast<size_t>(item));
	if (indices.empty()) {
		for (size_t i = 0; i < events_.size(); ++i)
			indices.push_back(i);
	}
	return indices;
}

void DialogHardSubScanVideo::ActivateRegionTool() {
	if (!c->project->VideoProvider() || !c->videoDisplay)
		return;
	auto tool = agi::make_unique<HardSubRegionTool>(c->videoDisplay, c,
		[this](wxRect const& video_region, int) { OnRegionSelected(video_region); },
		region_);
	c->videoDisplay->SetTool(std::move(tool));
	SetStatus(_("Drag a rectangle over the subtitle area on the video."));
}

void DialogHardSubScanVideo::OnRegionSelected(wxRect const& video_region) {
	region_ = video_region;
	UpdateRegionLabel();
	SyncRegionSpins();
	UpdateControls();
	SetStatus(_("Region set."));
}

void DialogHardSubScanVideo::OnSelect(wxCommandEvent&) {
	ActivateRegionTool();
}

void DialogHardSubScanVideo::OnRegionPixelChanged(wxSpinEvent&) {
	if (updating_region_spins_)
		return;
	int x = region_x_spin->GetValue();
	int y = region_y_spin->GetValue();
	int w = std::max(1, region_w_spin->GetValue());
	int h = std::max(1, region_h_spin->GetValue());
	region_ = wxRect(x, y, w, h);
	UpdateRegionLabel();
	SyncRegionSpins();
	UpdateControls();
}

void DialogHardSubScanVideo::OnRegionPercentChanged(wxSpinEvent&) {
	if (updating_region_spins_)
		return;
	int vw = 0;
	int vh = 0;
	GetVideoSize(vw, vh);
	if (vw <= 0 || vh <= 0)
		return;

	double left_p = pct_left_spin->GetValue() / 100.0;
	double top_p = pct_top_spin->GetValue() / 100.0;
	double right_p = pct_right_spin->GetValue() / 100.0;
	double bottom_p = pct_bottom_spin->GetValue() / 100.0;

	int x = std::max(0, static_cast<int>(std::lround(left_p * vw)));
	int y = std::max(0, static_cast<int>(std::lround(top_p * vh)));
	int w = std::max(1, static_cast<int>(std::lround((1.0 - left_p - right_p) * vw)));
	int h = std::max(1, static_cast<int>(std::lround((1.0 - top_p - bottom_p) * vh)));
	x = std::min(x, vw - 1);
	y = std::min(y, vh - 1);
	w = std::min(w, vw - x);
	h = std::min(h, vh - y);

	region_ = wxRect(x, y, w, h);
	UpdateRegionLabel();
	SyncRegionSpins();
	UpdateControls();
}

void DialogHardSubScanVideo::OnScan(wxCommandEvent&) {
	if (scanning_)
		return;
	if (region_.GetWidth() <= 0 || region_.GetHeight() <= 0) {
		wxMessageBox(_("Select a scan region on the video first."),
		             _("Hard Subtitle Scan"), wxOK | wxICON_INFORMATION | wxCENTER, this);
		ActivateRegionTool();
		return;
	}
	if (!c->project->VideoProvider())
		return;

	SaveIntOption("Tool/HardSubScanVideo/Region X", region_.GetX());
	SaveIntOption("Tool/HardSubScanVideo/Region Y", region_.GetY());
	SaveIntOption("Tool/HardSubScanVideo/Region W", region_.GetWidth());
	SaveIntOption("Tool/HardSubScanVideo/Region H", region_.GetHeight());
	SaveBoolOption("Tool/HardSubScanVideo/Center Only", center_only_check->GetValue());
	SaveIntOption("Tool/HardSubScanVideo/Center Band", center_band_spin->GetValue());
	SaveIntOption("Tool/HardSubScanVideo/Min Duration Frames", min_duration_spin->GetValue());
	SaveIntOption("Tool/HardSubScanVideo/Detect Every", detect_every_spin->GetValue());
	SaveBoolOption("Tool/HardSubScanVideo/Merge Same Interval", merge_check->GetValue());
	SaveBoolOption("Tool/HardSubScanVideo/Replace Newlines", replace_newlines_check->GetValue());

	hardsub::TimelineScanOptions options;
	options.region_x = region_.GetX();
	options.region_y = region_.GetY();
	options.region_w = region_.GetWidth();
	options.region_h = region_.GetHeight();
	auto provider = c->project->VideoProvider();
	options.frame_count = provider->GetFrameCount();
	options.frame_start = 0;
	options.frame_end = -1;
	options.center_only = center_only_check->GetValue();
	options.center_band = center_band_spin->GetValue();
	options.min_duration_frames = min_duration_spin->GetValue();
	options.detect_every = detect_every_spin->GetValue();
	options.merge_same_interval = merge_check->GetValue();
	options.replace_newlines = replace_newlines_check->GetValue();

	agi::fs::path video_filename;
	if (provider)
		video_filename = provider->GetFilename();

	cancel_scan_ = false;
	scanning_ = true;
	events_.clear();
	PopulateResults();
	progress->SetRange(1000);
	progress->SetValue(0);
	progress->Show();
	Layout();
	SetStatus(_("Scanning..."));
	UpdateControls();

	resume_playback_ = c->videoController->IsPlaying();
	if (resume_playback_)
		c->videoController->Stop();

	auto handler = this;
	scan_thread_ = std::thread([handler, options, video_filename]() mutable {
		auto cancel = [handler] { return handler->cancel_scan_.load(); };
		std::string decoder_error;
		auto decoder = std::make_shared<hardsub::ScanVideoDecoder>(video_filename, decoder_error);
		bool use_decoder = !video_filename.empty() && decoder->Valid();
		auto loader = use_decoder
			? hardsub::FrameLoader([decoder](int f) { return decoder->GetFrame(f); })
			: hardsub::FrameLoader([handler](int f) {
				return handler->c->videoController->GetFrame(f, true);
			});

		ocr::OCROptions ocr_options;
		ocr_options.keep_line_breaks = true;
		ocr_options.language = LanguageOption();
		std::shared_ptr<ocr::OCRProcess> rec_proc;
		std::function<ocr::OCRResult(hardsub::RegionImage const&)> detect;
		std::function<ocr::OCRResult(hardsub::RegionImage const&)> recognize;
		// Use a single full-pipeline engine for both detection (client-side
		// read-only boxes) and recognition, so the OCR response shape is always
		// the well-tested full-pipeline format.
		rec_proc = WaitForEngine(handler->ocr_full_state_, 10000);
		detect = [handler, rec_proc, ocr_options, cancel](hardsub::RegionImage const& crop) -> ocr::OCRResult {
			ocr::OCRResult out;
			int code = GuardSEH([&] {
				handler->scan_ocr_samples_.fetch_add(1);
				wxString temp;
				TempPngGuard guard(temp);
				wxImage img = FitToMax(BuildImage(crop, 1), 1600);
				if (!img.IsOk())
					return;
				temp = wxFileName::CreateTempFileName("aegisub-hardsub-");
				if (temp.empty() || !img.SaveFile(temp, wxBITMAP_TYPE_PNG))
					return;
				ocr::OCRResult tmp;
				if (!StartDetector(agi::fs::path(std::wstring(temp.wc_str())), ocr_options,
				                   rec_proc, cancel, tmp))
					return;
				double sx = static_cast<double>(crop.width) / img.GetWidth();
				double sy = static_cast<double>(crop.height) / img.GetHeight();
				for (auto& line : tmp.lines)
					for (auto& pt : line.box) {
						pt.first = static_cast<int>(pt.first * sx);
						pt.second = static_cast<int>(pt.second * sy);
					}
				out = std::move(tmp);
			});
			if (code)
				return ocr::OCRResult();
			return out;
		};
		recognize = [handler, rec_proc, ocr_options, cancel](hardsub::RegionImage const& crop) -> ocr::OCRResult {
			ocr::OCRResult out;
			int code = GuardSEH([&] {
				wxString temp;
				TempPngGuard guard(temp);
				wxImage img = FitToMax(BuildImage(crop, 2), 1600);
				if (!img.IsOk())
					return;
				temp = wxFileName::CreateTempFileName("aegisub-hardsub-");
				if (temp.empty() || !img.SaveFile(temp, wxBITMAP_TYPE_PNG))
					return;
				ocr::OCRResult tmp;
				if (!RunRecognizer(agi::fs::path(std::wstring(temp.wc_str())), ocr_options,
				                   rec_proc, cancel, tmp))
					return;
				out = std::move(tmp);
			});
			if (code)
				return ocr::OCRResult();
			return out;
		};

		auto last_post = std::chrono::steady_clock::now();
		auto clock_start = last_post;
		auto progress = [handler, &last_post, &clock_start](int done, int total) {
			auto now = std::chrono::steady_clock::now();
			if (now - last_post < std::chrono::milliseconds(250))
				return;
			last_post = now;
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - clock_start).count();
			handler->AddPendingEvent(ValueEvent<HardSubScanVideoProgress>(
				EVT_HARDSUB_SCANV_PROGRESS, -1,
				HardSubScanVideoProgress{done, total,
					handler->scan_ocr_samples_.load(), elapsed}));
		};
		auto row_cb = [handler](hardsub::TimelineRowEvent const& ev) {
			handler->AddPendingEvent(ValueEvent<hardsub::TimelineRowEvent>(
				EVT_HARDSUB_SCANV_ROW, -1, ev));
		};

		HardSubScanVideoOutcome outcome;
		try {
			hardsub::TimelineScanResult result;
			int scan_code = GuardSEH([&] {
				result = hardsub::ScanVideoTimeline(options, loader, detect, recognize,
					cancel, progress, row_cb);
			});
			if (scan_code) {
				outcome = HardSubScanVideoOutcome();
				outcome.error = from_wx(agi::wxformat(
					_("The scan was interrupted by an internal error (0x%08X)."),
					scan_code));
			}
			else if (cancel()) {
				outcome.cancelled = true;
			}
			else {
				outcome.ok = true;
				outcome.events = std::move(result.events);
			}
		}
		catch (std::exception const& e) {
			outcome = HardSubScanVideoOutcome();
			outcome.error = from_wx(agi::wxformat(_("Hard subtitle scan failed: %s"), e.what()));
		}
		catch (...) {
			outcome = HardSubScanVideoOutcome();
			outcome.error = from_wx(_("Hard subtitle scan failed with an unknown error."));
		}
		handler->AddPendingEvent(ValueEvent<HardSubScanVideoOutcome>(
			EVT_HARDSUB_SCANV_DONE, -1, std::move(outcome)));
	});
}

void DialogHardSubScanVideo::OnScanProgress(ValueEvent<HardSubScanVideoProgress>& event) {
	auto const& p = event.Get();
	if (p.total <= 0)
		return;
	int percent = std::min(1000, p.done * 1000 / p.total);
	progress->SetValue(percent);

	long long elapsed = p.elapsed_ms;
	long long eta = 0;
	if (p.done > 0)
		eta = elapsed * static_cast<long long>(p.total - p.done) / p.done;
	auto fmt = [](long long ms) {
		long long s = ms / 1000;
		return wxString::Format(_("%lld:%02lld:%02lld"), s / 3600, (s % 3600) / 60, s % 60);
	};
	// Do NOT pass wxString values to wxString::Format's %s: wxString does not
	// convert to a character pointer in a C varargs call, so %s would treat the
	// wxString object's bytes as a pointer and dereference garbage (crash).
	// Concatenate the string pieces instead.
	wxString status = wxString::Format(_("Frame %d / %d (%d%%)  |  elapsed "),
		p.done, p.total, percent / 10);
	status += fmt(elapsed);
	status += wxString(_("  |  ETA "));
	status += fmt(eta);
	status += wxString::Format(_("  |  OCR %d"), p.ocr_samples);
	SetStatus(status);
}

void DialogHardSubScanVideo::OnScanRow(ValueEvent<hardsub::TimelineRowEvent>& event) {
	if (!scanning_)
		return;
	auto ev = event.Get();
	events_.push_back(ev);
	AppendResultRow(ev);
	UpdateControls();
}

void DialogHardSubScanVideo::OnScanDone(ValueEvent<HardSubScanVideoOutcome>& event) {
	auto outcome = event.Get();
	scanning_ = false;
	progress->Hide();
	if (resume_playback_) {
		resume_playback_ = false;
		c->videoController->Play();
	}

	if (outcome.cancelled) {
		SetStatus(_("Scan cancelled."));
	}
	else if (outcome.ok) {
		events_ = std::move(outcome.events);
		PopulateResults();
		SetStatus(wxString::Format(_("Scan complete: %d lines detected."),
			static_cast<int>(events_.size())));
	}
	else {
		SetStatus(to_wx(outcome.error));
	}
	Layout();
	UpdateControls();
}

void DialogHardSubScanVideo::OnCopy(wxCommandEvent&) {
	wxString text;
	for (long item = -1; (item = result_list->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1;) {
		auto const& ev = events_[static_cast<size_t>(item)];
		if (!text.IsEmpty())
			text += wxString("\n");
		text += to_wx(ev.text);
	}
	if (text.IsEmpty())
		return;
	SetClipboard(text);
	SetStatus(_("Copied to clipboard."));
}

void DialogHardSubScanVideo::OnInsert(wxCommandEvent&) {
	if (events_.empty())
		return;

	std::vector<size_t> indices = ResultIndices();

	std::string style = c->ass->Styles.empty() ? std::string("Default") : c->ass->Styles.front().name;
	std::vector<AssDialogue*> new_lines;
	for (size_t idx : indices) {
		auto const& ev = events_[idx];
		std::string text = ev.text;
		boost::replace_all(text, "\r\n", "\\N");
		boost::replace_all(text, "\r", "\\N");
		boost::replace_all(text, "\n", "\\N");
		auto line = new AssDialogue();
		line->Start = c->videoController->TimeAtFrame(ev.start_frame, agi::vfr::START);
		line->End = c->videoController->TimeAtFrame(ev.end_frame, agi::vfr::END);
		line->Text = text;
		line->Style = style;
		new_lines.push_back(line);
	}

	std::sort(new_lines.begin(), new_lines.end(),
		[](AssDialogue* a, AssDialogue* b) { return a->Start < b->Start; });
	auto& events = c->ass->Events;
	for (AssDialogue* line : new_lines) {
		auto it = std::find_if(events.begin(), events.end(),
			[&](AssDialogue const& d) { return d.Start > line->Start; });
		events.insert(it, *line);
	}
	c->ass->Commit(_("insert hard subtitle lines"), AssFile::COMMIT_DIAG_ADDREM);
	if (!new_lines.empty())
		c->selectionController->SetSelectionAndActive(Selection{new_lines.front()}, new_lines.front());
	SetStatus(wxString::Format(_("Inserted %d lines."), static_cast<int>(new_lines.size())));
}

void DialogHardSubScanVideo::OnExport(wxCommandEvent&) {
	if (events_.empty())
		return;

	agi::fs::path video_name = c->project->VideoName();
	wxString default_name = video_name.empty()
		? wxString("hardsub_scan")
		: to_wx(video_name.stem().string());
	wxFileDialog dlg(this, _("Export subtitle (.ass)"), wxEmptyString, default_name + ".ass",
	                 _("ASS subtitles (*.ass)|*.ass"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dlg.ShowModal() != wxID_OK)
		return;
	wxString path = dlg.GetPath();
	if (!path.Lower().EndsWith(".ass"))
		path += ".ass";

	int res_x = 0, res_y = 0;
	c->ass->GetResolution(res_x, res_y);
	if (auto provider = c->project->VideoProvider()) {
		if (res_x <= 0) res_x = provider->GetWidth();
		if (res_y <= 0) res_y = provider->GetHeight();
	}

	std::string data;
	data.reserve(events_.size() * 128);
	data += "[Script Info]\n";
	bool has_res_x = false, has_res_y = false;
	for (auto const& info : c->ass->Info) {
		std::string key = info.Key();
		if (key == "PlayResX") has_res_x = true;
		if (key == "PlayResY") has_res_y = true;
		data += info.GetEntryData() + "\n";
	}
	if (!has_res_x) data += "PlayResX: " + std::to_string(res_x) + "\n";
	if (!has_res_y) data += "PlayResY: " + std::to_string(res_y) + "\n";
	data += "\n[V4+ Styles]\n";
	data += "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";
	for (auto const& style : c->ass->Styles)
		data += style.GetEntryData() + "\n";
	data += "\n[Events]\n";
	data += "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
	std::string style_name = c->ass->Styles.empty() ? std::string("Default") : c->ass->Styles.front().name;
	for (size_t idx : ResultIndices()) {
		auto const& ev = events_[idx];
		std::string text = ev.text;
		boost::replace_all(text, "\r\n", "\\N");
		boost::replace_all(text, "\r", "\\N");
		boost::replace_all(text, "\n", "\\N");
		int start_ms = c->videoController->TimeAtFrame(ev.start_frame, agi::vfr::START);
		int end_ms = c->videoController->TimeAtFrame(ev.end_frame, agi::vfr::END);
		data += agi::format("Dialogue: 0,%s,%s,%s,,0,0,0,,%s\n",
		                    ToAssTime(start_ms), ToAssTime(end_ms), style_name, text);
	}

	wxFile file;
	if (!file.Open(path, wxFile::write)) {
		wxMessageBox(_("Could not write the export file."),
		             _("Hard Subtitle Scan"), wxOK | wxICON_ERROR | wxCENTER, this);
		return;
	}
	file.Write(data.data(), static_cast<wxFileOffset>(data.size()));
	file.Close();
	SetStatus(_("Exported .ass file."));
}

void DialogHardSubScanVideo::OnClear(wxCommandEvent&) {
	ClearResults();
	SetStatus(_("Result list cleared."));
}

void DialogHardSubScanVideo::ClearResults() {
	events_.clear();
	result_list->DeleteAllItems();
	UpdateControls();
}

void DialogHardSubScanVideo::CancelScan() {
	cancel_scan_ = true;
	if (scan_thread_.joinable())
		scan_thread_.join();
	scanning_ = false;
	if (resume_playback_) {
		resume_playback_ = false;
		c->videoController->Play();
	}
}

void DialogHardSubScanVideo::OnClose(wxCloseEvent& event) {
	CancelScan();
	if (c->videoDisplay && c->videoDisplay->ToolIsType(typeid(HardSubRegionTool)))
		cmd::call("video/tool/cross", c);
	event.Skip();
}

void DialogHardSubScanVideo::OnVideoChanged(AsyncVideoProvider *provider) {
	if (!provider) {
		CancelScan();
	}
	else if (region_.GetWidth() <= 0) {
		int w = provider->GetWidth();
		int h = provider->GetHeight();
		if (w > 0 && h > 0) {
			region_ = wxRect(0, h * 4 / 5, w, std::max(1, h - h * 4 / 5));
			UpdateRegionLabel();
		}
	}
	SyncRegionSpins();
	UpdateControls();
}

void DialogHardSubScanVideo::StartOcrWarmup() {
	// Warm both engines in the background while the user picks a region, so that
	// the moment they click Scan the models are already loaded (a synchronous
	// load at click time would otherwise block the scan for several seconds and
	// leave the window looking frozen).
	auto start_one = [](std::shared_ptr<OcrWarmState> state) {
		if (!state)
			return;
		agi::dispatch::Background().Async([state] {
			try {
				auto process = std::make_shared<ocr::OCRProcess>();
				agi::fs::path executable, models_dir, config_path;
				std::string diagnostic;
				if (ocr::OCRProcess::FindRuntime(executable, models_dir, config_path, diagnostic)
				    && process->Start(executable, models_dir, config_path, diagnostic,
				                      state->config)) {
					std::lock_guard<std::mutex> lock(state->mutex);
					if (!state->process || !state->process->IsRunning())
						state->process = std::move(process);
					else
						process->Stop();
					state->ready = true;
				}
			}
			catch (...) {
				// OCR startup failure must never crash the app.
			}
		});
	};
	start_one(ocr_full_state_); // full pipeline: recognition
}
