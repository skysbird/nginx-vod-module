#include "watermark_filter.h"
#include "video_filter.h"
#include "../media_set_parser.h"

// macros
// #define watermark_FILTER_DESC_PATTERN "[%uD]atempo=%uD.%02uD[%uD]"
// #define watermark_FILTER_DESC_PATTERN "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d"
#define watermark_FILTER_DESC_PATTERN "drawtext=text='Your Watermark':fontcolor=white:fontsize=24:x=mod(2*mod(n\\,w+t*n)+2*t\\,w):y=mod(2*mod(n\\,h+t*n)+2*t\\,h)"


// enums
enum {
	watermark_FILTER_PARAM_watermark,
	watermark_FILTER_PARAM_SOURCE,

	watermark_FILTER_PARAM_COUNT
};

// constants
static json_object_key_def_t watermark_filter_params[] = {
	{ vod_string("watermark"),	VOD_JSON_FRAC,		watermark_FILTER_PARAM_watermark },
	{ vod_string("source"),	VOD_JSON_OBJECT,	watermark_FILTER_PARAM_SOURCE },
	{ vod_null_string, 0, 0 }
};

// globals
static vod_hash_t watermark_filter_hash;

void
watermark_filter_scale_track_timestamps(
	media_track_t* track,
	uint32_t speed_num,
	uint32_t speed_denom)
{
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	frame_list_part_t* part;

	// TODO: remove this (added temporarily in order to avoid changing existing responses)
	if (speed_num % 10 == 0 && speed_denom % 10 == 0)
	{
		speed_num /= 10;
		speed_denom /= 10;
	}

	track->media_info.timescale *= speed_num;
	track->media_info.duration *= speed_denom;
	track->media_info.full_duration *= speed_denom;
	track->media_info.duration_millis = rescale_time(track->media_info.duration, track->media_info.timescale, 1000);

	track->first_frame_time_offset *= speed_denom;
	track->total_frames_duration *= speed_denom;

	track->media_info.min_frame_duration *= speed_denom;

	if (track->media_info.media_type == MEDIA_TYPE_VIDEO)
	{
		return;		// should not change the frame durations for video, since they will be filtered by libavcodec
	}

	track->media_info.bitrate = (uint32_t)((track->total_frames_size * track->media_info.timescale * 8) / track->media_info.full_duration);

	part = &track->frames;
	last_frame = part->last_frame;
	for (cur_frame = part->first_frame;; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL)
				break;
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
		}

		cur_frame->duration *= speed_denom;
		cur_frame->pts_delay *= speed_denom;
	}
}

static uint32_t
watermark_filter_get_desc_size(media_clip_t* clip)
{
	media_clip_watermark_filter_t* filter = vod_container_of(clip, media_clip_watermark_filter_t, base);

	int width = filter->media_info->u.video.width;  // 视频宽度
    int height = filter->media_info->u.video.height;  // 视频高度
    int fontsize = 24;
    int x_step = 200;  // 水平方向步长
    int y_step = 50;   // 垂直方向步长
	
	char filter_desc[8192] = "";
    char filter_desc_tmp[256];
	char *temp = filter->watermark.data;
	*(temp+ filter->watermark.len) = '\0';

	// 生成 drawtext 滤镜描述字符串
    for (int y = 0; y < height; y += y_step) {
        for (int x = 0; x < width; x += x_step) {
            vod_snprintf(filter_desc_tmp, sizeof(filter_desc_tmp),
                     "drawtext=text='%s':fontcolor=white:fontfile=/usr/share/fonts/truetype/dejavu/puhui.ttf:fontsize=%d:x=%d:y=%d,",
                     temp, fontsize, x, y);
            strcat(filter_desc, filter_desc_tmp);
        }
    }

 	// 移除最后一个逗号
    filter_desc[strlen(filter_desc) - 1] = '\0';	
	return strlen(filter_desc);
}

static u_char*
watermark_filter_append_desc(u_char* p, media_clip_t* clip)
{
	media_clip_watermark_filter_t* filter = vod_container_of(clip, media_clip_watermark_filter_t, base);
	// uint32_t denom;
	// uint32_t num;

	// normalize the fraction to 100 denom
	// num = filter->watermark.num;
	// denom = filter->watermark.denom;
	// while (denom < 100)
	// {
	// 	num *= 10;
	// 	denom *= 10;
	// }

	int width = filter->media_info->u.video.width;  // 视频宽度
    int height = filter->media_info->u.video.height;  // 视频高度
    int fontsize = 24;
    int x_step = 200;  // 水平方向步长
    int y_step = 50;   // 垂直方向步长
	
	char filter_desc[8192] = "";
    char filter_desc_tmp[256];
	char *temp = filter->watermark.data;
	*(temp+ filter->watermark.len) = '\0';

	// 生成 drawtext 滤镜描述字符串
    for (int y = 0; y < height; y += y_step) {
        for (int x = 0; x < width; x += x_step) {
			
            snprintf(filter_desc_tmp, sizeof(filter_desc_tmp),
                     "drawtext=text='%s':fontcolor=white:fontfile=/usr/share/fonts/truetype/dejavu/puhui.ttf:fontsize=%d:x=%d:y=%d,",
                     temp, fontsize, x, y);
            strcat(filter_desc, filter_desc_tmp);
        }
    }

 	// 移除最后一个逗号
    filter_desc[strlen(filter_desc) - 1] = '\0';	
	//TODO 很多hard coding
	return vod_sprintf(
		p,
		"%s",filter_desc);
}

static video_filter_t watermark_filter = {
	watermark_filter_get_desc_size,
	watermark_filter_append_desc,
};

vod_status_t
watermark_filter_parse(
	void* ctx,
	vod_json_object_t* element,
	void** result)
{
	
	return VOD_OK;
}

vod_status_t
watermark_filter_create_from_string(
	request_context_t* request_context, 
	vod_str_t* str, 
	media_clip_t* source, 
	media_clip_watermark_filter_t** result)
{
	media_clip_watermark_filter_t* filter;
	
	filter = vod_alloc(request_context->pool, sizeof(*filter) + sizeof(filter->base.sources[0]));
	if (filter == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"watermark_filter_create_from_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	filter->base.parent = source->parent;
	filter->base.sources = (void*)(filter + 1);
	filter->base.sources[0] = source;
	filter->base.source_count = 1;

	filter->base.type = MEDIA_CLIP_WATERMARK_FILTER;
	filter->base.video_filter = &watermark_filter;
	filter->base.audio_filter = NULL;
	filter->watermark = *str;
	

	source->parent = &filter->base;

	*result = filter;

	return VOD_OK;
}

vod_status_t
watermark_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool)
{
	vod_status_t rc;

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"watermark_filter_hash",
		watermark_filter_params,
		sizeof(watermark_filter_params[0]),
		&watermark_filter_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
