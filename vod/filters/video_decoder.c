#include "video_decoder.h"

// globals
static const AVCodec *decoder_codec = NULL;
static bool_t initialized = FALSE;

void
video_decoder_process_init(vod_log_t* log)
{
	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
		avcodec_register_all();
	#endif

	decoder_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (decoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"video_decoder_process_init: failed to get AAC decoder, video decoding is disabled");
		return;
	}

	initialized = TRUE;
}

// void save_extra_data_to_file(media_info_t* media_info, const char* file_name) {
//     FILE* file = fopen(file_name, "wb");
//     if (!file) {
//         perror("Failed to open file");
//         return;
//     }

//     size_t written = fwrite(media_info->extra_data.data, 1, media_info->extra_data.len, file);
//     if (written != media_info->extra_data.len) {
//         perror("Failed to write all data to file");
//     } else {
//         printf("Data successfully written to %s\n", file_name);
//     }

//     fclose(file);
// }


/* 将数据复制并且增加start      code */
static int alloc_and_copy(AVPacket *pOutPkt, const uint8_t *spspps, uint32_t spsppsSize,
                          const uint8_t *pIn, uint32_t inSize)
{
    int err;
    int startCodeLen = 3; /* start code长度 */

    /* 给pOutPkt->data分配内存 */
    err = av_grow_packet(pOutPkt, spsppsSize + inSize + startCodeLen);
    if (err < 0)
        return err;
    
    if (spspps)
    {
        memcpy(pOutPkt->data , spspps, spsppsSize); /* 拷贝SPS与PPS(前面分离的时候已经加了startcode(00 00 00 01)) */
    }
    
    /* 将真正的原始数据写入packet中 */
    (pOutPkt->data + spsppsSize)[0] = 0;
    (pOutPkt->data + spsppsSize)[1] = 0;
    (pOutPkt->data + spsppsSize)[2] = 1;
    memcpy(pOutPkt->data + spsppsSize + startCodeLen , pIn, inSize);

    return 0;
}

static int h264Mp4ToAnnexb(u_char* buffer, int size,  AVPacket *spsppsPkt , AVPacket **pOutPkt)
{
    unsigned char *pData = buffer; /* 帧数据 */
    unsigned char *pEnd = NULL;
    int dataSize = size; /* pAvPkt->data的数据量 */
    int curSize = 0;
    int naluSize = 0; 
    int i;
    unsigned char nalHeader, nalType;
    int ret;
    int len;

    *pOutPkt = av_packet_alloc();
    (*pOutPkt)->data = NULL;
    (*pOutPkt)->size = 0;

    pEnd = pData + dataSize;

     

    while(curSize < dataSize)
    {
        if(pEnd-pData < 4)
            goto fail;

        /* 前四个字节表示当前NALU的大小 */
        for(i = 0; i < 4; i++)
        {
            naluSize <<= 8;
            naluSize |= pData[i];
        }

        pData += 4;

        if(naluSize > (pEnd-pData+1) || naluSize <= 0)
        {
            goto fail;
        }
        
        nalHeader = *pData;
        nalType = nalHeader&0x1F;
        if(nalType == 5)
        {
           
            /* 添加start code */
            ret = alloc_and_copy(*pOutPkt, spsppsPkt->data, spsppsPkt->size, pData, naluSize);
            if(ret < 0)
                goto fail;
        }
        else
        {
            /* 添加start code */
            ret = alloc_and_copy(*pOutPkt, NULL, 0, pData, naluSize);
            if(ret < 0)
                goto fail;
        }

        /* 将处理好的数据写入文件中 */
        // len = fwrite(pOutPkt->data, 1, pOutPkt->size, pFd);
        // if(len != pOutPkt->size)
        // {
        //     av_log(NULL, AV_LOG_DEBUG, "fwrite warning(%d, %d)!\n", len, pOutPkt->size);
        // }

        // /* 将数据从缓冲区写入磁盘 */
        // fflush(pFd);

        curSize += (naluSize+4);
        pData += naluSize; /* 处理下一个NALU */
    }
    
fail:
    // av_packet_free(&pOutPkt);
    // if(spsppsPkt.data)
    // {
    //     free(spsppsPkt.data);
    //     spsppsPkt.data = NULL;
    // }
        
    return 0;
}


