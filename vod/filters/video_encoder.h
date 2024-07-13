#ifndef __video_ENCODER_H__
#define __video_ENCODER_H__

// includes
#include "../media_format.h"
#include <libavcodec/avcodec.h>

// constants
#define video_ENCODER_INPUT_SAMPLE_FORMAT (AV_SAMPLE_FMT_S16)

//typedefs
typedef struct
{
	uint64_t channel_layout;
	uint16_t channels;
	uint32_t sample_rate;
	uint32_t timescale;
	uint32_t bitrate;
} video_encoder_params_t;

// functions
void video_encoder_process_init(
	vod_log_t* log);

vod_status_t video_encoder_init(
	request_context_t* request_context,
	video_encoder_params_t* params,
	vod_array_t* frames_array,
	void** result);

void video_encoder_free(
	void* context);

size_t video_encoder_get_frame_size(
	void* context);

vod_status_t video_encoder_write_frame(
	void* context,
	AVFrame* frame);

vod_status_t video_encoder_flush(
	void* context);

vod_status_t video_encoder_update_media_info(
	void* context,
	media_info_t* media_info);

#endif // __video_ENCODER_H__
