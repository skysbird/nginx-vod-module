#include "frame_watermark_filter.h"
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

// macros
#define THIS_FILTER (MEDIA_FILTER_WATERMARK)
#define get_context(ctx) ((frame_watermark_filter_state_t*)ctx->context[THIS_FILTER])

typedef struct {
    AVFilterContext *buffersrc_ctx;
    AVFilterContext *buffersink_ctx;
    AVFilterGraph *filter_graph;
} FilterContext;

//typedefs
typedef struct
{
	uint64_t channel_layout;
	uint16_t channels;
	uint32_t sample_rate;
	uint32_t timescale;
	uint32_t bitrate;
} video_encoder_params_t;

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

static vod_status_t
frame_watermark_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	frame_watermark_filter_state_t* state = get_context(context);

	state->cur_offset = 0;

	state->frame = frame;

	return state->start_frame(context, frame);
}

void
frame_watermark_start_sub_frame(media_filter_context_t* context, uint32_t size)
{
	frame_watermark_filter_state_t* state = get_context(context);

	state->cur_offset = 0;
}


static vod_status_t
video_decoder_decode_frame(
	frame_watermark_filter_state_t* state,
	u_char* buffer,
	AVFrame** result)
{
	AVPacket* input_packet;
	u_char original_pad[VOD_BUFFER_PADDING_SIZE];
	u_char* frame_end;
	int avrc;

	input_packet = av_packet_alloc();
	if (input_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_decoder_decode_frame: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// send a frame
	input_packet->data = buffer;
	input_packet->size = state->frame->size;
	input_packet->dts = state->frame->dts;
	input_packet->pts = state->frame->pts;
	input_packet->duration = state->frame->duration;
	input_packet->flags = AV_PKT_FLAG_KEY;
	// // state->dts += frame->duration;

	av_frame_unref(state->decoded_frame);

	vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"buffer_size %ld, frame_size %ld, header_size %ld", strlen(buffer), state->frame->size, state->frame->header_size);


	// frame_end = buffer + state->frame->size;
	// vod_memcpy(original_pad, frame_end, sizeof(original_pad));
	// vod_memzero(frame_end, sizeof(original_pad));

	avrc = avcodec_send_packet(state->decoder, input_packet);
	
	av_packet_free(&input_packet);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_decode_frame: avcodec_send_packet failed %s", av_err2str(avrc));
		return VOD_BAD_DATA;
	}

	// // move to the next frame
	// // state->cur_frame++;
	// // if (state->cur_frame >= state->cur_frame_part.last_frame &&
	// // 	state->cur_frame_part.next != NULL)
	// // {
	// // 	state->cur_frame_part = *state->cur_frame_part.next;
	// // 	state->cur_frame = state->cur_frame_part.first_frame;
	// // }

	// // state->frame_started = FALSE;

	// // receive a frame
	// avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);

	// vod_memcpy(frame_end, original_pad, sizeof(original_pad));

	// if (avrc == AVERROR(EAGAIN))
	// {
	// 	return VOD_AGAIN;
	// }

	// if (avrc < 0)
	// {
	// 	vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
	// 		"audio_decoder_decode_frame: avcodec_receive_frame failed %d", avrc);
	// 	return VOD_BAD_DATA;
	// }

	*result = state->decoded_frame;
	return VOD_OK;
}


// 映射函数
const char* get_ffmpeg_encoder_name(int codec_id) {
    switch (codec_id) {
        case VOD_CODEC_ID_AVC:
            return "libx264";  // H.264 编码器
        case VOD_CODEC_ID_HEVC:
            return "libx265";  // H.265 编码器
        case VOD_CODEC_ID_VP8:
            return "libvpx";   // VP8 编码器
        case VOD_CODEC_ID_VP9:
            return "libvpx-vp9"; // VP9 编码器
        case VOD_CODEC_ID_AV1:
            return "libaom-av1"; // AV1 编码器
        default:
            return NULL;        // 未知的 codec_id
    }
}

// 映射函数
enum AVCodecID get_ffmpeg_decoder_id(int codec_id) {
    switch (codec_id) {
        case VOD_CODEC_ID_AVC:
            return AV_CODEC_ID_H264;  // H.264 解码器
        case VOD_CODEC_ID_HEVC:
            return AV_CODEC_ID_HEVC;  // H.265 解码器
        case VOD_CODEC_ID_VP8:
            return AV_CODEC_ID_VP8;   // VP8 解码器
        case VOD_CODEC_ID_VP9:
            return AV_CODEC_ID_VP9;   // VP9 解码器
        case VOD_CODEC_ID_AV1:
            return AV_CODEC_ID_AV1;   // AV1 解码器
        default:
            return AV_CODEC_ID_NONE;  // 未知的 codec_id
    }
}

