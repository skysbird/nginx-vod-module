#ifndef __video_DECODER_H__
#define __video_DECODER_H__

// includes
#include "../media_format.h"
#include <libavcodec/avcodec.h>

// macros
#define video_decoder_has_frame(decoder) \
	((decoder)->cur_frame < (decoder)->cur_frame_part.last_frame)

// typedefs
typedef struct {
	request_context_t* request_context;
	AVCodecContext* decoder;
	AVFrame* decoded_frame;

	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	uint64_t dts;

	u_char* frame_buffer;
	uint32_t max_frame_size;
	uint32_t cur_frame_pos;
	bool_t data_handled;
	bool_t frame_started;
	vod_str_t extra_data;
	
} video_decoder_state_t;

// functions
void video_decoder_process_init(vod_log_t* log);

vod_status_t video_decoder_init(
	video_decoder_state_t* state,
	request_context_t* request_context,
	media_track_t* track,
	int cache_slot_id);

void video_decoder_free(video_decoder_state_t* state);

vod_status_t video_decoder_get_frame(
	video_decoder_state_t* state,
	AVFrame** result);

#endif // __video_DECODER_H__
