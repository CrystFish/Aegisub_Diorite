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

/// @file dialog_localization.cpp
/// @brief Game localization matching dialog.

#include "dialog_localization.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "charset_detect.h"
#include "compat.h"
#include "dialog_manager.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "persist_location.h"
#include "selection_controller.h"
#include "subs_edit_box.h"
#include "text_file_reader.h"
#include "utils.h"

#include "localization/localization_loader.h"
#include "localization/localization_matcher.h"

#include <libaegisub/fs.h>
#include <libaegisub/exception.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/signal.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/filedlg.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {

std::string ReplaceAll(std::string s, std::string const& from, std::string const& to) {
	if (from.empty()) return s;
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}

/// Convert plain text to ASS text, escaping real line breaks as \N.
std::string AssEscapeNewlines(std::string s) {
	s = ReplaceAll(std::move(s), "\r\n", "\\N");
	s = ReplaceAll(std::move(s), "\r", "\\N");
	return ReplaceAll(std::move(s), "\n", "\\N");
}

std::string PathUtf8(agi::fs::path const& path) {
	return from_wx(wxString(path.wstring()));
}

std::string ReadLocalizationFile(agi::fs::path const& path, std::string& error) {
	try {
		std::string encoding = CharSetDetect::GetEncoding(path);
		TextFileReader reader(path, encoding, false);
		std::string content;
		while (reader.HasMoreLines()) {
			content += reader.ReadLineFromFile();
			content += "\n";
		}
		return content;
	}
	catch (agi::Exception const& e) {
		error = e.GetMessage();
		return {};
	}
}

} // namespace

struct DialogLocalization::Impl {
	DialogLocalization *dialog;
	agi::Context *c;

	agi::signal::Connection active_line_connection;
	agi::signal::Connection commit_connection;

	AssDialogue *active_line = nullptr;
	std::vector<agi::fs::path> paths;
	std::vector<localization::LocalizationFile> files;
	std::vector<localization::MatchResult> results;
	bool updating = false;

	wxTextCtrl *original_text = nullptr;
	wxCheckBox *fuzzy_check = nullptr;
	wxCheckBox *tags_check = nullptr;
	wxCheckBox *punct_check = nullptr;
	wxCheckBox *case_check = nullptr;
	wxCheckBox *sentence_check = nullptr;
	wxCheckBox *clean_check = nullptr;
	wxTextCtrl *regex_ctrl = nullptr;
	wxComboBox *language_combo = nullptr;
	wxSpinCtrlDouble *threshold_spin = nullptr;
	wxListBox *files_box = nullptr;
	wxListCtrl *results_list = nullptr;
	wxStaticText *status = nullptr;
	wxButton *replace_button = nullptr;
	wxButton *insert_button = nullptr;
	wxButton *copy_button = nullptr;

	std::unique_ptr<PersistLocation> persist;

	Impl(DialogLocalization *dialog, agi::Context *c);

	localization::MatchOptions CurrentOptions() const;
	void SaveOptions();
	void LoadPersistedFiles();
	void LoadFiles();
	void UpdateFilesBox();
	void UpdateDisplay();
	void RefreshMatch();
	void UpdateStatus();
	void OnActiveLineChanged(AssDialogue *line);
	void OnExternalCommit(int commit_type);
	void OnAddFiles(wxCommandEvent&);
	void OnRemoveFile(wxCommandEvent&);
	void OnClearFiles(wxCommandEvent&);
	void OnOptionsChanged(wxCommandEvent&);
	void OnListActivated(wxListEvent&);
	void OnReplace(wxCommandEvent&);
	void OnInsert(wxCommandEvent&);
	void OnCopy(wxCommandEvent&);
};

DialogLocalization::Impl::Impl(DialogLocalization *dialog, agi::Context *c)
: dialog(dialog)
, c(c)
, active_line_connection(c->selectionController->AddActiveLineListener(
	&Impl::OnActiveLineChanged, this))
