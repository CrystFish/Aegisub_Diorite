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

#include "dialog_hardsub_scan.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "async_video_provider.h"
#include "command/command.h"
#include "compat.h"
#include "format.h"
#include "hardsub_region_tool.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "localization/localization_matcher.h"
#include "ocr/ocr_engine.h"
#include "ocr/ocr_process.h"
#include "options.h"
#include "persist_location.h"
#include "project.h"
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
#include <cctype>
#include <cstring>
#include <map>
#include <thread>
#include <typeinfo>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/image.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {

wxDEFINE_EVENT(EVT_HARDSUB_RECOGNIZE_DONE, ValueEvent<HardSubRecognizeOutcome>);
wxDEFINE_EVENT(EVT_HARDSUB_SCAN_PROGRESS, ValueEvent<HardSubScanProgress>);
wxDEFINE_EVENT(EVT_HARDSUB_SCAN_DONE, ValueEvent<HardSubScanOutcome>);

/// Detects text boxes in a PNG crop; returns the raw OCR result.
using FrameDetector = std::function<ocr::OCRResult(agi::fs::path const&)>;

std::string LanguageOption() {
	std::string language = OPT_GET("Tool/OCR/Language")->GetString();
	if (language.empty())
		language = "japanese";
	return language;
}

/// Number of non-whitespace bytes in normalized OCR text. Byte length is a
/// consistent proxy for content amount when comparing candidates produced by
/// the same model.
int NonSpaceLength(std::string const& text) {
	int count = 0;
	for (unsigned char c : text) {
		if (!std::isspace(c))
			++count;
	}
	return count;
}

double ConfidenceSum(ocr::OCRResult const& result) {
	double total = 0.0;
	for (auto const& line : result.lines)
		total += line.confidence;
	return total;
}

/// Match options used to compare OCR text with the reference subtitle text:
/// fuzzy, tags/punctuation/case-insensitive, whitespace collapsed.
localization::MatchOptions StrictMatchOptions() {
	localization::MatchOptions options;
	options.fuzzy = true;
	options.ignore_tags = true;
	options.ignore_punctuation = true;
	options.ignore_case = true;
	return options;
}

/// Similarity (0..1) between the reference subtitle text and an OCR result.
double TextSimilarity(std::string const& reference, std::string const& candidate) {
	auto ref = localization::Normalize(reference, StrictMatchOptions());
	auto cand = localization::Normalize(candidate, StrictMatchOptions());
	return localization::Similarity(ref, cand);
}

/// Save the selected region of `frame` as a PNG and return its path. `margin`
/// enlarges the crop on every side and `upscale` doubles it, which helps the
/// OCR engine read small subtitles; the boundary detector uses neither. The
/// temp file path is reused across calls when `temp` is non-empty. Returns an
/// empty path on failure.
agi::fs::path SaveRegionPng(hardsub::FrameLoader const& loader,
                            hardsub::RegionImage const& tpl, int region_x, int region_y,
                            int frame, int margin, bool upscale, wxString& temp) {
	auto frame_data = loader(frame);
	if (!frame_data)
		return {};
	auto crop = hardsub::CropRegion(*frame_data, region_x - margin, region_y - margin,
	                                tpl.width + margin * 2, tpl.height + margin * 2);
	if (!crop.Valid())
		return {};

	wxImage img(crop.width, crop.height);
	if (!img.GetData())
		return {};
	std::memcpy(img.GetData(), crop.rgb.data(), crop.rgb.size());
	if (upscale)
		img = img.Scale(crop.width * 2, crop.height * 2, wxIMAGE_QUALITY_BICUBIC);

	if (temp.empty())
		temp = wxFileName::CreateTempFileName("aegisub-hardsub-");
	if (temp.empty() || !img.SaveFile(temp, wxBITMAP_TYPE_PNG))
		return {};
	return agi::fs::path(std::wstring(temp.wc_str()));
}

/// Refine a diff-detected boundary with the OCR detector. For the start
/// boundary this looks for the earliest frame with detected text in the window;
/// for the end boundary the latest one.
int AdjustBoundaryWithOcr(FrameDetector const& detect, hardsub::FrameLoader const& loader,
                          hardsub::RegionImage const& tpl, int region_x, int region_y,
                          int boundary, int window, bool find_earliest,
                          hardsub::CancelFn const& cancel) {
	int best = boundary;
	auto check_frame = [&](int frame) {
		if (cancel && cancel())
			return false;
		wxString temp;
		auto image_path = SaveRegionPng(loader, tpl, region_x, region_y, frame, 0, false, temp);
		if (image_path.empty())
			return false;
		auto result = detect(image_path);
		agi::fs::Remove(image_path);
		return result.ok && !result.lines.empty();
	};

	if (find_earliest) {
		for (int f = boundary - window; f <= boundary + window; ++f) {
			if (cancel && cancel())
				break;
			if (check_frame(f)) {
				best = f;
				break;
			}
		}
	}
	else {
		for (int f = boundary + window; f >= boundary - window; --f) {
			if (cancel && cancel())
				break;
			if (check_frame(f)) {
				best = f;
				break;
			}
		}
	}
	return best;
}

