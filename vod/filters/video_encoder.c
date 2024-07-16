#include "video_encoder.h"
#include "video_filter.h"

// constants
#define video_ENCODER_BITS_PER_SAMPLE (16)

// typedefs
typedef struct
{
	request_context_t* request_context;
	vod_array_t* frames_array;
	AVCodecContext *encoder;
} video_encoder_state_t;

// globals
static const AVCodec *encoder_codec = NULL;
static bool_t initialized = FALSE;

static char* aac_encoder_names[] = {
	"libfdk_aac",
	"aac",
	NULL
};


static bool_t
video_encoder_is_format_supported(const AVCodec *codec, enum AVSampleFormat sample_fmt)
{
	const enum AVSampleFormat *p;

	for (p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++)
	{
		if (*p == sample_fmt)
		{
			return TRUE;
		}
	}

	return FALSE;
}

void
video_encoder_process_init(vod_log_t* log)
{
	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
        avcodec_register_all();
    #endif

    encoder_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder_codec) {
        vod_log_error(VOD_LOG_WARN, log, 0,
            "video_encoder_process_init: failed to find H.264 encoder, video encoding is disabled.");
        return;
    }

    initialized = TRUE;
}

vod_status_t
video_encoder_init(
	request_context_t* request_context,
	video_encoder_params_t* params,
	vod_array_t* frames_array,
	void** result)
{
	video_encoder_state_t* state;
	AVCodecContext* encoder;
	int avrc;

	if (!initialized)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"video_encoder_init: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"video_encoder_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// init the encoder
	encoder = avcodec_alloc_context3(encoder_codec);
	if (encoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"video_encoder_init: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	state->encoder = encoder;

	encoder->gop_size = 1; 
	encoder->has_b_frames = 0;
	encoder->max_b_frames = 0;

	encoder->height = params->height;
	encoder->width = params->width;
	encoder->sample_aspect_ratio.num = 0;
	encoder->sample_aspect_ratio.den = 1;
	encoder->codec_id = encoder_codec->id;
	encoder->pix_fmt = *(encoder_codec->pix_fmts);
	encoder->time_base.num = 1;
	encoder->time_base.den = 25600;
	// encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;		// make the codec generate the extra data

// #if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
// 	av_channel_layout_from_mask(&encoder->ch_layout, params->channel_layout);
// #else
// 	encoder->channels = params->channels;
// 	encoder->channel_layout = params->channel_layout;
// #endif

// 	encoder->bit_rate = params->bitrate;
// 	encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;		// make the codec generate the extra data

	avrc = avcodec_open2(encoder, encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"video_encoder_init: avcodec_open2 failed %d", avrc);
		video_encoder_free(state);
		return VOD_UNEXPECTED;
	}

	state->request_context = request_context;
	state->frames_array = frames_array;

	*result = state;

	return VOD_OK;
}

void
video_encoder_free(
	void* context)
{
	video_encoder_state_t* state = context;

	if (state == NULL)
	{
		return;
	}
	
	avcodec_close(state->encoder);
	av_free(state->encoder);
}

size_t
video_encoder_get_frame_size(void* context)
{
	video_encoder_state_t* state = context;

	if ((state->encoder->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0)
	{
		return 0;
	}

	return state->encoder->frame_size;
}

static vod_status_t
video_encoder_write_packet(
	video_encoder_state_t* state,
	AVPacket* output_packet)
{
	input_frame_t* cur_frame;
	vod_status_t rc;
	void* data;

	rc = video_filter_alloc_memory_frame(
		state->request_context,
		state->frames_array,
		output_packet->size,
		&cur_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	data = (void*)(uintptr_t)cur_frame->offset;
	vod_memcpy(data, output_packet->data, output_packet->size);

	cur_frame->duration = output_packet->duration;
	cur_frame->pts_delay = output_packet->pts - output_packet->dts;

	return VOD_OK;
}



static void save_to_file(u_char* buffer, int size, const char* filename) {
    FILE* file = fopen(filename, "wb");
    if (file == NULL) {
        perror("Error opening file");
        return;
    }
    size_t written = fwrite(buffer, 1, size, file);
    if (written != size) {
        perror("Error writing to file");
    }
    fclose(file);
}

// Save AVFrame to YUV file
void save_frame_to_yuv(AVFrame *frame, const char *filename) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Could not open %s\n", filename);
        return;
    }

    // Write Y plane
    for (int y = 0; y < frame->height; y++) {
        fwrite(frame->data[0] + y * frame->linesize[0], 1, frame->width, file);
    }

    // Write U plane
    for (int y = 0; y < frame->height / 2; y++) {
        fwrite(frame->data[1] + y * frame->linesize[1], 1, frame->width / 2, file);
    }

    // Write V plane
    for (int y = 0; y < frame->height / 2; y++) {
        fwrite(frame->data[2] + y * frame->linesize[2], 1, frame->width / 2, file);
    }

    fclose(file);
}

