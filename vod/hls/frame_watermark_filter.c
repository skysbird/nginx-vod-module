#include "frame_watermark_filter.h"

// macros
#define THIS_FILTER (MEDIA_FILTER_WATERMARK)
#define get_context(ctx) ((frame_watermark_filter_state_t*)ctx->context[THIS_FILTER])


// typedefs
typedef struct
{
	// fixed input data
	media_filter_start_frame_t start_frame;
	media_filter_write_t write;

	uint32_t cur_offset;
} frame_watermark_filter_state_t;

static vod_status_t
frame_watermark_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	frame_watermark_filter_state_t* state = get_context(context);

	state->cur_offset = 0;

	
	return state->start_frame(context, frame);
}

void
frame_watermark_start_sub_frame(media_filter_context_t* context, uint32_t size)
{
	frame_watermark_filter_state_t* state = get_context(context);

	state->cur_offset = 0;
}

static vod_status_t
frame_watermark_write(media_filter_context_t* context, const u_char* buffer, uint32_t size)
{
	frame_watermark_filter_state_t* state = get_context(context);
	// uint32_t offset_limit;
	uint32_t end_offset;
	// uint32_t cur_size;
	// vod_status_t rc;
	// int out_size;

	end_offset = state->cur_offset + size;


	//TODO watermark process
	request_context_t* request_context = context->request_context;

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"test watermark");

	// clear trail
	if (state->cur_offset < end_offset)
	{
		return state->write(context, buffer, end_offset - state->cur_offset);
	}

	return VOD_OK;
}

static void
frame_watermark_cleanup(frame_watermark_filter_state_t* state)
{
}

vod_status_t
frame_watermark_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context)
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

	// override functions
	filter->start_frame = frame_watermark_start_frame;
	filter->write = frame_watermark_write;

	// save the context
	context->context[THIS_FILTER] = state;

	return VOD_OK;
}
