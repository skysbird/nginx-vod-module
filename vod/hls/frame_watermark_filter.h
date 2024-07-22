#ifndef __FRAME_WATERMARK_FILTER_H__
#define __FRAME_WATERMARK_FILTER_H__

// include
#include "media_filter.h"
#include "../media_format.h"


// typedefs
typedef struct
{
	// fixed input data
	media_filter_start_frame_t start_frame;
	media_filter_write_t write;
	output_frame_t* frame;
	AVFrame* decoded_frame;
	AVCodecContext* decoder;
	AVCodecContext* encoder;
	request_context_t* request_context;
	uint32_t cur_offset;
	uint32_t codec_id;
	vod_str_t codec_name;

} frame_watermark_filter_state_t;

// functions
vod_status_t frame_watermark_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context
	);

vod_status_t frame_watermark_filter_init_2(
	media_filter_t* filter,
	media_filter_context_t* context,
	media_info_t* media_info
	);

void frame_watermark_start_sub_frame(
	media_filter_context_t* context,
	uint32_t size);



#endif // __FRAME_WATERMARK_FILTER_H__