static vod_status_t
video_decoder_init_decoder(
	video_decoder_state_t* state,
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

	// init the decoder	
	decoder = avcodec_alloc_context3(decoder_codec);
	if (decoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_decoder_init_decoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	state->decoder = decoder;	
	state->extra_data = media_info->extra_data;
	// decoder->extradata = media_info->extra_data.data;
	// decoder->extradata_size = media_info->extra_data.len;

	decoder->codec_tag = media_info->format;
	decoder->time_base.num = 1;
	decoder->time_base.den = media_info->frames_timescale;
	decoder->codec_id = get_ffmpeg_decoder_id(media_info->codec_id);
    decoder->codec_type = AVMEDIA_TYPE_VIDEO;
    decoder->width = media_info->u.video.width;
    decoder->height = media_info->u.video.height;

    decoder->pix_fmt = AV_PIX_FMT_YUV420P;

	avrc = avcodec_open2(decoder, decoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_decoder_init_decoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

vod_status_t
video_decoder_init(
	video_decoder_state_t* state,
	request_context_t* request_context,
	media_track_t* track,
	int cache_slot_id)
{
	frame_list_part_t* part;
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	vod_status_t rc;

	if (!initialized)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"video_decoder_init: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	state->request_context = request_context;

	// init the decoder
	rc = video_decoder_init_decoder(
		state,
		&track->media_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// allocate a frame
	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"video_decoder_init: av_frame_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// calculate the max frame size
	state->max_frame_size = 0;
	part = &track->frames;
	last_frame = part->last_frame;
	for (cur_frame = part->first_frame;; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL)
			{
				break;
			}
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
		}

		if (cur_frame->size > state->max_frame_size)
		{
			state->max_frame_size = cur_frame->size;
		}
	}

	// initialize the frame state
	state->cur_frame_pos = 0;
	state->data_handled = TRUE;
	state->frame_started = FALSE;
	state->frame_buffer = NULL;

	state->cur_frame_part = track->frames;
	state->cur_frame = track->frames.first_frame;
	state->dts = track->first_frame_time_offset;

	state->cur_frame_part.frames_source->set_cache_slot_id(
		state->cur_frame_part.frames_source_context,
		cache_slot_id);

	return VOD_OK;
}

void
video_decoder_free(video_decoder_state_t* state)
{
	avcodec_close(state->decoder);
	av_free(state->decoder);
	state->decoder = NULL;
	av_frame_free(&state->decoded_frame);
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

/* 获取SPS与PPS */
static int h264_extradata_to_annexb(const unsigned char *pCodecExtraData,  const int codecExtraDataSize, 
                                AVPacket *pOutExtradata, int padding)
{
    const unsigned char *pExtraData = NULL; /* 前四个字节没用 */
    int len = 0;
    int spsUnitNum, ppsUnitNum;
    int unitSize, totolSize = 0;
    unsigned char startCode[] = {0, 0, 0, 1};
    unsigned char *pOut = NULL;
    int err;

    pExtraData = pCodecExtraData+4;
    len = (*pExtraData++ & 0x3) + 1;

    /* 获取SPS */
    spsUnitNum = (*pExtraData++ & 0x1f); /* SPS数量 */
    while(spsUnitNum--)
    {
        unitSize = (pExtraData[0]<<8 | pExtraData[1]); /* 两个字节表示这个unit的长度 */
        pExtraData += 2;
        totolSize += unitSize + sizeof(startCode);
        printf("unitSize:%d\n", unitSize);

        if(totolSize > INT_MAX - padding) 
        {
            av_log(NULL, AV_LOG_ERROR,
                   "Too big extradata size, corrupted stream or invalid MP4/AVCC bitstream\n");
            av_free(pOut);
            return AVERROR(EINVAL);
        }

        if(pExtraData + unitSize > pCodecExtraData + codecExtraDataSize) 
        {
            av_log(NULL, AV_LOG_ERROR, "Packet header is not contained in global extradata, "
                   "corrupted stream or invalid MP4/AVCC bitstream\n");
            av_free(pOut);
            return AVERROR(EINVAL);
        }

        if((err = av_reallocp(&pOut, totolSize + padding)) < 0)
            return err;
        
        
        memcpy(pOut+totolSize-unitSize-sizeof(startCode), startCode, sizeof(startCode));
        memcpy(pOut+totolSize-unitSize, pExtraData, unitSize);

        pExtraData += unitSize;
    }

    /* 获取PPS */
    ppsUnitNum = (*pExtraData++ & 0x1f); /* PPS数量 */
    while(ppsUnitNum--) 
    {
        unitSize = (pExtraData[0]<<8 | pExtraData[1]); /* 两个字节表示这个unit的长度 */
        pExtraData += 2;
        totolSize += unitSize + sizeof(startCode);
        printf("unitSize:%d\n", unitSize);

        if(totolSize > INT_MAX - padding) 
        {
            av_log(NULL, AV_LOG_ERROR,
                   "Too big extradata size, corrupted stream or invalid MP4/AVCC bitstream\n");
            av_free(pOut);
            return AVERROR(EINVAL);
        }

        if(pExtraData + unitSize > pCodecExtraData + codecExtraDataSize) 
        {
            av_log(NULL, AV_LOG_ERROR, "Packet header is not contained in global extradata, "
                   "corrupted stream or invalid MP4/AVCC bitstream\n");
            av_free(pOut);
            return AVERROR(EINVAL);
        }

        if((err = av_reallocp(&pOut, totolSize + padding)) < 0)
            return err;
        
        
        memcpy(pOut+totolSize-unitSize-sizeof(startCode), startCode, sizeof(startCode));
        memcpy(pOut+totolSize-unitSize, pExtraData, unitSize);

        pExtraData += unitSize;
    }

    pOutExtradata->data = pOut;
    pOutExtradata->size = totolSize;

    return len;
}
static int annexb_to_h264_extradata(const unsigned char *pOutExtradata, const int extradataSize, 
                                    unsigned char **pCodecExtraData, int *codecExtraDataSize)
{
    const unsigned char *pExtradata = pOutExtradata;
    const unsigned char startCode[] = {0, 0, 0, 1};
    int spsCount = 0, ppsCount = 0;
    int totalSize = 0;
    unsigned char *pExtraData = NULL;
    unsigned char *pOut = NULL;
    int unitSize, len;
    int remainingExtradataSize = extradataSize;
    int err;

    // Allocate initial memory for extradata (add 4 bytes for the front)
    pExtraData = malloc(extradataSize + 4 + 10); // 4 for unused data, 10 for headers
    if (!pExtraData) {
        return -1;
    }

    pOut = pExtraData + 4;
    len = 0;

    while (remainingExtradataSize > 4) {
        if (memcmp(pExtradata, startCode, sizeof(startCode)) != 0) {
            printf("Invalid start code in Annex B format\n");
            free(pExtraData);
            return -1;
        }
        pExtradata += 4;
        remainingExtradataSize -= 4;

        unitSize = 0;
        while (remainingExtradataSize > 0 && memcmp(pExtradata, startCode, sizeof(startCode)) != 0) {
            unitSize++;
            remainingExtradataSize--;
            pExtradata++;
        }

        if (unitSize > 0) {
            if ((pExtradata[-unitSize] & 0x1f) == 7) { // SPS
                spsCount++;
            } else if ((pExtradata[-unitSize] & 0x1f) == 8) { // PPS
                ppsCount++;
            }

            if (len + unitSize + 2 > extradataSize) {
                free(pExtraData);
                return -1;
            }

            pOut[len++] = (unitSize >> 8) & 0xff;
            pOut[len++] = unitSize & 0xff;
            memcpy(pOut + len, pExtradata - unitSize, unitSize);
            len += unitSize;
        }
    }

    // Write the SPS and PPS counts and headers
    pExtraData[0] = 0x01;  // configurationVersion
    pExtraData[1] = 0x64;  // AVCProfileIndication
    pExtraData[2] = 0x00;  // profile_compatibility
    pExtraData[3] = 0x1f;  // AVCLevelIndication
    pExtraData[4] = 0xff;  // lengthSizeMinusOne
    pExtraData[5] = 0xe1;  // numOfSequenceParameterSets
    pExtraData[6] = (spsCount & 0x1f);  // numOfSequenceParameterSets
    pExtraData[7] = (ppsCount & 0x1f);  // numOfPictureParameterSets

    // Update the output pointers
    *pCodecExtraData = pExtraData;
    *codecExtraDataSize = len + 4;
}

static vod_status_t
video_decoder_decode_frame(
	video_decoder_state_t* state,
	u_char* buffer,
	AVFrame** result)
{
	static int count = 0;
	input_frame_t* frame = state->cur_frame;

	AVPacket* input_packet;
	u_char original_pad[VOD_BUFFER_PADDING_SIZE];
	u_char* frame_end;
	int avrc;


	vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"key_frame: %d\n", frame->key_frame);

	input_packet = av_packet_alloc();
	if (input_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_decoder_decode_frame: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}


	//FIXME extradata



	// unsigned char ** pout;
	// int poutSize;

	// h264_extradata_to_annexb(state->extra_data.data,
    //                         state->extra_data.len,
    //                         &spsppsPkt, AV_INPUT_BUFFER_PADDING_SIZE);

	// annexb_to_h264_extradata(state->extra_data.data,
	// 	state->extra_data.len,
	// 	&pout,&poutSize);
	AVPacket *pAvPkt;

    AVPacket spsppsPkt;
	spsppsPkt.data = state->extra_data.data;;
	spsppsPkt.size = state->extra_data.len;;

	// save_to_file(buffer,frame->size,"/tmp/decode_source.bin");


	h264Mp4ToAnnexb(buffer, frame->size, &spsppsPkt, &pAvPkt);
	// uint8_t *out_data = NULL;
	// int out_size = 0;

	// avc_to_annexb(buffer, frame->size, &out_data, &out_size);
	// if (avrc)
	// {
	// 	vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
	// 		"av_bitstream_filter_filter: av_bitstream_filter_filter failed %d", av_err2str(avrc));
	// 	return VOD_OK;
	// }
	// state->decoder->extradata = state->extra_data.data;
	// state->decoder->extradata_size = state->extra_data.len;
	// save_to_file(pout,poutSize,"/tmp/d.bin");
	// send a frame
	// save_to_file(pAvPkt->data,pAvPkt->size,"/tmp/decocde.bin");

	input_packet->data = pAvPkt->data;
	input_packet->size = pAvPkt->size;
	input_packet->dts = state->dts;
	input_packet->pts = state->dts + frame->pts_delay;
	input_packet->duration = frame->duration;
	input_packet->flags = AV_PKT_FLAG_KEY;
	state->dts += frame->duration;

	av_frame_unref(state->decoded_frame);

	frame_end = buffer + frame->size;
	vod_memcpy(original_pad, frame_end, sizeof(original_pad));
	vod_memzero(frame_end, sizeof(original_pad));




	avrc = avcodec_send_packet(state->decoder, input_packet);
	av_packet_free(&input_packet);


	// move to the next frame
	state->cur_frame++;
	if (state->cur_frame >= state->cur_frame_part.last_frame &&
		state->cur_frame_part.next != NULL)
	{
		state->cur_frame_part = *state->cur_frame_part.next;
		state->cur_frame = state->cur_frame_part.first_frame;
	}

	state->frame_started = FALSE;

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_decoder_decode_frame: avcodec_send_packet failed %s", av_err2str(avrc));
		return VOD_BAD_DATA;
	}

	// receive a frame
	avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);


	vod_memcpy(frame_end, original_pad, sizeof(original_pad));

	if (avrc == AVERROR(EAGAIN))
	{
		return VOD_AGAIN;
	}

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_decoder_decode_frame: avcodec_receive_frame failed %d", avrc);
		return VOD_BAD_DATA;
	}

	*result = state->decoded_frame;

	vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"video_decoder_decode_frame: frame count %d", count++);

	return VOD_OK;
}

