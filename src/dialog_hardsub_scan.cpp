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
#include <chrono>
#include <cctype>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
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

/// Shared state for the background-pre-warmed detection-only OCR engine. The
/// warmup task captures a copy of the shared_ptr, so it never touches the
/// dialog after destruction and the dialog never joins it.
/// Defined at global scope to match the forward declaration in the header.
struct OcrWarmState {
	std::mutex mutex;
	std::shared_ptr<ocr::OCRProcess> process;
	std::atomic<bool> ready{false};
};

namespace {

wxDEFINE_EVENT(EVT_HARDSUB_RECOGNIZE_DONE, ValueEvent<HardSubRecognizeOutcome>);
wxDEFINE_EVENT(EVT_HARDSUB_SCAN_PROGRESS, ValueEvent<HardSubScanProgress>);
wxDEFINE_EVENT(EVT_HARDSUB_SCAN_PIXEL_DONE, ValueEvent<HardSubScanOutcome>);
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

/// Snap a pixel-detected boundary with joint pixel+OCR evidence inside a small
/// window. Each frame in the window gets its template coverage and a
/// detection-only OCR call on a slightly enlarged, upscaled crop; the boundary
/// moves only toward frames where both signals agree (coverage at least
/// `threshold_exit` and a detected text box), and never farther than the
/// window. Frames whose coverage is already below `threshold_exit` skip the
/// OCR call entirely (they cannot qualify), and coverage already recorded in
/// `known_coverage` (the pixel scan's evidence series) is reused instead of
/// reloading the frame. Falls back to the pixel boundary when no frame
/// qualifies.
int SnapBoundaryJoint(FrameDetector const& detect, hardsub::FrameLoader const& loader,
                      hardsub::RegionImage const& tpl, std::vector<int> const& text_pixels,
                      int region_x, int region_y, int boundary, int window,
                      double threshold_exit, bool find_earliest,
                      std::map<int, double> const* known_coverage,
                      hardsub::CancelFn const& cancel) {
	std::vector<int> frames;
	std::vector<double> coverage;
	std::vector<bool> ocr_box;
	wxString temp;

	for (int f = boundary - window; f <= boundary + window; ++f) {
		if (cancel && cancel())
			break;

		double cov = -1.0;
		std::shared_ptr<VideoFrame> frame_data;
		if (known_coverage) {
			auto it = known_coverage->find(f);
			if (it != known_coverage->end())
				cov = it->second;
		}
		if (cov < 0.0) {
			frame_data = loader(f);
			if (!frame_data)
				continue;
			auto crop = hardsub::CropRegion(*frame_data, region_x, region_y,
			                                tpl.width, tpl.height);
			if (!crop.Valid())
				continue;
			cov = hardsub::TemplateTextCoverage(tpl, crop, text_pixels);
		}
		coverage.push_back(cov);

		// OCR on a slightly enlarged crop (a tight selection should not cut
		// characters off), upscaled for better small-text detection. The
		// pixel check above uses the exact region; only the OCR input gets the
		// margin so the boundary stays tied to the user's selection.
		bool box = false;
		if (cov >= threshold_exit) {
			if (!frame_data)
				frame_data = loader(f);
			if (frame_data) {
				auto ocr_crop = hardsub::CropRegion(*frame_data, region_x - 6, region_y - 6,
				                                    tpl.width + 12, tpl.height + 12);
				if (ocr_crop.Valid()) {
					wxImage img(ocr_crop.width, ocr_crop.height);
					if (img.GetData()) {
						std::memcpy(img.GetData(), ocr_crop.rgb.data(), ocr_crop.rgb.size());
						img = img.Scale(ocr_crop.width * 2, ocr_crop.height * 2,
						                wxIMAGE_QUALITY_BICUBIC);
						if (temp.empty())
							temp = wxFileName::CreateTempFileName("aegisub-hardsub-");
						if (!temp.empty() && img.SaveFile(temp, wxBITMAP_TYPE_PNG)) {
							auto result = detect(agi::fs::path(std::wstring(temp.wc_str())));
							agi::fs::Remove(agi::fs::path(std::wstring(temp.wc_str())));
							box = result.ok && !result.lines.empty();
						}
					}
				}
			}
		}
		ocr_box.push_back(box);
		frames.push_back(f);
	}

	if (!temp.empty())
		agi::fs::Remove(agi::fs::path(std::wstring(temp.wc_str())));
	if (frames.empty())
		return boundary;

	int fallback = boundary - frames.front();
	int snapped = frames.front()
		+ hardsub::SnapBoundaryFromEvidence(coverage, ocr_box, threshold_exit,
		                                    find_earliest, fallback);
	return snapped;
}

/// True when the pixel-scan coverage series shows interior frames inside the
/// reported run whose coverage dips below the enter threshold (dips that the
/// debounce filled, or a fade between two back-to-back subtitles). Used only
/// as a merge suspicion hint; the boundaries themselves are not changed by it.
bool RunHasInteriorDip(hardsub::ScanResult const& result, double enter, int confirm) {
	if (!result.ok || result.frames.size() < 4)
		return false;
	// The scan only produces a dense, contiguous series for short spans;
	// probe/binary samples for long spans have gaps, so skip those.
	for (size_t i = 1; i < result.frames.size(); ++i) {
		if (result.frames[i] - result.frames[i - 1] != 1)
			return false;
	}
	if (result.frames.front() > result.start_frame || result.frames.back() < result.end_frame)
		return false;

	size_t begin = size_t(result.start_frame - result.frames.front());
	size_t end = size_t(result.end_frame - result.frames.front());
	int dip = 0;
	for (size_t i = begin + 1; i + 1 < end; ++i) {
		if (result.diffs[i] < enter) {
			if (++dip >= std::max(1, confirm))
				return true;
		}
		else {
			dip = 0;
		}
	}
	return false;
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
	ocr_warm_state_ = std::make_shared<OcrWarmState>();
	StartOcrWarmup();

	persist = agi::make_unique<PersistLocation>(this, "Tool/HardSub");

	Bind(wxEVT_CLOSE_WINDOW, &DialogHardSubScan::OnClose, this);
	connections = agi::signal::make_vector({
		c->project->AddVideoProviderListener(&DialogHardSubScan::OnVideoChanged, this)
	});
}

DialogHardSubScan::~DialogHardSubScan() {
	recognize_alive_ = false;
	CancelScan();
	if (ocr_warm_state_) {
		{
			std::lock_guard<std::mutex> lock(ocr_warm_state_->mutex);
			if (ocr_warm_state_->process)
				ocr_warm_state_->process->Stop();
		}
		// The warmup task keeps the state alive if it is still starting; it
		// will finish on its own and release the engine.
		ocr_warm_state_.reset();
	}
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

		text_ctrl = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize,
		                           wxTE_MULTILINE | wxTE_RICH2);
		// Default to a 4-line OCR result box; it still grows with the dialog.
		const int text_lines = 4;
		text_ctrl->SetMinSize(wxSize(-1, text_ctrl->GetCharHeight() * text_lines + 8));
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

