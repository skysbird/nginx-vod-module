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
	int last_pts;
	media_info_t *output_media_info;
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
	// state->media_info = media_info;

	encoder->gop_size = 30; 
	// encoder->has_b_frames = 0;
	// encoder->max_b_frames = 0;

	encoder->height = params->media_info->u.video.height;
	encoder->width = params->media_info->u.video.width;
	encoder->sample_aspect_ratio.num = 16;
	encoder->sample_aspect_ratio.den = 15;
	encoder->codec_id = encoder_codec->id;
	encoder->pix_fmt = *(encoder_codec->pix_fmts);
	encoder->time_base = params->time_base;
	state->output_media_info = params->media_info;
	// encoder->bit_rate = 400000;
	// encoder->framerate.num = 1;
	// encoder->framerate.den = 48;

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
	state->last_pts = 0;


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

// 写入AVCC格式
static void write_avcc(uint8_t **out_buf, int *out_size, const uint8_t *data, int size) {
    uint32_t nalu_size = htonl(size);
    *out_buf = realloc(*out_buf, *out_size + 4 + size);
    memcpy(*out_buf + *out_size, &nalu_size, 4);
    memcpy(*out_buf + *out_size + 4, data, size);
    *out_size += 4 + size;
}

// 将Annex B格式转换为AVCC格式
static void convert_annexb_to_avcc(const uint8_t *buffer, int buffer_size, uint8_t **out_buf, int *out_size) {
    int i = 0;
    while (i < buffer_size) {
        if (i + 4 > buffer_size) break; // Ensure there is enough data for the start code

        // Find the start code (0x00000001)
        if (buffer[i] == 0x00 && buffer[i + 1] == 0x00 && buffer[i + 2] == 0x00 && buffer[i + 3] == 0x01) {
            int j = i + 4;
            while (j + 4 <= buffer_size && !(buffer[j] == 0x00 && buffer[j + 1] == 0x00 && buffer[j + 2] == 0x00 && buffer[j + 3] == 0x01)) {
                j++;
            }
            int nalu_size = j - (i + 4);
            write_avcc(out_buf, out_size, buffer + i + 4, nalu_size);
            i = j;
        } else {
            i++;
		}
    }
}

static vod_status_t
video_encoder_write_packet(
	video_encoder_state_t* state,
	AVPacket* output_packet)
{
	input_frame_t* cur_frame;
	vod_status_t rc;
	void* data;

	uint8_t *out_buf;
	int out_size;

	//to avcc
	// convert_annexb_to_avcc(output_packet->data, output_packet->size, &out_buf, &out_size);
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


	save_to_file(output_packet->data, output_packet->size, "/tmp/avcc.ts"); //保存非avcc可直接拨
	save_to_file(state->encoder->extradata, state->encoder->extradata_size, "/tmp/extra.bin"); //保存非avcc可直接拨

	cur_frame->duration = output_packet->duration;
	cur_frame->pts_delay = output_packet->pts - output_packet->dts;
	
	return VOD_OK;
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

	// av_frame_unref(frame);


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

	// save_to_file(output_packet->data, output_packet->size, "/tmp/en.h264");

	AVRational t;
	t.num = 1;
	t.den = state->output_media_info->frames_timescale;
	// output_packet->pts = av_rescale_q(output_packet->pts, t, state->encoder->time_base);
	// output_packet->dts = av_rescale_q(output_packet->dts, t, state->encoder->time_base);
	// output_packet->duration = av_rescale_q(output_packet->duration, state->encoder->time_base, t);

	//FIXME 这里duration不对！
	// output_packet->duration = pkt_duration;
	// if (output_packet->duration == 0) {
	// 		output_packet->duration = av_rescale_q(1, (AVRational){1, state->encoder->framerate.num}, state->encoder->time_base);
	// }
	output_packet->duration = frame->pkt_duration;
	// output_packet->duration = av_rescale_q(output_packet->duration, t, state->encoder->time_base);

	rc = video_encoder_write_packet(state, output_packet);
	// state->last_pts = output_packet->pts;

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

	// media_info->timescale = encoder->time_base.den;
	media_info->bitrate = encoder->bit_rate;

	// media_info->u.audio.object_type_id = 0x40;		// ffmpeg always writes 0x40 (ff_mp4_obj_type)

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	// media_info->u.audio.channels = encoder->ch_layout.nb_channels;
	// media_info->u.audio.channel_layout = encoder->ch_layout.u.mask;
#else
	// media_info->u.audio.channels = encoder->channels;
	// media_info->u.audio.channel_layout = encoder->channel_layout;
#endif

	// media_info->u.audio.bits_per_sample = video_ENCODER_BITS_PER_SAMPLE;
	// media_info->u.audio.packet_size = 0;			// ffmpeg always writes 0 (mov_write_video_tag)
	// media_info->u.audio.sample_rate = encoder->sample_rate;

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