/// Content-validate the pixel-scan boundaries: sample the candidate run with
/// full OCR and compare each frame's recognized text against `reference_text`.
/// The run ends at the last frame whose text still matches the reference and
/// starts at the first one, so a neighboring subtitle can no longer pull the
/// boundary into itself. Returns true when OCR evidence verified the result;
/// returns false (leaving the result untouched) when OCR is unavailable, the
/// reference text is empty or nothing could be verified, so callers can fall
/// back to the cheaper detection-only confirmation.
bool RefineTimelineWithOcr(FrameDetector const& recognize,
                           hardsub::FrameLoader const& loader,
                           hardsub::RegionImage const& tpl, int region_x, int region_y,
                           int base_frame, int frame_count,
                           hardsub::ScanResult& result,
                           std::string const& reference_text,
                           hardsub::CancelFn const& cancel,
                           hardsub::ProgressFn const& progress) {
	if (!result.ok || reference_text.empty())
		return false;
	if (localization::Normalize(reference_text, StrictMatchOptions()).empty())
		return false;

	constexpr double kMatchThreshold = 0.85;
	constexpr int kSampleStep = 16;
	constexpr int kMaxExtraEndFrames = 8; // how far past the pixel end a text match is still trusted
	constexpr int kMaxStartProbe = 6;     // how far before the pixel start earlier matches are looked for
	constexpr int kMaxSamples = 128;      // cap OCR calls on pathological long runs

	int old_start = result.start_frame;
	int old_end = result.end_frame;
	int new_start = old_start;
	int new_end = old_end;

	wxString temp;
	// Remove the shared temp PNG when this function returns, on any path.
	struct TempPngGuard {
		wxString &path;
		explicit TempPngGuard(wxString &p) : path(p) {}
		~TempPngGuard() {
			if (!path.empty())
				agi::fs::Remove(agi::fs::path(std::wstring(path.wc_str())));
		}
	} temp_guard(temp);
	std::map<int, double> sim_cache;
	auto frame_sim = [&](int frame) -> double {
		if (cancel && cancel())
			return -1.0;
		auto it = sim_cache.find(frame);
		if (it != sim_cache.end())
			return it->second;
		auto path = SaveRegionPng(loader, tpl, region_x, region_y, frame, 6, true, temp);
		if (path.empty())
			return -1.0;
		auto res = recognize(path);
		agi::fs::Remove(path);
		double sim = 0.0;
		if (res.ok) {
			if (!res.text.empty())
				sim = TextSimilarity(reference_text, res.text);
			if (progress)
				progress(1, 1);
		}
		sim_cache.emplace(frame, sim);
		return sim;
	};
	auto is_match = [&](double sim) { return sim >= kMatchThreshold; };

	// ---- Forward pass: where does the text stop matching? ------------------
	// Sweep the candidate run; the last matching sample anchors the end, then
	// the transition is refined frame by frame. If the whole run matches, a
	// text match is trusted a little beyond the pixel end (the pixel signal can
	// drop early, e.g. when a foreground object briefly covers the subtitle).
	int sample_step = kSampleStep;
	int run_len = old_end - old_start + 1;
	if (run_len / kSampleStep > kMaxSamples)
		sample_step = std::max(1, run_len / kMaxSamples);

	int last_good = -1;
	int first_bad = -1;
	int first_good = -1;
	for (int f = old_start; f <= old_end; f += sample_step) {
		double sim = frame_sim(f);
		if (sim < 0.0)
			return false;
		if (is_match(sim)) {
			last_good = f;
			first_bad = -1;
			if (first_good < 0)
				first_good = f;
		}
		else if (first_bad < 0) {
			first_bad = f;
		}
	}
	if (last_good < 0)
		return false; // OCR never matched the reference; keep the pixel result

	new_end = last_good;
	if (first_bad > last_good) {
		for (int f = last_good + 1; f < first_bad; ++f) {
			double sim = frame_sim(f);
			if (sim < 0.0)
				return false;
			if (is_match(sim))
				new_end = f;
		}
	}
	else {
		int misses = 0;
		int probe_end = std::min(frame_count - 1, old_end + kMaxExtraEndFrames);
		for (int f = old_end + 1; f <= probe_end; ++f) {
			double sim = frame_sim(f);
			if (sim < 0.0)
				return false;
			if (is_match(sim)) {
				new_end = f;
				misses = 0;
			}
			else if (++misses >= 2) {
				break;
			}
		}
	}

	// ---- Backward pass: where does the text start matching? ----------------
	bool first_sample_matches = is_match(frame_sim(old_start));
	if (first_sample_matches) {
		// Extend the start while the frames just before it still match, so a
		// subtitle that appeared earlier than the pixel scan reported is not
		// cut short. Stop at the first mismatch so the previous subtitle line
		// cannot pull the start backward into itself.
		for (int f = old_start - 1; f >= std::max(0, old_start - kMaxStartProbe); --f) {
			double sim = frame_sim(f);
			if (sim < 0.0)
				return false;
			if (is_match(sim))
				new_start = f;
			else
				break;
		}
	}
	else if (first_good > old_start + sample_step) {
		// The pixel start lands before the text actually begins (at least two
		// consecutive sweep samples did not match); pull the start forward to
		// the first matching frame. Requiring two misses avoids moving the
		// start for a single-frame OCR misread.
		for (int f = old_start + 1; f <= first_good; ++f) {
			double sim = frame_sim(f);
			if (sim < 0.0)
				return false;
			if (is_match(sim)) {
				new_start = f;
				break;
			}
		}
	}

	// Only trust the refinement when the base frame stays inside the run and
	// the range stays sane; otherwise keep the pixel-scan result.
	if (new_start > new_end || base_frame < new_start || base_frame > new_end)
		return false;

	result.start_frame = new_start;
	result.end_frame = new_end;
	return true;
}

} // namespace