	int info_lines = 0;
	{
		scan_button = new wxButton(this, -1, _("Scan Start/End Frames"));
		main_sizer->Add(scan_button, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

		progress = new wxGauge(this, -1, 100, wxDefaultPosition, wxSize(-1, 14));
		progress->Hide();
		main_sizer->Add(progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		status_label = new wxStaticText(this, -1, _("Ready"));
		main_sizer->Add(status_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		result_label = new wxStaticText(this, -1, "");
		// The info area shares window space with the region box and defaults
		// to 4.5 text lines of height, which fits the longest scan result
		// (3-line summary plus the merge warning).
		info_lines = static_cast<int>(result_label->GetCharHeight() * 4.5);
		// Min width is a quarter smaller than the original 380.
		result_label->SetMinSize(wxSize(285, info_lines));
		main_sizer->Add(result_label, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

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
	Layout();
	// The minimum height is the fixed content (buttons, options, status line);
	// the flexible OCR box and info area compress when the window shrinks.
	const int fixed_min_height = GetSize().GetHeight()
		- text_ctrl->GetSize().GetHeight() - result_label->GetSize().GetHeight();
	// Open compact instead of at the full fitted size: the info area starts at
	// about half of its 4.5-line default, so the dialog does not need to be
	// dragged smaller by hand. Enlarging the window reveals the full area.
	const int compact_offset = info_lines / 2;
	SetSize(std::max(GetSize().GetWidth(), 360), GetSize().GetHeight() - compact_offset);
	// Minimum width is a quarter smaller than the original 480.
	SetMinSize(wxSize(360, std::max(260, fixed_min_height)));

	max_range_spin->SetValue(OPT_GET("Tool/HardSub/Max Frames")->GetInt());
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
	Bind(EVT_HARDSUB_SCAN_PIXEL_DONE, &DialogHardSubScan::OnScanPixelDone, this);
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
		try {
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
				outcome.error = ocr_error.empty() ? from_wx(_("OCR runtime is unavailable.")) : ocr_error;
			}
		}
		catch (...) {
			outcome.error = from_wx(_("OCR runtime failed to start."));
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
				outcome.error = from_wx(_("No OCR result could be read for the region."));
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
	OPT_SET("Tool/HardSub/Threshold")->SetInt(threshold_spin->GetValue());
	OPT_SET("Tool/HardSub/Confirm Frames")->SetInt(confirm_spin->GetValue());
	OPT_SET("Tool/HardSub/Min Duration Frames")->SetInt(min_duration_spin->GetValue());
	OPT_SET("Tool/HardSub/Confirm With OCR")->SetBool(ocr_confirm_check->GetValue());
	OPT_SET("Tool/HardSub/Strict OCR Check")->SetBool(strict_ocr_check->GetValue());

	hardsub::ScanOptions options;
	options.max_frames = max_range_spin->GetValue();
	options.threshold = threshold_spin->GetValue() / 100.0;
	// The exit threshold is deliberately much lower than the enter threshold
	// so the hysteresis zone covers slow fades: frames fading in/out keep the
	// run state, and the OCR boundary confirmation (which only runs when the
	// boundary is ambiguous) can pull the edge back to the first/last frame
	// where the text is actually readable.
	options.threshold_exit = std::max(0.15, options.threshold * 0.5);
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
	std::vector<int> keyframes = c->project->VideoProvider()->GetKeyFrames();

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

	// Pause playback while scanning: the scan saturates the video decoder
	// worker with synchronous frame requests, which would otherwise starve
	// playback and make the UI appear frozen.
	resume_playback_ = c->videoController->IsPlaying();
	if (resume_playback_)
		c->videoController->Stop();

	auto handler = this;
	agi::fs::path video_filename;
	if (auto provider = c->project->VideoProvider())
		video_filename = provider->GetFilename();
	int region_w = region_.GetWidth();
	int region_h = region_.GetHeight();

	scan_thread_ = std::thread([handler, options, tpl, region_x, region_y, region_w, region_h,
	                            base_frame, frame_count, confirm_with_ocr, strict_ocr,
	                            reference_text, ocr_options, keyframes, video_filename]{
		auto loader = [handler](int frame) -> std::shared_ptr<VideoFrame> {
			if (handler->cancel_scan_.load())
				return {};
			if (!handler->c->project->VideoProvider())
				return {};
			return handler->c->videoController->GetFrame(frame, true);
		};

		// The normal provider path converts every 4K frame to BGRA, which
		// costs about 100 ms per frame and makes a dense scan of a long
		// subtitle take close to a minute. When the file is FFMS2-readable,
		// open a lightweight scan decoder that hands us native YUV frames
		// (CropRegion converts only the selected region), dropping the
		// per-frame cost to a few milliseconds. If that fails for any reason
		// the scan falls back to the normal provider path.
		std::string decoder_error;
		auto decoder = std::make_shared<hardsub::ScanVideoDecoder>(video_filename, decoder_error);
		bool use_decoder = !video_filename.empty() && decoder->Valid();
		auto scan_tpl = tpl;
		if (use_decoder) {
			auto base = decoder->GetFrame(base_frame);
			if (base) {
				auto rebuilt = hardsub::CropRegion(*base, region_x, region_y, region_w, region_h);
				if (rebuilt.Valid())
					scan_tpl = std::move(rebuilt);
				else
					use_decoder = false;
			}
			else
				use_decoder = false;
		}
		auto scan_loader = use_decoder
			? hardsub::FrameLoader([decoder](int frame) {
				return decoder->GetFrame(frame);
			})
			: loader;

		auto cancel = [handler] { return handler->cancel_scan_.load(); };
		auto progress = [handler](int done, int total) {
			handler->AddPendingEvent(ValueEvent<HardSubScanProgress>(
				EVT_HARDSUB_SCAN_PROGRESS, -1, HardSubScanProgress{done, total}));
		};

		// The detection-only engine is pre-warmed by the dialog (model loading
		// overlaps region selection and text editing); the strict pass needs
		// recognized text, so it starts its own full-pipeline engine
		// asynchronously and only when enabled.
		auto start_full_process = []() -> std::shared_ptr<ocr::OCRProcess> {
			try {
				auto process = std::make_shared<ocr::OCRProcess>();
				agi::fs::path executable, models_dir, config_path;
				std::string diagnostic;
				if (ocr::OCRProcess::FindRuntime(executable, models_dir, config_path, diagnostic)
				    && process->Start(executable, models_dir, config_path, diagnostic))
					return process;
			}
			catch (...) {
			}
			return {};
		};

		std::future<std::shared_ptr<ocr::OCRProcess>> strict_ready;
		if (strict_ocr)
			strict_ready = std::async(std::launch::async, start_full_process);

		HardSubScanOutcome outcome;
		try {
			outcome.result = hardsub::ScanBoundaries(scan_tpl, region_x, region_y, base_frame,
			                                         frame_count, options, scan_loader, cancel,
			                                         progress, keyframes);
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
				// Wait for the OCR engine(s) to finish loading while keeping
				// the scan cancellable; a missing engine just disables the
				// OCR phase and the pixel result stands.
				std::shared_ptr<ocr::OCRProcess> full_process;
				auto wait_for_process = [&](std::future<std::shared_ptr<ocr::OCRProcess>>& ready,
				                            std::shared_ptr<ocr::OCRProcess>& out) {
					if (!ready.valid() || cancel())
						return;
					for (int i = 0; i < 50; ++i) {
						if (ready.wait_for(std::chrono::milliseconds(100))
						    == std::future_status::ready)
							break;
						if (cancel())
							return;
					}
					if (ready.wait_for(std::chrono::milliseconds(0))
					    == std::future_status::ready) {
						try { out = ready.get(); } catch (...) { }
					}
				};
				wait_for_process(strict_ready, full_process);

				// The pre-warmed detection-only engine: it has been loading
				// since the dialog opened; wait briefly for it (the pixel
				// scan above already gave it time), then proceed without OCR
				// if it is still not ready so the scan stays fast.
				std::shared_ptr<ocr::OCRProcess> detect_process;
				{
					auto state = handler->ocr_warm_state_;
					if (state) {
						for (int i = 0; i < 15 && !cancel(); ++i) {
							if (state->ready.load())
								break;
							std::this_thread::sleep_for(std::chrono::milliseconds(100));
						}
						if (state->ready.load()) {
							std::lock_guard<std::mutex> lock(state->mutex);
							detect_process = state->process;
						}
					}
				}

				FrameDetector detect;
				FrameDetector recognize;
				if (confirm_with_ocr && !strict_ocr && detect_process) {
					detect = [&detect_process, &ocr_options, &cancel](agi::fs::path const& image_path) {
						return detect_process->RunImage(image_path, ocr_options, true, cancel);
					};
				}
				if (strict_ocr && full_process) {
					recognize = [&full_process, &ocr_options, &cancel](agi::fs::path const& image_path) {
						return full_process->RunImage(image_path, ocr_options, false, cancel);
					};
				}
				else if (strict_ocr) {
					// One-shot engine fallback for platforms where the
					// persistent process is unavailable.
					ocr::OCREngine fallback_engine;
					if (fallback_engine.GetDiagnostic(ocr_options).empty()) {
						recognize = [&fallback_engine, &ocr_options](agi::fs::path const& image_path) {
							return fallback_engine.RecognizeImage(image_path, ocr_options);
						};
					}
				}

				bool may_refine = detect || (strict_ocr && recognize);
				if (!cancel() && may_refine) {
					// Show the frame-precise pixel result immediately; the OCR
					// verification below refines it and posts the final result.
					HardSubScanOutcome prelim;
					prelim.ok = true;
					prelim.preliminary = true;
					prelim.result = outcome.result;
					prelim.start_ms = handler->c->videoController->TimeAtFrame(
						outcome.result.start_frame, agi::vfr::START);
					prelim.end_ms = handler->c->videoController->TimeAtFrame(
						outcome.result.end_frame, agi::vfr::END);
					handler->AddPendingEvent(ValueEvent<HardSubScanOutcome>(
						EVT_HARDSUB_SCAN_PIXEL_DONE, -1, std::move(prelim)));
				}

				bool strict_applied = false;
				if (strict_ocr && recognize) {
					strict_applied = RefineTimelineWithOcr(recognize, scan_loader, scan_tpl, region_x,
					                                       region_y, base_frame, frame_count,
					                                       outcome.result, reference_text,
					                                       cancel, progress);
					if (cancel())
						outcome.cancelled = true;
				}

				if (!outcome.cancelled && !strict_applied && detect) {
					int window = 2;
					auto text_pixels = hardsub::TemplateTextPixels(scan_tpl);
					if (!text_pixels.empty()) {
						// Reuse the coverage the pixel scan already computed
						// for the boundary windows instead of reloading frames.
						std::map<int, double> cov_map;
						for (size_t i = 0; i < outcome.result.frames.size(); ++i)
							cov_map.emplace(outcome.result.frames[i], outcome.result.diffs[i]);

						// OCR confirmation only helps when the pixel edge is
						// ambiguous (a fade or occlusion leaves text-like
						// coverage just outside the run); clean edges are
						// already frame-exact and skip the OCR calls.
						int start = outcome.result.start_frame;
						int end = outcome.result.end_frame;
						if (hardsub::BoundaryAmbiguous(outcome.result, options.threshold_exit, true))
							start = SnapBoundaryJoint(detect, scan_loader, scan_tpl, text_pixels,
							                          region_x, region_y, start, window,
							                          options.threshold_exit, true, &cov_map,
							                          cancel);
						if (hardsub::BoundaryAmbiguous(outcome.result, options.threshold_exit, false))
							end = SnapBoundaryJoint(detect, scan_loader, scan_tpl, text_pixels,
							                        region_x, region_y, end, window,
							                        options.threshold_exit, false, &cov_map,
							                        cancel);
						start = std::max(start, outcome.result.start_frame - window);
						end = std::min(end, outcome.result.end_frame + window);
						if (start <= end && start <= base_frame && end >= base_frame) {
							outcome.result.start_frame = start;
							outcome.result.end_frame = end;
						}
					}
				}

				if (!cancel()) {
					outcome.ok = true;
					outcome.start_ms = handler->c->videoController->TimeAtFrame(
						outcome.result.start_frame, agi::vfr::START);
					outcome.end_ms = handler->c->videoController->TimeAtFrame(
						outcome.result.end_frame, agi::vfr::END);
					// Sanity check: a duration far longer than the text
					// length suggests is a strong sign that consecutive
					// subtitles were merged into one run.
					if (!reference_text.empty()) {
						int duration_ms = outcome.end_ms - outcome.start_ms;
						int plausible_ms = std::max(1000, NonSpaceLength(reference_text) * 60);
						outcome.suspect_merge = duration_ms > plausible_ms * 3;
					}
					outcome.suspect_merge = outcome.suspect_merge
						|| RunHasInteriorDip(outcome.result, options.threshold,
						                     options.confirm_frames);
				}
				else {
					outcome.cancelled = true;
				}
			}
		}
		catch (std::exception const& e) {
			outcome = HardSubScanOutcome();
			outcome.error = from_wx(agi::wxformat(_("Hard subtitle scan failed: %s"), e.what()));
		}
		catch (...) {
			outcome = HardSubScanOutcome();
			outcome.error = from_wx(_("Hard subtitle scan failed with an unknown error."));
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

void DialogHardSubScan::OnScanPixelDone(ValueEvent<HardSubScanOutcome>& event) {
	auto const& outcome = event.Get();
	if (!outcome.ok || !outcome.result.ok)
		return;

	wxString label = agi::wxformat(
		_("Pixel scan: frame %d (%s) to frame %d (%s)\n"
		  "Verifying boundaries with OCR..."),
		outcome.result.start_frame, agi::Time(outcome.start_ms).GetAssFormatted(true),
		outcome.result.end_frame, agi::Time(outcome.end_ms).GetAssFormatted(true));
	result_label->SetLabelText(label);
	SetStatus(_("Verifying boundaries with OCR..."));
	Layout();
}

void DialogHardSubScan::OnScanDone(ValueEvent<HardSubScanOutcome>& event) {
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
		scan_result_ = std::move(outcome.result);
		scan_start_ms_ = outcome.start_ms;
		scan_end_ms_ = outcome.end_ms;
		wxString label = agi::wxformat(
			_("Start: frame %d (%s)\nEnd: frame %d (%s)\nDuration: %d frames"),
			scan_result_.start_frame, agi::Time(scan_start_ms_).GetAssFormatted(true),
			scan_result_.end_frame, agi::Time(scan_end_ms_).GetAssFormatted(true),
			scan_result_.end_frame - scan_result_.start_frame + 1);
		if (outcome.suspect_merge) {
			label += _("\nWarning: the range may contain multiple subtitles or a mid-run "
			           "transition. Check the boundaries before inserting.");
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
	if (resume_playback_) {
		resume_playback_ = false;
		c->videoController->Play();
	}
}

void DialogHardSubScan::StartOcrWarmup() {
	auto state = ocr_warm_state_;
	if (!state)
		return;

	// Start the detection-only engine on the background dispatcher so model
	// loading (a few seconds) happens while the user selects the region and
	// edits the text, not on the scan critical path. The task captures the
	// shared state by value, never the dialog, so it is safe to close the
	// dialog while the engine is still loading; dispatch also routes any
	// exception to the crash handler instead of terminating the process.
	agi::dispatch::Background().Async([state] {
		try {
			auto process = std::make_shared<ocr::OCRProcess>();
			agi::fs::path executable, models_dir, config_path;
			std::string diagnostic;
			if (ocr::OCRProcess::FindRuntime(executable, models_dir, config_path, diagnostic)
			    && process->Start(executable, models_dir, config_path, diagnostic,
			                      ocr::OCRProcessConfig{true, false, false})) {
				std::lock_guard<std::mutex> lock(state->mutex);
				state->process = std::move(process);
				state->ready = true;
			}
		}
		catch (...) {
			// OCR startup failure must never crash the app.
		}
	});
}
