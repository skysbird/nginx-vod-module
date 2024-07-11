#ifndef __FRAME_WATERMARK_FILTER_H__
#define __FRAME_WATERMARK_FILTER_H__

// include
#include "media_filter.h"

// functions
vod_status_t frame_watermark_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context
	);

void frame_watermark_start_sub_frame(
	media_filter_context_t* context,
	uint32_t size);

#endif // __FRAME_WATERMARK_FILTER_H__
