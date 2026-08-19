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

#include "hardsub_region_tool.h"

#include "async_video_provider.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "video_controller.h"
#include "video_display.h"

#include <algorithm>
#include <cmath>
#include <wx/cursor.h>
#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>

namespace {

int RoundCoord(double value) {
	return static_cast<int>(std::lround(value));
}

} // namespace

HardSubRegionTool::HardSubRegionTool(VideoDisplay *parent, agi::Context *context,
                                     std::function<void(wxRect const&, int)> on_selected,
                                     wxRect const& initial_region)
: VisualToolBase(parent, context)
, on_selected(std::move(on_selected))
, region(initial_region)
, has_region(initial_region.GetWidth() > 0 && initial_region.GetHeight() > 0)
{
}

HardSubRegionTool::~HardSubRegionTool() {
	parent->SetCursor(wxNullCursor);
}

void HardSubRegionTool::OnAttached() {
	parent->SetCursor(wxCursor(wxCURSOR_CROSS));
}

void HardSubRegionTool::OnMouseCaptureLost(wxMouseCaptureLostEvent&) {
	dragging = false;
}

Vector2D HardSubRegionTool::ToDisplay(Vector2D px) const {
	auto provider = c->project->VideoProvider();
	if (!provider || video_res.X() <= 0 || video_res.Y() <= 0)
		return Vector2D();

	return video_pos + Vector2D(
		px.X() * video_res.X() / std::max(provider->GetWidth(), 1),
		px.Y() * video_res.Y() / std::max(provider->GetHeight(), 1));
}

wxRect HardSubRegionTool::ToFrameRect(Vector2D a, Vector2D b) const {
	auto provider = c->project->VideoProvider();
	if (!provider || video_res.X() <= 0 || video_res.Y() <= 0)
		return wxRect();

	auto to_px = [&](Vector2D p) {
		Vector2D rel = p - video_pos;
		return Vector2D(
			rel.X() * provider->GetWidth() / video_res.X(),
			rel.Y() * provider->GetHeight() / video_res.Y());
	};

	Vector2D pa = to_px(a);
	Vector2D pb = to_px(b);
	int x = RoundCoord(std::min(pa.X(), pb.X()));
	int y = RoundCoord(std::min(pa.Y(), pb.Y()));
	int x2 = RoundCoord(std::max(pa.X(), pb.X()));
	int y2 = RoundCoord(std::max(pa.Y(), pb.Y()));

	x = std::clamp(x, 0, std::max(provider->GetWidth() - 1, 0));
	y = std::clamp(y, 0, std::max(provider->GetHeight() - 1, 0));
	x2 = std::clamp(x2, x, std::max(provider->GetWidth(), x + 1));
	y2 = std::clamp(y2, y, std::max(provider->GetHeight(), y + 1));
	return wxRect(x, y, x2 - x, y2 - y);
}

wxRect HardSubRegionTool::DisplayDragRect() const {
	int x = RoundCoord(std::min(drag_start.X(), drag_current.X()));
	int y = RoundCoord(std::min(drag_start.Y(), drag_current.Y()));
	int x2 = RoundCoord(std::max(drag_start.X(), drag_current.X()));
	int y2 = RoundCoord(std::max(drag_start.Y(), drag_current.Y()));
	return wxRect(x, y, x2 - x, y2 - y);
}

void HardSubRegionTool::OnMouseEvent(wxMouseEvent& event) {
	mouse_pos = event.GetPosition();

	if (event.Leaving()) {
		mouse_pos = Vector2D();
		parent->Render();
		return;
	}

	if (event.LeftDown()) {
		dragging = true;
		drag_start = drag_current = mouse_pos;
		parent->SetFocus();
		parent->CaptureMouse();
	}
	else if (dragging && event.Dragging()) {
		drag_current = mouse_pos;
	}
	else if (dragging && event.LeftUp()) {
		dragging = false;
		parent->ReleaseMouse();

		wxRect display_rect = DisplayDragRect();
		if (display_rect.GetWidth() >= 4 && display_rect.GetHeight() >= 4) {
			wxRect frame_rect = ToFrameRect(
				Vector2D(display_rect.GetX(), display_rect.GetY()),
				Vector2D(display_rect.GetX() + display_rect.GetWidth(), display_rect.GetY() + display_rect.GetHeight()));
			if (frame_rect.GetWidth() > 0 && frame_rect.GetHeight() > 0) {
				region = frame_rect;
				has_region = true;
				if (on_selected)
					on_selected(frame_rect, c->videoController->GetFrameN());
			}
		}
	}

	parent->Render();
}

void HardSubRegionTool::Draw() {
	if (video_res.X() <= 0 || video_res.Y() <= 0)
		return;

	wxRect rect;
	if (dragging) {
		rect = DisplayDragRect();
	}
	else if (has_region) {
		Vector2D p1 = ToDisplay(Vector2D(region.GetX(), region.GetY()));
		Vector2D p2 = ToDisplay(Vector2D(region.GetX() + region.GetWidth(), region.GetY() + region.GetHeight()));
		float left = std::min(p1.X(), p2.X());
		float top = std::min(p1.Y(), p2.Y());
		float right = std::max(p1.X(), p2.X());
		float bottom = std::max(p1.Y(), p2.Y());
		rect = wxRect(RoundCoord(left), RoundCoord(top),
		              RoundCoord(right - left), RoundCoord(bottom - top));
	}

	if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0)
		return;

	Vector2D p1(rect.GetX(), rect.GetY());
	Vector2D p2(rect.GetX() + rect.GetWidth(), rect.GetY() + rect.GetHeight());

	gl.SetFillColour(wxColour(255, 215, 0), 0.22f);
	gl.SetModeFill();
	gl.DrawRectangle(p1, p2);

	gl.SetLineColour(*wxWHITE, 1.0f, 2);
	gl.SetModeLine();
	gl.DrawRectangle(p1, p2);
}

bool HardSubRegionTool::OnKeyEvent(wxKeyEvent& event) {
	if (event.GetKeyCode() == WXK_ESCAPE && dragging) {
		dragging = false;
		parent->ReleaseMouse();
		parent->Render();
		return true;
	}
	return false;
}