static vod_status_t
video_encoder_init(
		frame_watermark_filter_state_t* state
)
{
	AVCodecContext* encoder;
	int avrc;

	//FIXME
	// if (!initialized)
	// {
	// 	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
	// 		"audio_encoder_init: module failed to initialize successfully");
	// 	return VOD_UNEXPECTED;
	// }


	const AVCodec *encoder_codec = NULL;

	encoder_codec = avcodec_find_encoder_by_name(get_ffmpeg_encoder_name(state->codec_id));
	if (encoder_codec != NULL)
	{
		vod_log_error(VOD_LOG_INFO, state->request_context->log, 0,
			"audio_encoder_process_init: using aac encoder \"%s\"", get_ffmpeg_encoder_name(state->codec_id));
		return VOD_ALLOC_FAILED;
	}

	// init the encoder
	encoder = avcodec_alloc_context3(encoder_codec);
	if (encoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_init: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	state->encoder = encoder;
	encoder->width = state->decoder->width;
	encoder->height = state->decoder->height;
	encoder->sample_aspect_ratio = state->decoder->sample_aspect_ratio;

	encoder->pix_fmt = state->decoder->pix_fmt;
	encoder->time_base = state->decoder->time_base;

	encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;		// make the codec generate the extra data

	avrc = avcodec_open2(encoder, encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_init: avcodec_open2 failed %d", avrc);
		//FIXME
		// audio_encoder_free(state);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}


static vod_status_t
video_decoder_init(
	frame_watermark_filter_state_t* state,
	media_info_t* media_info)
{
	AVCodecContext* decoder;
	int avrc;

	if (media_info->codec_id != VOD_CODEC_ID_VIDEO)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_decoder_init_decoder: codec id %uD not supported", media_info->codec_id);
		return VOD_BAD_REQUEST;
	}
	const AVCodec *decoder_codec = NULL;

	state->decoded_frame = av_frame_alloc();

	decoder_codec = avcodec_find_decoder(get_ffmpeg_decoder_id(media_info->codec_id));
	// init the decoder	
	decoder = avcodec_alloc_context3(decoder_codec);
	if (decoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_init_decoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	state->decoder = decoder;	
	
	decoder->time_base.num = 1;
	decoder->time_base.den = media_info->frames_timescale;

    decoder->codec_id = get_ffmpeg_decoder_id(media_info->codec_id);
    decoder->codec_type = AVMEDIA_TYPE_VIDEO;
    decoder->width = media_info->u.video.width;
    decoder->height = media_info->u.video.height;

    decoder->pix_fmt = AV_PIX_FMT_YUV420P;
    // dec_ctx->bit_rate = 
    // dec_ctx->framerate = 
	avrc = avcodec_open2(decoder, decoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_init_decoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}


static vod_status_t
video_encoder_write_frame(
	void* context,
	AVFrame* frame,
	void **new_buffer)
{
	frame_watermark_filter_state_t* state = context;
	vod_status_t rc = VOD_OK;
	AVPacket* output_packet;
	int avrc;

	// send frame
	avrc = avcodec_send_frame(state->encoder, frame);

	av_frame_unref(frame);

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_write_frame: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	// receive packet
	output_packet = av_packet_alloc();
	if (output_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_write_frame: av_packet_alloc failed");
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
			"audio_encoder_write_frame: avcodec_receive_packet failed %d", avrc);
		av_packet_free(&output_packet);
		return VOD_ALLOC_FAILED;
	}

	//转buffer
	new_buffer = vod_alloc(state->request_context->pool, output_packet->size);

	vod_memcpy(new_buffer, output_packet->data, output_packet->size);

	// rc = audio_encoder_write_packet(state, output_packet);

	av_packet_free(&output_packet);

	return rc;
}

static int create_filter_graph(FilterContext *filter_ctx, AVCodecContext *dec_ctx) {
    char args[512];
    int ret;
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();

    filter_ctx->filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_ctx->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
             dec_ctx->time_base.num, dec_ctx->time_base.den,
             dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&filter_ctx->buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_ctx->filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Cannot create buffer source\n");
        goto end;
    }

    ret = avfilter_graph_create_filter(&filter_ctx->buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_ctx->filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Cannot create buffer sink\n");
        goto end;
    }

    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
    ret = av_opt_set_int_list(filter_ctx->buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        fprintf(stderr, "Cannot set output pixel format\n");
        goto end;
    }

    AVFilterContext *text_filter;
    ret = avfilter_graph_create_filter(&text_filter, avfilter_get_by_name("drawtext"), "drawtext",
                                       "text='Your Watermark':fontcolor=white@0.8:x=10:y=10", NULL, filter_ctx->filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Cannot create drawtext filter\n");
        goto end;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = filter_ctx->buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = filter_ctx->buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    if ((ret = avfilter_link(filter_ctx->buffersrc_ctx, 0, text_filter, 0)) < 0) {
        fprintf(stderr, "Error linking filters\n");
        goto end;
    }
    if ((ret = avfilter_link(text_filter, 0, filter_ctx->buffersink_ctx, 0)) < 0) {
        fprintf(stderr, "Error linking filters\n");
        goto end;
    }

    if ((ret = avfilter_graph_config(filter_ctx->filter_graph, NULL)) < 0) {
        fprintf(stderr, "Error configuring the filter graph\n");
        goto end;
    }

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}


static int process_frame(FilterContext *filter_ctx, AVFrame *frame) {
    int ret;

    // 将帧添加到过滤器图形中
    if ((ret = av_buffersrc_add_frame_flags(filter_ctx->buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
        fprintf(stderr, "Error while feeding the filtergraph\n");
        return ret;
    }

    // 从过滤器图形中获取处理后的帧
    AVFrame *filt_frame = av_frame_alloc();
    if (!filt_frame) {
        ret = AVERROR(ENOMEM);
        return ret;
    }

    while ((ret = av_buffersink_get_frame(filter_ctx->buffersink_ctx, filt_frame)) >= 0) {
        // 在这里处理处理后的帧 filt_frame，例如将其写入输出文件

        av_frame_unref(filt_frame);
    }

    av_frame_free(&filt_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
    }

    return ret;
}

static vod_status_t
frame_watermark_write(media_filter_context_t* context, const u_char* buffer, uint32_t size)
{
	
	frame_watermark_filter_state_t* state = get_context(context);
	// uint32_t offset_limit;
	// uint32_t end_offset;
	// uint32_t cur_size;
	// vod_status_t rc;
	// int out_size;
	int32_t ret;

	// end_offset = state->cur_offset + size;


	//TODO watermark process
	request_context_t* request_context = context->request_context;

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"test watermark");

	
	AVFrame* result;
	if ( (ret = video_decoder_decode_frame(state, (u_char *)buffer, &result)) != VOD_OK ) 
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"video_decoder_decode_frame error %d\n", ret);
			
		return state->write(context, buffer, size);
	}

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"ooooooooooook \n");

	// //TODO 挪出去
	// FilterContext filter_ctx;


    // // 创建过滤器图形
    // if ((ret = create_filter_graph(&filter_ctx, state->decoder)) < 0) {
	// 	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
	// 		"Could not create filter graph %d\n", ret);
    //     return VOD_UNEXPECTED;
    // }

	// //add watermark
	// // 在这里处理视频帧并添加水印
	// if ((ret = process_frame(&filter_ctx, result)) < 0) {
	// 	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
	// 		"process frame error %d\n", ret);
	// 	av_frame_free(&result);
	// 	return ret;
	// }

	// void *new_buffer = NULL;
	// //输出新的buffer
	// video_encoder_write_frame(state, result, &new_buffer);

	// // clear trail
	// return state->write(context, new_buffer, sizeof(new_buffer));
	
	return VOD_OK;
}

// 初始化 FFmpeg 库
void initialize_ffmpeg() {
    // av_register_all();
    // avfilter_register_all();
}





static void
frame_watermark_cleanup(frame_watermark_filter_state_t* state)
{
}

vod_status_t
frame_watermark_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context
	)
{
	frame_watermark_filter_state_t* state;
	request_context_t* request_context = context->request_context;

	vod_pool_cleanup_t *cln;

	// allocate state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"frame_watermark_filter_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// allocate cleanup item
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"frame_watermark_filter_init: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}
	
	

	cln->handler = (vod_pool_cleanup_pt)frame_watermark_cleanup;
	cln->data = state;


	// save required functions
	state->start_frame = filter->start_frame;
	state->write = filter->write;
	state->request_context = request_context;

	// override functions
	filter->start_frame = frame_watermark_start_frame;
	filter->flush_frame = frame_watermark_write;


	
	// save the context
	context->context[THIS_FILTER] = state;

	return VOD_OK;
}


vod_status_t
frame_watermark_filter_init_2(
	media_filter_t* filter,
	media_filter_context_t* context,
	media_info_t *media_info)
{
	frame_watermark_filter_state_t* state = get_context(context);
	request_context_t* request_context = context->request_context;


	state->codec_id = media_info->codec_id;
	state->codec_name = media_info->codec_name;

	video_decoder_init(state, media_info);
	video_encoder_init(state);
	

	return VOD_OK;
}