vod_status_t
video_decoder_get_frame(
	video_decoder_state_t* state,
	AVFrame** result)
{
	u_char* read_buffer;
	uint32_t read_size;
	vod_status_t rc;
	bool_t frame_done;

	for (;;)
	{
		// start a frame if needed
		if (!state->frame_started)
		{
			if (state->cur_frame >= state->cur_frame_part.last_frame)
			{
				return VOD_DONE;
			}

			// start the frame
			rc = state->cur_frame_part.frames_source->start_frame(
				state->cur_frame_part.frames_source_context,
				state->cur_frame,
				NULL);
			if (rc != VOD_OK)
			{
				return rc;
			}

			state->frame_started = TRUE;
		}

		// read some data from the frame
		rc = state->cur_frame_part.frames_source->read(
			state->cur_frame_part.frames_source_context,
			&read_buffer,
			&read_size,
			&frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}

			if (!state->data_handled)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"video_decoder_get_frame: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->data_handled = FALSE;
			return VOD_AGAIN;
		}

		state->data_handled = TRUE;

		if (!frame_done)
		{
			// didn't finish the frame, append to the frame buffer
			if (state->frame_buffer == NULL)
			{
				state->frame_buffer = vod_alloc(
					state->request_context->pool,
					state->max_frame_size + VOD_BUFFER_PADDING_SIZE);
				if (state->frame_buffer == NULL)
				{
					vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
						"video_decoder_get_frame: vod_alloc failed");
					return VOD_ALLOC_FAILED;
				}
			}

			vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, read_size);
			state->cur_frame_pos += read_size;
			continue;
		}

		if (state->cur_frame_pos != 0)
		{
			// copy the remainder
			vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, read_size);
			state->cur_frame_pos = 0;
			read_buffer = state->frame_buffer;
		}

		// process the frame
		rc = video_decoder_decode_frame(state, read_buffer, result);
		if (rc != VOD_AGAIN && rc!=VOD_BAD_DATA )
		{
			return rc;
		} 

	}
}
