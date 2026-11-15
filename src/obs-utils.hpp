#pragma once

#include <obs.h>
#include <cmath>
#include <cstdint>

struct VirtuEditCanvasSize {
	uint32_t width = 1920;
	uint32_t height = 1080;
};

inline VirtuEditCanvasSize virtuedit_get_canvas_size()
{
	VirtuEditCanvasSize size;
	struct obs_video_info ovi {};
	if (obs_get_video_info(&ovi)) {
		if (ovi.base_width > 0)
			size.width = ovi.base_width;
		if (ovi.base_height > 0)
			size.height = ovi.base_height;
	}
	return size;
}

inline VirtuEditCanvasSize virtuedit_get_output_size()
{
	VirtuEditCanvasSize size{1280, 720};
	struct obs_video_info ovi {};
	if (obs_get_video_info(&ovi)) {
		if (ovi.output_width > 0)
			size.width = ovi.output_width;
		if (ovi.output_height > 0)
			size.height = ovi.output_height;
	}
	return size;
}

inline bool virtuedit_is_near_16x9(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return false;

	const double ratio = static_cast<double>(width) /
			     static_cast<double>(height);
	return std::fabs(ratio - (16.0 / 9.0)) <= 0.015;
}

inline VirtuEditCanvasSize virtuedit_get_16x9_player_size()
{
	VirtuEditCanvasSize canvas = virtuedit_get_canvas_size();
	VirtuEditCanvasSize size;
	size.width = canvas.width;
	size.height = static_cast<uint32_t>((canvas.width * 9u + 8u) / 16u);
	if (size.height == 0)
		size.height = 1;
	return size;
}