, commit_connection(c->ass->AddCommitListener(&Impl::OnExternalCommit, this))
, active_line(c->selectionController->GetActiveLine())
{
	auto main_sizer = new wxBoxSizer(wxVERTICAL);

	auto original_box = new wxStaticBoxSizer(wxVERTICAL, dialog, _("Current line"));
	// ~25% narrower than the original 560 px so the dialog stays compact.
	original_text = new wxTextCtrl(dialog, -1, "", wxDefaultPosition, wxSize(420, 70),
		wxTE_MULTILINE | wxTE_READONLY);
	original_box->Add(original_text, 1, wxEXPAND | wxALL, 4);
	main_sizer->Add(original_box, 0, wxEXPAND | wxALL, 5);

	auto options_box = new wxStaticBoxSizer(wxVERTICAL, dialog, _("Match options"));
	// Row 1: boolean match toggles only, so the row is short enough for a
	// narrower window than the original single wide options row.
	fuzzy_check = new wxCheckBox(dialog, -1, _("Fuzzy match"));
	tags_check = new wxCheckBox(dialog, -1, _("Ignore text tags"));
	punct_check = new wxCheckBox(dialog, -1, _("Ignore punctuation"));
	case_check = new wxCheckBox(dialog, -1, _("Ignore case"));
	auto flags_row1 = new wxBoxSizer(wxHORIZONTAL);
	flags_row1->Add(fuzzy_check, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
	flags_row1->Add(tags_check, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
	flags_row1->AddStretchSpacer();
	options_box->Add(flags_row1, 0, wxEXPAND | wxALL, 4);
	auto flags_row2 = new wxBoxSizer(wxHORIZONTAL);
	flags_row2->Add(punct_check, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
	flags_row2->Add(case_check, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
	flags_row2->AddStretchSpacer();
	options_box->Add(flags_row2, 0, wxEXPAND | wxALL, 4);

	// Row 2: similarity threshold and preferred language.
	auto scoring_sizer = new wxBoxSizer(wxHORIZONTAL);
	scoring_sizer->Add(new wxStaticText(dialog, -1, _("Minimum similarity:")),
		0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
	threshold_spin = new wxSpinCtrlDouble(dialog, -1, "", wxDefaultPosition, wxSize(70, -1),
		wxSP_ARROW_KEYS, 0.5, 1.0, 0.7, 0.05);
	scoring_sizer->Add(threshold_spin, 0, wxALIGN_CENTER_VERTICAL);
	scoring_sizer->AddSpacer(12);
	scoring_sizer->Add(new wxStaticText(dialog, -1, _("Preferred language:")),
		0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
	language_combo = new wxComboBox(dialog, -1, "", wxDefaultPosition, wxSize(110, -1),
		wxArrayString(), wxCB_READONLY);
	language_combo->Append(wxS("中文"));
	language_combo->Append(wxS("English"));
	language_combo->Append(wxS("日本語"));
	language_combo->Append(wxS("한국어"));
	language_combo->Append(wxS("Русский"));
	scoring_sizer->Add(language_combo, 0, wxALIGN_CENTER_VERTICAL);
	scoring_sizer->AddStretchSpacer();
	options_box->Add(scoring_sizer, 0, wxEXPAND | wxALL, 4);

	// Row 3: sentence and regex splitting.
	auto options_sizer2 = new wxBoxSizer(wxHORIZONTAL);
	sentence_check = new wxCheckBox(dialog, -1, _("Split by sentences"));
	sentence_check->SetToolTip(_("Split segments at sentence endings (。.!?…). Turn this off to keep whole entries as single segments."));
	regex_ctrl = new wxTextCtrl(dialog, -1, "", wxDefaultPosition, wxDefaultSize);
	regex_ctrl->SetToolTip(_("Split segments wherever this regular expression matches; the matched text is removed. For example, use \\{[^}]*\\} to break at text tags such as {*1} or {TA7}. Invalid patterns are ignored."));
	options_sizer2->Add(sentence_check, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 12);
	options_sizer2->Add(new wxStaticText(dialog, -1, _("Split regex:")),
		0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
	options_sizer2->Add(regex_ctrl, 1, wxALIGN_CENTER_VERTICAL);
	options_box->Add(options_sizer2, 0, wxEXPAND | wxALL, 4);

	// Row 4: clean result preference (only has an effect while a split regex
	// is configured).
	clean_check = new wxCheckBox(dialog, -1,
		_("Prefer results without split-regex text"));
	clean_check->SetToolTip(_(
		"When a split regex is set, list results whose localized text contains "
		"no match of that regex first, so cleaned segments are preferred over "
		"results that still carry the unsplit tag text."));
	options_box->Add(clean_check, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
	main_sizer->Add(options_box, 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

	auto files_box_sizer = new wxStaticBoxSizer(wxVERTICAL, dialog, _("Localization files"));
	files_box = new wxListBox(dialog, -1, wxDefaultPosition, wxSize(-1, 90));
	auto file_buttons = new wxBoxSizer(wxHORIZONTAL);
	auto add_button = new wxButton(dialog, -1, _("Add Files..."));
	auto remove_button = new wxButton(dialog, -1, _("Remove"));
	auto clear_button = new wxButton(dialog, -1, _("Clear"));
	file_buttons->Add(add_button, 0, wxRIGHT, 4);
	file_buttons->Add(remove_button, 0, wxRIGHT, 4);
	file_buttons->Add(clear_button, 0);
	files_box_sizer->Add(files_box, 1, wxEXPAND | wxALL, 4);
	files_box_sizer->Add(file_buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
	main_sizer->Add(files_box_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

	auto results_box = new wxStaticBoxSizer(wxVERTICAL, dialog, _("Results"));
	results_list = new wxListCtrl(dialog, -1, wxDefaultPosition, wxSize(-1, 200),
		wxLC_REPORT | wxLC_SINGLE_SEL);
	// Column widths are ~25% smaller than the original 60/150/180/220/200 so
	// the whole table (and therefore the dialog) does not need a wide window.
	results_list->InsertColumn(0, _("Match"), wxLIST_FORMAT_RIGHT, 50);
	results_list->InsertColumn(1, _("Source"), wxLIST_FORMAT_LEFT, 110);
	results_list->InsertColumn(2, _("Matched text"), wxLIST_FORMAT_LEFT, 135);
	results_list->InsertColumn(3, _("Localized text"), wxLIST_FORMAT_LEFT, 165);
	results_list->InsertColumn(4, _("Origin"), wxLIST_FORMAT_LEFT, 150);
	results_list->SetMinSize(wxSize(430, 200));
	results_box->Add(results_list, 1, wxEXPAND | wxALL, 4);
	main_sizer->Add(results_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

	auto button_sizer = new wxBoxSizer(wxHORIZONTAL);
	replace_button = new wxButton(dialog, -1, _("Replace Line"));
	insert_button = new wxButton(dialog, -1, _("Insert at Caret"));
	copy_button = new wxButton(dialog, -1, _("Copy"));
	status = new wxStaticText(dialog, -1, "");
	button_sizer->Add(replace_button, 0, wxRIGHT, 4);
	button_sizer->Add(insert_button, 0, wxRIGHT, 4);
	button_sizer->Add(copy_button, 0);
	button_sizer->AddStretchSpacer();
	button_sizer->Add(status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	button_sizer->Add(new wxButton(dialog, wxID_CANCEL, _("&Close")), 0);
	main_sizer->Add(button_sizer, 0, wxEXPAND | wxALL, 5);

	dialog->SetSizerAndFit(main_sizer);
	dialog->Layout();
	// The options are laid out on short rows now, so the fitted size is
	// already well below the original one-row layout. Cap it at a compact
	// width, then open about 30% wider than that so the results table has
	// room to breathe. The minimum stays below the default so the user can
	// still drag the window narrower by hand.
	const int fit_width = dialog->GetSize().GetWidth();
	const int fit_height = dialog->GetSize().GetHeight();
	const int compact_width = std::min(fit_width, 640);
	const int default_width = compact_width * 13 / 10;
	const int min_width = std::max(420, compact_width - 140);
	dialog->SetSize(default_width, fit_height);
	dialog->SetMinSize(wxSize(min_width, fit_height));
	dialog->CenterOnParent();

	persist = agi::make_unique<PersistLocation>(dialog, "Tool/Localization");

	fuzzy_check->SetValue(OPT_GET("Tool/Localization/Fuzzy")->GetBool());
	tags_check->SetValue(OPT_GET("Tool/Localization/Ignore Tags")->GetBool());
	punct_check->SetValue(OPT_GET("Tool/Localization/Ignore Punctuation")->GetBool());
	case_check->SetValue(OPT_GET("Tool/Localization/Ignore Case")->GetBool());
	threshold_spin->SetValue(OPT_GET("Tool/Localization/Threshold")->GetDouble());
	language_combo->SetValue(to_wx(OPT_GET("Tool/Localization/Language")->GetString()));
	sentence_check->SetValue(OPT_GET("Tool/Localization/Split Sentences")->GetBool());
	regex_ctrl->SetValue(to_wx(OPT_GET("Tool/Localization/Split Regex")->GetString()));
	clean_check->SetValue(OPT_GET("Tool/Localization/Prefer Without Split Regex")->GetBool());

	add_button->Bind(wxEVT_BUTTON, &Impl::OnAddFiles, this);
	remove_button->Bind(wxEVT_BUTTON, &Impl::OnRemoveFile, this);
	clear_button->Bind(wxEVT_BUTTON, &Impl::OnClearFiles, this);
	replace_button->Bind(wxEVT_BUTTON, &Impl::OnReplace, this);
	insert_button->Bind(wxEVT_BUTTON, &Impl::OnInsert, this);
	copy_button->Bind(wxEVT_BUTTON, &Impl::OnCopy, this);
	fuzzy_check->Bind(wxEVT_CHECKBOX, &Impl::OnOptionsChanged, this);
	tags_check->Bind(wxEVT_CHECKBOX, &Impl::OnOptionsChanged, this);
	punct_check->Bind(wxEVT_CHECKBOX, &Impl::OnOptionsChanged, this);
	case_check->Bind(wxEVT_CHECKBOX, &Impl::OnOptionsChanged, this);
	threshold_spin->Bind(wxEVT_SPINCTRLDOUBLE, &Impl::OnOptionsChanged, this);
	language_combo->Bind(wxEVT_COMBOBOX, &Impl::OnOptionsChanged, this);
	sentence_check->Bind(wxEVT_CHECKBOX, &Impl::OnOptionsChanged, this);
	regex_ctrl->Bind(wxEVT_TEXT, &Impl::OnOptionsChanged, this);
	clean_check->Bind(wxEVT_CHECKBOX, &Impl::OnOptionsChanged, this);
	results_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &Impl::OnListActivated, this);

	LoadPersistedFiles();
	LoadFiles();
	UpdateDisplay();
}

localization::MatchOptions DialogLocalization::Impl::CurrentOptions() const {
	localization::MatchOptions options;
	options.fuzzy = fuzzy_check->GetValue();
	options.ignore_tags = tags_check->GetValue();
	options.ignore_punctuation = punct_check->GetValue();
	options.ignore_case = case_check->GetValue();
	options.threshold = threshold_spin->GetValue();
	options.preferred_language = from_wx(language_combo->GetValue());
	options.split_sentences = sentence_check->GetValue();
	options.split_regex = from_wx(regex_ctrl->GetValue());
	options.prefer_without_split_regex = clean_check->GetValue();
	return options;
}

void DialogLocalization::Impl::SaveOptions() {
	OPT_SET("Tool/Localization/Fuzzy")->SetBool(fuzzy_check->GetValue());
	OPT_SET("Tool/Localization/Ignore Tags")->SetBool(tags_check->GetValue());
	OPT_SET("Tool/Localization/Ignore Punctuation")->SetBool(punct_check->GetValue());
	OPT_SET("Tool/Localization/Ignore Case")->SetBool(case_check->GetValue());
	OPT_SET("Tool/Localization/Threshold")->SetDouble(threshold_spin->GetValue());
	OPT_SET("Tool/Localization/Language")->SetString(from_wx(language_combo->GetValue()));
	OPT_SET("Tool/Localization/Split Sentences")->SetBool(sentence_check->GetValue());
	OPT_SET("Tool/Localization/Split Regex")->SetString(from_wx(regex_ctrl->GetValue()));
	OPT_SET("Tool/Localization/Prefer Without Split Regex")->SetBool(clean_check->GetValue());

	std::string joined;
	for (auto const& path : paths) {
		if (!joined.empty()) joined += "\n";
		joined += PathUtf8(path);
	}
	OPT_SET("Tool/Localization/Files")->SetString(joined);
}

void DialogLocalization::Impl::LoadPersistedFiles() {
	std::string saved = OPT_GET("Tool/Localization/Files")->GetString();
	size_t start = 0;
	while (start <= saved.size()) {
		size_t end = saved.find('\n', start);
		if (end == std::string::npos) end = saved.size();
		std::string item = saved.substr(start, end - start);
		if (!item.empty()) {
			wxString wide = to_wx(item);
			agi::fs::path path(std::wstring(wide.wc_str()));
			if (agi::fs::FileExists(path)) paths.push_back(path);
		}
		if (end == saved.size()) break;
		start = end + 1;
	}
}

void DialogLocalization::Impl::LoadFiles() {
	files.clear();
	for (auto const& path : paths) {
		localization::LocalizationFile file;
		file.name = PathUtf8(path.filename());
		std::string error;
		std::string content = ReadLocalizationFile(path, error);
		if (!error.empty()) {
			file.ok = false;
			file.error = error;
		}
		else {
			file = localization::LoadContent(content, file.name, PathUtf8(path.extension()));
		}
		files.push_back(std::move(file));
	}
	UpdateFilesBox();
	RefreshMatch();
}

void DialogLocalization::Impl::UpdateFilesBox() {
	files_box->Clear();
	for (size_t i = 0; i < paths.size(); ++i) {
		wxString label = to_wx(PathUtf8(paths[i]));
		if (i < files.size() && !files[i].ok)
			label += _("  [failed to load]");
		files_box->Append(label);
	}
}

void DialogLocalization::Impl::OnActiveLineChanged(AssDialogue *line) {
	active_line = line;
	UpdateDisplay();
}

void DialogLocalization::Impl::OnExternalCommit(int commit_type) {
	if (commit_type & (AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_ADDREM))
		UpdateDisplay();
}

void DialogLocalization::Impl::UpdateDisplay() {
	AssDialogue *line = c->selectionController->GetActiveLine();
	active_line = line;

	if (line)
		original_text->SetValue(to_wx(line->GetStrippedText()));
	else
		original_text->SetValue(_("No active subtitle line."));

	RefreshMatch();
}

void DialogLocalization::Impl::RefreshMatch() {
	if (updating) return;
	updating = true;

	results_list->DeleteAllItems();
	results.clear();

	if (active_line) {
		results = localization::Match(active_line->GetStrippedText(), files, CurrentOptions());
		for (size_t i = 0; i < results.size(); ++i) {
			auto const& r = results[i];
			long row = results_list->GetItemCount();
			results_list->InsertItem(row,
				wxString::Format("%d%%", static_cast<int>(std::lround(r.score * 100.0))));
			results_list->SetItem(row, 1, to_wx(
				r.file + (r.key.empty() ? "" : " [" + r.key + "]")));
			results_list->SetItem(row, 2, to_wx(r.from_key ? r.key : r.matched));
			results_list->SetItem(row, 3, to_wx(r.replacement));
			results_list->SetItem(row, 4, to_wx(r.origin));
		}
	}

	bool has_result = !results.empty();
	replace_button->Enable(has_result);
	insert_button->Enable(has_result);
	copy_button->Enable(has_result);
	UpdateStatus();

	updating = false;
}

void DialogLocalization::Impl::UpdateStatus() {
	size_t entry_count = 0;
	size_t error_count = 0;
	for (auto const& file : files) {
		entry_count += file.items.size();
		if (!file.ok) ++error_count;
	}
	wxString text = fmt_tl("%d matches from %d files (%d entries)",
		results.size(), files.size(), entry_count);
	if (error_count > 0)
		text += fmt_tl(", %d file(s) failed to load", error_count);
	status->SetLabel(text);
}

void DialogLocalization::Impl::OnAddFiles(wxCommandEvent&) {
	wxFileDialog diag(dialog, _("Choose localization files"), "", "",
		"Game localization files (*.json;*.ini;*.txt;*.csv;*.tsv;*.dat;*.cfg)|*.json;*.ini;*.txt;*.csv;*.tsv;*.dat;*.cfg|All files (*.*)|*.*",
		wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
	if (diag.ShowModal() == wxID_CANCEL) return;

	wxArrayString chosen;
	diag.GetPaths(chosen);
	for (auto const& path : chosen) {
		agi::fs::path p(std::wstring(path.wc_str()));
		bool exists = false;
		for (auto const& existing : paths)
			if (existing == p) exists = true;
		if (!exists) paths.push_back(p);
	}
	SaveOptions();
	LoadFiles();
}

void DialogLocalization::Impl::OnRemoveFile(wxCommandEvent&) {
	int sel = files_box->GetSelection();
	if (sel == wxNOT_FOUND || sel >= static_cast<int>(paths.size())) return;
	paths.erase(paths.begin() + sel);
	SaveOptions();
	LoadFiles();
}

void DialogLocalization::Impl::OnClearFiles(wxCommandEvent&) {
	if (paths.empty()) return;
	paths.clear();
	SaveOptions();
	LoadFiles();
}

void DialogLocalization::Impl::OnOptionsChanged(wxCommandEvent&) {
	SaveOptions();
	RefreshMatch();
}

void DialogLocalization::Impl::OnListActivated(wxListEvent&) {
	wxCommandEvent evt;
	OnReplace(evt);
}

void DialogLocalization::Impl::OnReplace(wxCommandEvent&) {
	long sel = results_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (sel == -1) {
		wxMessageBox(_("Select a result first."), _("Localization Match"),
			wxOK | wxICON_INFORMATION | wxCENTER, dialog);
		return;
	}

	AssDialogue *line = c->selectionController->GetActiveLine();
	if (!line) {
		wxMessageBox(_("No active subtitle line is selected."), _("Localization Match"),
			wxOK | wxICON_ERROR | wxCENTER, dialog);
		return;
	}

	line->Text = AssEscapeNewlines(results[static_cast<size_t>(sel)].replacement);
	c->ass->Commit(_("replace line with localization result"),
		AssFile::COMMIT_DIAG_TEXT, -1, line);
}

void DialogLocalization::Impl::OnInsert(wxCommandEvent&) {
	long sel = results_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (sel == -1) return;

	std::string replacement = results[static_cast<size_t>(sel)].replacement;
	if (!c->subsEditBox) return;

	if (c->subsEditBox->BetterViewEnabled())
		c->subsEditBox->InsertTextAtCaret(to_wx(replacement));
	else
		c->subsEditBox->InsertTextAtCaret(to_wx(AssEscapeNewlines(replacement)));
}

void DialogLocalization::Impl::OnCopy(wxCommandEvent&) {
	long sel = results_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (sel == -1) return;
	SetClipboard(results[static_cast<size_t>(sel)].replacement);
}

DialogLocalization::DialogLocalization(agi::Context *context)
: wxDialog(context->parent, -1, _("Localization Match"), wxDefaultPosition,
	wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, impl(new Impl(this, context))
{
}

DialogLocalization::~DialogLocalization() {
	impl->SaveOptions();
	delete impl;
}

void ShowLocalizationDialog(agi::Context *c) {
	c->dialog->Show<DialogLocalization>(c);
}
