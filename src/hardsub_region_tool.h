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

#include "visual_tool.h"

#include <functional>
#include <wx/gdicmn.h>

namespace agi { struct Context; }

/// Visual tool which lets the user drag a rectangle over the video to mark the
/// hard subtitle region. The selection is reported in video pixel coordinates
/// together with the frame it was drawn on.
class HardSubRegionTool final : public VisualToolBase {
	std::function<void(wxRect const& video_region, int frame)> on_selected;

	wxRect region;       ///< Selected region in video pixel coordinates
	bool has_region = false;
	bool dragging = false;
	Vector2D drag_start;
	Vector2D drag_current;

	/// Convert a point in video pixel coordinates to display coordinates
	Vector2D ToDisplay(Vector2D px) const;
	/// Convert a display-coordinate rectangle to video pixel coordinates
	wxRect ToFrameRect(Vector2D a, Vector2D b) const;
	/// Current drag rectangle in display coordinates
	wxRect DisplayDragRect() const;

public:
	HardSubRegionTool(VideoDisplay *parent, agi::Context *context,
	                  std::function<void(wxRect const&, int)> on_selected,
	                  wxRect const& initial_region = wxRect());
	~HardSubRegionTool();

	void OnMouseEvent(wxMouseEvent& event) override;
	void Draw() override;
	bool OnKeyEvent(wxKeyEvent& event) override;
	void OnAttached() override;
	void OnMouseCaptureLost(wxMouseCaptureLostEvent&) override;
};