DialogHardSubScan::DialogHardSubScan(agi::Context *context)
: wxDialog(context->parent, -1, _("Hard Subtitle Scan"), wxDefaultPosition, wxDefaultSize,
           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMINIMIZE_BOX)
, c(context)
{
	SetIcon(GETICON(button_motion_track_16));

	CreateControls();
	UpdateControls();

	persist = agi::make_unique<PersistLocation>(this, "Tool/HardSub");

	Bind(wxEVT_CLOSE_WINDOW, &DialogHardSubScan::OnClose, this);
	connections = agi::signal::make_vector({
		c->project->AddVideoProviderListener(&DialogHardSubScan::OnVideoChanged, this)
	});
}

DialogHardSubScan::~DialogHardSubScan() {
	recognize_alive_ = false;
	CancelScan();
}

void DialogHardSubScan::CreateControls() {
	auto main_sizer = new wxBoxSizer(wxVERTICAL);

	auto box = new wxStaticBoxSizer(wxVERTICAL, this, _("Region and Text"));
	{
		auto row = new wxBoxSizer(wxHORIZONTAL);
		select_button = new wxButton(this, -1, _("Select Region on Video"));
		clear_button = new wxButton(this, -1, _("Clear"));
		row->Add(select_button, 1, wxEXPAND | wxRIGHT, 4);
		row->Add(clear_button, 0, wxEXPAND);
		box->Add(row, 0, wxEXPAND | wxALL, 4);

		region_label = new wxStaticText(this, -1, _("No region selected"));
		box->Add(region_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		recognize_button = new wxButton(this, -1, _("Recognize Text"));
		box->Add(recognize_button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		text_ctrl = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, 96),
		                           wxTE_MULTILINE | wxTE_RICH2);
		text_ctrl->SetMinSize(wxSize(-1, 96));
		box->Add(text_ctrl, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
	}
	main_sizer->Add(box, 1, wxEXPAND | wxALL, 5);

	auto options_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Scan Options"));
	{
		auto grid = new wxFlexGridSizer(2, 5, 4);
		grid->AddGrowableCol(1);

		grid->Add(new wxStaticText(this, -1, _("Max range (frames)")), 0, wxALIGN_CENTER_VERTICAL);
		max_range_spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, -1),
		                                wxSP_ARROW_KEYS, 0, 100000, 0);
		max_range_spin->SetToolTip(_("Maximum frames to search in each direction; 0 = search to the video edges automatically."));
		grid->Add(max_range_spin, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, -1, _("Scan step (frames)")), 0, wxALIGN_CENTER_VERTICAL);
		stride_spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, -1),
		                             wxSP_ARROW_KEYS, 1, 100, 32);
		stride_spin->SetToolTip(_("Frame step of the scan. 1 checks every frame (most accurate); larger values are faster but can miss gaps shorter than the step."));
		grid->Add(stride_spin, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, -1, _("Text match (%)")), 0, wxALIGN_CENTER_VERTICAL);
		threshold_spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, -1),
		                                wxSP_ARROW_KEYS, 1, 100, 40);
		grid->Add(threshold_spin, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, -1, _("Confirm frames")), 0, wxALIGN_CENTER_VERTICAL);
		confirm_spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, -1),
		                              wxSP_ARROW_KEYS, 0, 20, 2);
		grid->Add(confirm_spin, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, -1, _("Min duration (frames)")), 0, wxALIGN_CENTER_VERTICAL);
		min_duration_spin = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxSize(-1, -1),
		                                   wxSP_ARROW_KEYS, 0, 5000, 0);
		grid->Add(min_duration_spin, 1, wxEXPAND);

		options_box->Add(grid, 0, wxEXPAND | wxALL, 4);

		ocr_confirm_check = new wxCheckBox(this, -1, _("Confirm boundaries with OCR"));
		ocr_confirm_check->SetValue(true);
		options_box->Add(ocr_confirm_check, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);

		strict_ocr_check = new wxCheckBox(this, -1, _("Strict timeline (OCR text check)"));
		strict_ocr_check->SetValue(true);
		strict_ocr_check->SetToolTip(_(
			"Runs full OCR over the candidate range and compares each frame's recognized text "
			"with the reference text. The boundaries stop as soon as a different subtitle "
			"appears, so consecutive subtitles are not merged into one long line. Slower, "
			"but more accurate."));
		options_box->Add(strict_ocr_check, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
	}
	main_sizer->Add(options_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	{
		scan_button = new wxButton(this, -1, _("Scan Start/End Frames"));
		main_sizer->Add(scan_button, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

		progress = new wxGauge(this, -1, 100, wxDefaultPosition, wxSize(-1, 14));
		progress->Hide();
		main_sizer->Add(progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		status_label = new wxStaticText(this, -1, _("Ready"));
		main_sizer->Add(status_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		result_label = new wxStaticText(this, -1, "");
		result_label->SetMinSize(wxSize(380, 60));
		main_sizer->Add(result_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		auto action_row = new wxBoxSizer(wxHORIZONTAL);
		insert_button = new wxButton(this, -1, _("Insert Line"));
		copy_button = new wxButton(this, -1, _("Copy"));
		action_row->Add(insert_button, 1, wxEXPAND | wxRIGHT, 4);
		action_row->Add(copy_button, 1, wxEXPAND);
		main_sizer->Add(action_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		auto close_row = new wxBoxSizer(wxHORIZONTAL);
		auto close_button = new wxButton(this, wxID_CANCEL, _("Close"));
		close_row->AddStretchSpacer();
		close_row->Add(close_button, 0, wxEXPAND);
		main_sizer->Add(close_row, 0, wxEXPAND | wxALL, 4);
	}

	SetSizerAndFit(main_sizer);
	SetSize(std::max(GetSize().GetWidth(), 480), GetSize().GetHeight() + 100);
	SetMinSize(GetSize());

	max_range_spin->SetValue(OPT_GET("Tool/HardSub/Max Frames")->GetInt());
	stride_spin->SetValue(OPT_GET("Tool/HardSub/Stride")->GetInt());
	threshold_spin->SetValue(OPT_GET("Tool/HardSub/Threshold")->GetInt());
	confirm_spin->SetValue(OPT_GET("Tool/HardSub/Confirm Frames")->GetInt());
	min_duration_spin->SetValue(OPT_GET("Tool/HardSub/Min Duration Frames")->GetInt());
	ocr_confirm_check->SetValue(OPT_GET("Tool/HardSub/Confirm With OCR")->GetBool());
	strict_ocr_check->SetValue(OPT_GET("Tool/HardSub/Strict OCR Check")->GetBool());

	select_button->Bind(wxEVT_BUTTON, &DialogHardSubScan::OnSelect, this);
	clear_button->Bind(wxEVT_BUTTON, &DialogHardSubScan::OnClear, this);
	recognize_button->Bind(wxEVT_BUTTON, &DialogHardSubScan::OnRecognize, this);
	scan_button->Bind(wxEVT_BUTTON, &DialogHardSubScan::OnScan, this);
	insert_button->Bind(wxEVT_BUTTON, &DialogHardSubScan::OnInsert, this);
	copy_button->Bind(wxEVT_BUTTON, &DialogHardSubScan::OnCopy, this);
	Bind(EVT_HARDSUB_RECOGNIZE_DONE, &DialogHardSubScan::OnRecognizeDone, this);
	Bind(EVT_HARDSUB_SCAN_PROGRESS, &DialogHardSubScan::OnScanProgress, this);
	Bind(EVT_HARDSUB_SCAN_DONE, &DialogHardSubScan::OnScanDone, this);
}

void DialogHardSubScan::UpdateControls() {
	bool has_video = c->project->VideoProvider() != nullptr;
	bool has_region = region_template_.Valid();

	select_button->Enable(has_video && !scanning_);
	clear_button->Enable(has_region && !scanning_);
	recognize_button->Enable(has_video && has_region && !scanning_);
	text_ctrl->Enable(has_region);
	scan_button->Enable(has_video && has_region && !scanning_);
	insert_button->Enable(has_region && !scanning_ && scan_result_.ok);
	copy_button->Enable(has_region && !scanning_ && scan_result_.ok);
}

void DialogHardSubScan::SetStatus(wxString const& text) {
	status_label->SetLabelText(text);
}

void DialogHardSubScan::GrabTemplate(int frame) {
	if (!c->project->VideoProvider())
		return;

	auto frame_data = c->videoController->GetFrame(frame, true);
	if (!frame_data)
		return;

	region_template_ = hardsub::CropRegion(*frame_data, region_.GetX(), region_.GetY(),
	                                       region_.GetWidth(), region_.GetHeight());
}

void DialogHardSubScan::ActivateRegionTool() {
	if (!c->project->VideoProvider() || !c->videoDisplay)
		return;

	auto tool = agi::make_unique<HardSubRegionTool>(c->videoDisplay, c,
		[this](wxRect const& video_region, int frame) { OnRegionSelected(video_region, frame); },
		region_);
	c->videoDisplay->SetTool(std::move(tool));
	SetStatus(_("Drag a rectangle over the subtitle on the video."));
}

void DialogHardSubScan::OnRegionSelected(wxRect const& video_region, int frame) {
	region_ = video_region;
	base_frame_ = frame;
	scan_result_ = hardsub::ScanResult();
	scan_start_ms_ = scan_end_ms_ = 0;
	GrabTemplate(frame);

	if (region_template_.Valid()) {
		region_label->SetLabelText(wxString::Format(_("Region: %d,%d %dx%d @ frame %d"),
		                                            region_.GetX(), region_.GetY(),
		                                            region_.GetWidth(), region_.GetHeight(), frame));
		result_label->SetLabelText("");
		SetStatus(_("Region locked. Recognizing text..."));
		UpdateControls();
		StartRecognize(frame);
	}
	else {
		SetStatus(_("Failed to capture region."));
		UpdateControls();
	}
}

void DialogHardSubScan::OnSelect(wxCommandEvent&) {
	ActivateRegionTool();
}

void DialogHardSubScan::OnClear(wxCommandEvent&) {
	CancelScan();
	ClearState();
	if (c->videoDisplay && c->videoDisplay->ToolIsType(typeid(HardSubRegionTool)))
		cmd::call("video/tool/cross", c);
	UpdateControls();
}

void DialogHardSubScan::ClearState() {
	region_ = wxRect();
	base_frame_ = -1;
	region_template_ = hardsub::RegionImage();
	scan_result_ = hardsub::ScanResult();
	scan_start_ms_ = scan_end_ms_ = 0;
	region_label->SetLabelText(_("No region selected"));
	result_label->SetLabelText("");
	text_ctrl->SetValue("");
	progress->Hide();
	Layout();
}

void DialogHardSubScan::OnRecognize(wxCommandEvent&) {
	if (!region_template_.Valid() || !c->project->VideoProvider())
		return;
	StartRecognize(c->videoController->GetFrameN());
}

void DialogHardSubScan::StartRecognize(int frame) {
	if (!region_template_.Valid() || !c->project->VideoProvider())
		return;

	ocr::OCROptions options;
	options.keep_line_breaks = true;
	options.language = LanguageOption();

	int frame_count = c->project->VideoProvider()->GetFrameCount();
	std::vector<int> frames;
	for (int f = frame - 2; f <= frame + 2; ++f) {
		if (f >= 0 && f < frame_count)
			frames.push_back(f);
	}

	auto tpl = region_template_;
	int region_x = region_.GetX();
	int region_y = region_.GetY();
	int region_w = region_.GetWidth();
	int region_h = region_.GetHeight();

	SetStatus(_("Recognizing..."));
	recognize_button->Enable(false);

	auto handler = this;
	agi::dispatch::Background().Async([handler, frames, frame, options, tpl,
	                                   region_x, region_y, region_w, region_h]{
		if (!handler->recognize_alive_.load())
			return;

		HardSubRecognizeOutcome outcome;
		FrameDetector recognize;
		ocr::OCRProcess process;
		ocr::OCREngine fallback_engine;
		std::string ocr_error;
		agi::fs::path executable, models_dir, config_path;
		if (ocr::OCRProcess::FindRuntime(executable, models_dir, config_path, ocr_error)
		    && process.Start(executable, models_dir, config_path, ocr_error)) {
			recognize = [&process, &options, handler](agi::fs::path const& image_path) {
				return process.RunImage(image_path, options, false,
					[handler] { return !handler->recognize_alive_.load(); });
			};
		}
		else if (fallback_engine.GetDiagnostic(options).empty()) {
			recognize = [&fallback_engine, options](agi::fs::path const& image_path) {
				return fallback_engine.RecognizeImage(image_path, options);
			};
		}
		else {
			outcome.error = ocr_error.empty() ? "OCR runtime is unavailable." : ocr_error;
		}

		if (recognize) {
			wxString temp;
			std::string best_text;
			int best_length = -1;
			double best_confidence = -1.0;
			int best_dist = 0;
			bool any_ok = false;

			for (int f : frames) {
				if (!handler->recognize_alive_.load())
					break;

				auto frame_data = handler->c->videoController->GetFrame(f, true);
				if (!frame_data)
					continue;

				// Only accept frames whose region still looks like the drawn
				// subtitle; a neighboring subtitle could otherwise win the
				// length contest on frames close to a cut.
				auto check = hardsub::CropRegion(*frame_data, region_x, region_y, region_w, region_h);
				if (!check.Valid() || hardsub::RegionChangedRatio(tpl, check) > 0.35)
					continue;

				// OCR on a slightly enlarged crop (a tight selection should
				// not cut characters off), upscaled for better small-text
				// detection.
				const int margin = 6;
				auto crop = hardsub::CropRegion(*frame_data, region_x - margin, region_y - margin,
				                                region_w + margin * 2, region_h + margin * 2);
				if (!crop.Valid())
					continue;

				wxImage img(crop.width, crop.height);
				if (!img.GetData())
					continue;
				std::memcpy(img.GetData(), crop.rgb.data(), crop.rgb.size());
				img = img.Scale(crop.width * 2, crop.height * 2, wxIMAGE_QUALITY_BICUBIC);

				if (temp.empty())
					temp = wxFileName::CreateTempFileName("aegisub-hardsub-");
				if (temp.empty() || !img.SaveFile(temp, wxBITMAP_TYPE_PNG))
					continue;

				auto result = recognize(agi::fs::path(std::wstring(temp.wc_str())));
				if (result.ok)
					any_ok = true;
				else {
					if (outcome.error.empty())
						outcome.error = result.diagnostic;
					continue;
				}
				if (result.text.empty())
					continue;

				int length = NonSpaceLength(result.text);
				double confidence = ConfidenceSum(result);
				int dist = f > frame ? f - frame : frame - f;
				if (length > best_length
				    || (length == best_length && dist < best_dist)
				    || (length == best_length && dist == best_dist && confidence > best_confidence)) {
					best_length = length;
					best_confidence = confidence;
					best_dist = dist;
					best_text = result.text;
				}
			}

			if (!temp.empty())
				agi::fs::Remove(agi::fs::path(std::wstring(temp.wc_str())));

			outcome.ok = any_ok;
			outcome.text = best_text;
			if (!any_ok && outcome.error.empty())
				outcome.error = "No OCR result could be read for the region.";
		}

		if (handler->recognize_alive_.load())
			handler->AddPendingEvent(ValueEvent<HardSubRecognizeOutcome>(
				EVT_HARDSUB_RECOGNIZE_DONE, -1, std::move(outcome)));
	});
}

void DialogHardSubScan::OnRecognizeDone(ValueEvent<HardSubRecognizeOutcome>& event) {
	auto const& outcome = event.Get();
	if (outcome.ok) {
		text_ctrl->SetValue(to_wx(outcome.text));
		if (outcome.text.empty())
			SetStatus(_("Recognition complete, but no text was found in the region."));
		else
			SetStatus(_("Recognition complete."));
	}
	else {
		SetStatus(outcome.error.empty() ? _("Recognition failed.") : to_wx(outcome.error));
	}
	UpdateControls();
}

void DialogHardSubScan::OnScan(wxCommandEvent&) {
	if (scanning_ || !region_template_.Valid() || !c->project->VideoProvider())
		return;

	CancelScan();

	OPT_SET("Tool/HardSub/Max Frames")->SetInt(max_range_spin->GetValue());
	OPT_SET("Tool/HardSub/Stride")->SetInt(stride_spin->GetValue());
	OPT_SET("Tool/HardSub/Threshold")->SetInt(threshold_spin->GetValue());
	OPT_SET("Tool/HardSub/Confirm Frames")->SetInt(confirm_spin->GetValue());
	OPT_SET("Tool/HardSub/Min Duration Frames")->SetInt(min_duration_spin->GetValue());
	OPT_SET("Tool/HardSub/Confirm With OCR")->SetBool(ocr_confirm_check->GetValue());
	OPT_SET("Tool/HardSub/Strict OCR Check")->SetBool(strict_ocr_check->GetValue());

	hardsub::ScanOptions options;
	options.max_frames = max_range_spin->GetValue();
	options.stride = stride_spin->GetValue();
	options.threshold = threshold_spin->GetValue() / 100.0;
	options.confirm_frames = confirm_spin->GetValue();
	options.min_duration_frames = min_duration_spin->GetValue();

	auto tpl = region_template_;
	int region_x = region_.GetX();
	int region_y = region_.GetY();
	int base_frame = base_frame_ >= 0 ? base_frame_ : c->videoController->GetFrameN();
	int frame_count = c->project->VideoProvider()->GetFrameCount();
	bool confirm_with_ocr = ocr_confirm_check->GetValue();
	bool strict_ocr = strict_ocr_check->GetValue();
	std::string reference_text = from_wx(text_ctrl->GetValue());
	if (strict_ocr && reference_text.empty())
		strict_ocr = false;

	ocr::OCREngine engine;
	ocr::OCROptions ocr_options;
	ocr_options.keep_line_breaks = true;
	ocr_options.language = LanguageOption();
	if (confirm_with_ocr && !engine.GetDetectionDiagnostic(ocr_options).empty())
		confirm_with_ocr = false;
	if (strict_ocr && !engine.GetDiagnostic(ocr_options).empty())
		strict_ocr = false;

	cancel_scan_ = false;
	scanning_ = true;
	scan_result_ = hardsub::ScanResult();
	result_label->SetLabelText("");
	progress->SetValue(0);
	progress->Show();
	Layout();
	SetStatus(_("Scanning frames..."));
	UpdateControls();

	auto handler = this;
	scan_thread_ = std::thread([handler, options, tpl, region_x, region_y, base_frame, frame_count,
	                            confirm_with_ocr, strict_ocr, reference_text, ocr_options]{
		auto loader = [handler](int frame) -> std::shared_ptr<VideoFrame> {
			if (handler->cancel_scan_.load())
				return {};
			if (!handler->c->project->VideoProvider())
				return {};
			return handler->c->videoController->GetFrame(frame, true);
		};
		auto cancel = [handler] { return handler->cancel_scan_.load(); };
		auto progress = [handler](int done, int total) {
			handler->AddPendingEvent(ValueEvent<HardSubScanProgress>(
				EVT_HARDSUB_SCAN_PROGRESS, -1, HardSubScanProgress{done, total}));
		};

		// Prefer a single persistent OCR process for the whole boundary batch;
		// fall back to the one-shot engine when the persistent mode is not
		// available (e.g. non-Windows builds or a broken runtime). The strict
		// pass needs full recognition (text), while the fast confirmation only
		// needs detection (boxes), so both lambdas can share the same process.
		FrameDetector detect;
		FrameDetector recognize;
		ocr::OCRProcess persistent_process;
		ocr::OCREngine fallback_engine;
		std::string ocr_error;
		if (confirm_with_ocr || strict_ocr) {
			agi::fs::path executable, models_dir, config_path;
			if (ocr::OCRProcess::FindRuntime(executable, models_dir, config_path, ocr_error)
			    && persistent_process.Start(executable, models_dir, config_path, ocr_error)) {
				if (confirm_with_ocr)
					detect = [&persistent_process, &ocr_options, &cancel](agi::fs::path const& image_path) {
						return persistent_process.RunImage(image_path, ocr_options, true, cancel);
					};
				if (strict_ocr)
					recognize = [&persistent_process, &ocr_options, &cancel](agi::fs::path const& image_path) {
						return persistent_process.RunImage(image_path, ocr_options, false, cancel);
					};
			}
			else {
				if (confirm_with_ocr && fallback_engine.GetDetectionDiagnostic(ocr_options).empty()) {
					detect = [&fallback_engine, &ocr_options](agi::fs::path const& image_path) {
						return fallback_engine.DetectTextRegions(image_path, ocr_options);
					};
				}
				if (strict_ocr && fallback_engine.GetDiagnostic(ocr_options).empty()) {
					recognize = [&fallback_engine, &ocr_options](agi::fs::path const& image_path) {
						return fallback_engine.RecognizeImage(image_path, ocr_options);
					};
				}
			}
		}

		HardSubScanOutcome outcome;
		outcome.result = hardsub::ScanBoundaries(tpl, region_x, region_y, base_frame, frame_count,
		                                         options, loader, cancel, progress);
		if (cancel()) {
			outcome.cancelled = true;
		}
		else if (!outcome.result.ok) {
			outcome.error = outcome.result.error.empty()
				? "Could not find a subtitle run around the base frame. "
				  "Try a lower text match value or check the selected region."
				: outcome.result.error;
		}
		else {
			bool strict_applied = false;
			if (strict_ocr && recognize) {
				strict_applied = RefineTimelineWithOcr(recognize, loader, tpl, region_x, region_y,
				                                       base_frame, frame_count, outcome.result,
				                                       reference_text, cancel, progress);
				if (cancel())
					outcome.cancelled = true;
			}

			if (!outcome.cancelled && !strict_applied && detect) {
				int window = 2;
				int start = AdjustBoundaryWithOcr(detect, loader, tpl, region_x, region_y,
				                                  outcome.result.start_frame, window, true, cancel);
				int end = AdjustBoundaryWithOcr(detect, loader, tpl, region_x, region_y,
				                                outcome.result.end_frame, window, false, cancel);
				start = std::max(start, outcome.result.start_frame - window);
				end = std::min(end, outcome.result.end_frame + window);
				if (start <= end && start <= base_frame && end >= base_frame) {
					outcome.result.start_frame = start;
					outcome.result.end_frame = end;
				}
			}

			if (!cancel()) {
				outcome.ok = true;
				outcome.start_ms = handler->c->videoController->TimeAtFrame(
					outcome.result.start_frame, agi::vfr::START);
				outcome.end_ms = handler->c->videoController->TimeAtFrame(
					outcome.result.end_frame, agi::vfr::END);
				// Sanity check: a duration far longer than the text length
				// suggests is a strong sign that consecutive subtitles were
				// merged into one run.
				if (strict_applied) {
					int duration_ms = outcome.end_ms - outcome.start_ms;
					int plausible_ms = std::max(1000, NonSpaceLength(reference_text) * 60);
					outcome.suspect_merge = duration_ms > plausible_ms * 3;
				}
			}
			else {
				outcome.cancelled = true;
			}
		}
		handler->AddPendingEvent(ValueEvent<HardSubScanOutcome>(
			EVT_HARDSUB_SCAN_DONE, -1, std::move(outcome)));
	});
}

void DialogHardSubScan::OnScanProgress(ValueEvent<HardSubScanProgress>& event) {
	auto const& p = event.Get();
	if (p.total > 0) {
		progress->SetRange(p.total);
		progress->SetValue(std::min(p.done, p.total));
	}
}

void DialogHardSubScan::OnScanDone(ValueEvent<HardSubScanOutcome>& event) {
	auto outcome = event.Get();
	scanning_ = false;
	progress->Hide();

	if (outcome.cancelled) {
		SetStatus(_("Scan cancelled."));
	}
	else if (outcome.ok) {
		scan_result_ = std::move(outcome.result);
		scan_start_ms_ = outcome.start_ms;
		scan_end_ms_ = outcome.end_ms;
		wxString label = agi::wxformat(
			_("Start: frame %d (%s)\nEnd: frame %d (%s)\nDuration: %d frames"),
			scan_result_.start_frame, agi::Time(scan_start_ms_).GetAssFormatted(true),
			scan_result_.end_frame, agi::Time(scan_end_ms_).GetAssFormatted(true),
			scan_result_.end_frame - scan_result_.start_frame + 1);
		if (outcome.suspect_merge) {
			label += _("\nWarning: the duration is far longer than the text length suggests; "
			           "the range may contain multiple subtitles. Check the boundaries before inserting.");
		}
		result_label->SetLabelText(label);
		SetStatus(_("Scan complete."));
	}
	else {
		SetStatus(to_wx(outcome.error));
	}
	// Refresh the layout after the multi-line result/status labels are set so
	// the result text never overlaps the buttons below it.
	Layout();
	UpdateControls();
}

void DialogHardSubScan::OnInsert(wxCommandEvent&) {
	if (!scan_result_.ok || scan_start_ms_ >= scan_end_ms_)
		return;

	wxString text = text_ctrl->GetValue();
	if (text.IsEmpty()) {
		wxMessageBox(_("The recognized text is empty. Edit it before inserting."),
		             _("Hard Subtitle"), wxOK | wxICON_WARNING | wxCENTER, this);
		return;
	}

	std::string ass_text = from_wx(text);
	boost::replace_all(ass_text, "\r\n", "\\N");
	boost::replace_all(ass_text, "\r", "\\N");
	boost::replace_all(ass_text, "\n", "\\N");

	auto line = new AssDialogue();
	line->Start = scan_start_ms_;
	line->End = scan_end_ms_;
	line->Text = ass_text;
	line->Style = c->ass->Styles.empty() ? std::string("Default") : c->ass->Styles.front().name;

	auto& events = c->ass->Events;
	auto it = std::find_if(events.begin(), events.end(),
		[&](AssDialogue const& d) { return d.Start > line->Start; });
	events.insert(it, *line);

	c->ass->Commit(_("insert hard subtitle line"), AssFile::COMMIT_DIAG_ADDREM);
	c->selectionController->SetSelectionAndActive(Selection{line}, line);
	SetStatus(_("Subtitle line inserted."));
}

void DialogHardSubScan::OnCopy(wxCommandEvent&) {
	if (!scan_result_.ok)
		return;
	SetClipboard(from_wx(text_ctrl->GetValue()));
	SetStatus(_("Copied to clipboard."));
}

void DialogHardSubScan::OnClose(wxCloseEvent& event) {
	CancelScan();
	if (c->videoDisplay && c->videoDisplay->ToolIsType(typeid(HardSubRegionTool)))
		cmd::call("video/tool/cross", c);
	event.Skip();
}

void DialogHardSubScan::OnVideoChanged(AsyncVideoProvider *provider) {
	if (!provider) {
		CancelScan();
		ClearState();
	}
	UpdateControls();
}

void DialogHardSubScan::CancelScan() {
	cancel_scan_ = true;
	if (scan_thread_.joinable())
		scan_thread_.join();
	scanning_ = false;
}