vod_status_t
video_encoder_write_frame(
	void* context,
	AVFrame* frame)
{
	video_encoder_state_t* state = context;
	vod_status_t rc;
	AVPacket* output_packet;
	int avrc;

	// save_frame_to_yuv(frame,"/tmp/en.h264");

	// send frame
	avrc = avcodec_send_frame(state->encoder, frame);


	av_frame_unref(frame);


	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_encoder_write_frame: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	// receive packet
	output_packet = av_packet_alloc();
	if (output_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_encoder_write_frame: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	// packet data will be allocated by the encoder

	avrc = avcodec_receive_packet(state->encoder, output_packet);

	if (avrc == AVERROR(EAGAIN))
	{
		av_packet_free(&output_packet);
		return VOD_OK;
	}

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_encoder_write_frame: avcodec_receive_packet failed %d", avrc);
		av_packet_free(&output_packet);
		return VOD_ALLOC_FAILED;
	}

	save_to_file(output_packet->data, output_packet->size, "/tmp/en.h264");

	rc = video_encoder_write_packet(state, output_packet);

	av_packet_free(&output_packet);

	return rc;
}

vod_status_t
video_encoder_flush(
	void* context)
{
	video_encoder_state_t* state = context;
	AVPacket* output_packet;
	vod_status_t rc;
	int avrc;

	avrc = avcodec_send_frame(state->encoder, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_encoder_flush: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	output_packet = av_packet_alloc();
	if (output_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_encoder_flush: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	for (;;)
	{
		// packet data will be allocated by the encoder, av_packet_unref is always called
		avrc = avcodec_receive_packet(state->encoder, output_packet);
		if (avrc == AVERROR_EOF)
		{
			break;
		}

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"video_encoder_flush: avcodec_receive_packet failed %d", avrc);
			av_packet_free(&output_packet);
			return VOD_UNEXPECTED;
		}

		rc = video_encoder_write_packet(state, output_packet);

		if (rc != VOD_OK)
		{
			av_packet_free(&output_packet);
			return rc;
		}
	}

	av_packet_free(&output_packet);
	return VOD_OK;
}

vod_status_t
video_encoder_update_media_info(
	void* context,
	media_info_t* media_info)
{
	video_encoder_state_t* state = context;
	AVCodecContext *encoder = state->encoder;
	u_char* new_extra_data;

	if (encoder->time_base.num != 1)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_encoder_update_media_info: unexpected encoder time base %d/%d",
			encoder->time_base.num, encoder->time_base.den);
		return VOD_UNEXPECTED;
	}

	media_info->timescale = encoder->time_base.den;
	media_info->bitrate = encoder->bit_rate;

	media_info->u.audio.object_type_id = 0x40;		// ffmpeg always writes 0x40 (ff_mp4_obj_type)

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	media_info->u.audio.channels = encoder->ch_layout.nb_channels;
	media_info->u.audio.channel_layout = encoder->ch_layout.u.mask;
#else
	media_info->u.audio.channels = encoder->channels;
	media_info->u.audio.channel_layout = encoder->channel_layout;
#endif

	media_info->u.audio.bits_per_sample = video_ENCODER_BITS_PER_SAMPLE;
	media_info->u.audio.packet_size = 0;			// ffmpeg always writes 0 (mov_write_video_tag)
	media_info->u.audio.sample_rate = encoder->sample_rate;

	new_extra_data = vod_alloc(state->request_context->pool, encoder->extradata_size);
	if (new_extra_data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"video_encoder_update_media_info: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memcpy(new_extra_data, encoder->extradata, encoder->extradata_size);

	media_info->extra_data.data = new_extra_data;
	media_info->extra_data.len = encoder->extradata_size;

	return VOD_OK;
}
