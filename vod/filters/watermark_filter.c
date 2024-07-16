#include "watermark_filter.h"
#include "video_filter.h"
#include "../media_set_parser.h"

// macros
// #define watermark_FILTER_DESC_PATTERN "[%uD]atempo=%uD.%02uD[%uD]"
// #define watermark_FILTER_DESC_PATTERN "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d"
#define watermark_FILTER_DESC_PATTERN "drawtext=text='Your Watermark':fontcolor=white@0.8:x=10:y=10"


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
	return sizeof(watermark_FILTER_DESC_PATTERN) + VOD_INT32_LEN * 4;
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

	//TODO 很多hard coding
	return vod_sprintf(
		p,
		watermark_FILTER_DESC_PATTERN);
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
	media_filter_parse_context_t* context = ctx;
	media_clip_watermark_filter_t* filter;
	media_range_t* new_range;
	media_range_t* old_range;
	vod_json_value_t* params[watermark_FILTER_PARAM_COUNT];
	vod_json_value_t* source;
	vod_json_value_t* watermark;
	uint32_t old_clip_from;
	uint32_t old_duration;
	vod_status_t rc;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"watermark_filter_parse: started");

	vod_memzero(params, sizeof(params));
		
	vod_json_get_object_values(
		element,
		&watermark_filter_hash,
		params);

	watermark = params[watermark_FILTER_PARAM_watermark];
	source = params[watermark_FILTER_PARAM_SOURCE];

	if (watermark == NULL || source == NULL)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"watermark_filter_parse: \"watermark\" and \"source\" are mandatory for watermark filter");
		return VOD_BAD_MAPPING;
	}

	if (watermark->v.num.denom > 100)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"watermark_filter_parse: invalid watermark, only 2 decimal points are allowed");
		return VOD_BAD_MAPPING;
	}
	
	if (watermark->v.num.num < 0 ||
		watermark->v.num.denom > (uint64_t)watermark->v.num.num * 2 || (uint64_t)watermark->v.num.num > watermark->v.num.denom * 2)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"watermark_filter_parse: invalid watermark %L/%uL, must be between 0.5 and 2", watermark->v.num.num, watermark->v.num.denom);
		return VOD_BAD_MAPPING;
	}

	filter = vod_alloc(context->request_context->pool, sizeof(*filter) + sizeof(filter->base.sources[0]));
	if (filter == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"watermark_filter_parse: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}
	filter->base.sources = (void*)(filter + 1);
	filter->base.source_count = 1;

	filter->base.type = MEDIA_CLIP_WATERMARK_FILTER;
	filter->base.video_filter = &watermark_filter;
	filter->base.audio_filter = NULL;

	filter->watermark.num = watermark->v.num.num;
	filter->watermark.denom = watermark->v.num.denom;

	old_range = context->range;
	if (old_range != NULL)
	{
		new_range = vod_alloc(context->request_context->pool, sizeof(*new_range));
		if (new_range == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"watermark_filter_parse: vod_alloc failed (2)");
			return VOD_ALLOC_FAILED;
		}

		new_range->start = (old_range->start * filter->watermark.num) / filter->watermark.denom;
		new_range->end = (old_range->end * filter->watermark.num) / filter->watermark.denom;
		new_range->timescale = old_range->timescale;
		new_range->original_clip_time = old_range->original_clip_time;

		context->range = new_range;
	}

	old_duration = context->duration;
	old_clip_from = context->clip_from;
	context->duration = ((uint64_t)old_duration * filter->watermark.num) / filter->watermark.denom;
	context->clip_from = ((uint64_t)old_clip_from * filter->watermark.num) / filter->watermark.denom;

	rc = media_set_parse_clip(
		context, 
		&source->v.obj, 
		&filter->base,
		&filter->base.sources[0]);
	if (rc != VOD_JSON_OK)
	{
		return rc;
	}

	context->range = old_range;
	context->duration = old_duration;
	context->clip_from = old_clip_from;

	*result = &filter->base;

	vod_log_debug2(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"watermark_filter_parse: done, watermark=%uD/%uD", filter->watermark.num, filter->watermark.denom);

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
	vod_int_t num;

	num = vod_atofp(str->data, str->len, 2);
	if (num < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"watermark_filter_create_from_string: failed to parse playback watermark \"%V\", expecting a float with up to 2 digits precision", str);
		return VOD_BAD_REQUEST;
	}

	if (num < 50 || num > 200)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"watermark_filter_create_from_string: invalid playback watermark value %i/100, must be between 0.5 and 2", num);
		return VOD_BAD_REQUEST;
	}

	filter = vod_alloc(request_context->pool, sizeof(*filter) + sizeof(filter->base.sources[0]));
	if (filter == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"watermark_filter_create_from_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	filter->base.parent = NULL;
	filter->base.sources = (void*)(filter + 1);
	filter->base.sources[0] = source;
	filter->base.source_count = 1;

	filter->base.type = MEDIA_CLIP_WATERMARK_FILTER;
	filter->base.video_filter = &watermark_filter;
	filter->base.audio_filter = NULL;
	filter->watermark.num = num;
	filter->watermark.denom = 100;

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
