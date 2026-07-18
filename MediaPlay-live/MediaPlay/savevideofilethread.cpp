#include "savevideofilethread.h"
//音视频编码 + 文件写入 + 同步
SaveVideoFileThread::SaveVideoFileThread()
{
    m_picInPicRead =new PicInPic_Read;

    connect(m_picInPicRead,SIGNAL(SIG_sendPicInPic( QImage )),this,SIGNAL(SIG_sendPicInPic( QImage )));
    connect(m_picInPicRead,SIGNAL(SIG_sendVideoFrame( QImage )),this,SIGNAL(SIG_sendVideoFrame( QImage )));
    connect(m_picInPicRead,SIGNAL(SIG_sendVideoFrameData(uint8_t*,int)),this,SLOT(slot_writeVideoFrameData(uint8_t*, int)));

    m_audioRead = new Audio_Read;
    connect(m_audioRead,SIGNAL(SIG_sendAudioFrameData( uint8_t*, int)),this,SLOT(slot_writeAudioFrameData(uint8_t*, int)));

    mAudioOneFrameSize=0;

    lastVideoNode=nullptr;
    m_videoBeginTime=0;
    m_videoBeginFlag=false;

    isStop=false;
    video_pts=0;
    audio_pts=0;

}
//get_video_frame(ost);
//write_video_frame(oc,&video_st);
int SaveVideoFileThread::write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                                     AVStream *st, AVFrame *frame)
{
    int ret;

    if(!frame) return 0;

    // send the frame to the encoder
    ret = avcodec_send_frame(c, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame to the encoder\n");
        exit(1);
    }

    while (ret >= 0) {
        AVPacket pkt = { 0 };

        ret = avcodec_receive_packet(c, &pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error encoding a frame\n");
            exit(1);
        }

        /* rescale output packet timestamp values from codec to stream timebase */
        av_packet_rescale_ts(&pkt, c->time_base, st->time_base);
        pkt.stream_index = st->index;

        /* Write the compressed frame to the media file. */
        ret = av_interleaved_write_frame(fmt_ctx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) {
            fprintf(stderr, "Error while writing output packet\n");
            exit(1);
        }
    }

    return ret == AVERROR_EOF ? 1 : 0;
}

void SaveVideoFileThread::slot_writeVideoFrameData(uint8_t *picture_buf, int buffer_size)
{
    //写入视频帧
    //记录第一次时间
    //投递队列时, 每一帧数据需要 记录对应的时间
    if( m_videoBeginFlag ) {
        m_videoBeginTime = QDateTime::currentMSecsSinceEpoch();;
        m_videoBeginFlag = false;
    }
    qint64 curTime = QDateTime::currentMSecsSinceEpoch();
    qint64 pts = curTime - m_videoBeginTime;
    videoDataQuene_Input( picture_buf , buffer_size , pts );
    //以下为写入视频帧的函数
    //int write_video_frame( ... );
    // memcpy( video_st.frameBuffer,picture_buf , buffer_size );
    // av_free( picture_buf );
    // video_st.frame->pts = video_st.next_pts++;
    // write_frame(oc, video_st.enc, video_st.st, video_st.frame);
}
bool SaveVideoFileThread::write_audio_frame(AVFormatContext *oc, OutputStream *ost)
{
    //qDebug() << __func__;
    AVCodecContext *c;
    c = audio_st.enc;
    //从队列里面获取
    BufferDataNode* node = NULL;
    node = audioDataQuene_get();
    if ( node )
    {
        memcpy(ost->frameBuffer, node->buffer, node->bufferSize);
        free(node->buffer);
        node->buffer = NULL;
        free(node);
        node = NULL;
    }
    else
    {
        return false;
    }
    audio_st.frame->pts = audio_st.next_pts;
    audio_st.next_pts+= audio_st.frame->nb_samples;
    AVRational rational;
    rational.num = 1;
    rational.den = c->sample_rate;
    audio_st.frame->pts = av_rescale_q(audio_st.samples_count, rational, c->time_base);
    audio_st.samples_count += audio_st.frame->nb_samples;
    return write_frame(oc, audio_st.enc, audio_st.st, audio_st.frame ,audio_pts , ost);
}


bool SaveVideoFileThread::write_video_frame(AVFormatContext *oc, OutputStream *ost, double time)
{
   // qDebug() << __func__;
    /// 稳帧的处理, 首先根据下一帧的时间, 获取大于下一帧时间的数据
    /// 如果没有就播放上一画面
    BufferDataNode *node = videoDataQuene_get(time);
    if (node == NULL)
    {
        if( lastVideoNode )
            node = lastVideoNode;
        else
            return false;
    }
    else
    {
        if (node != lastVideoNode)
        {
            if (lastVideoNode != NULL)
            {
                av_free(lastVideoNode->buffer);
                free(lastVideoNode);
            }
            lastVideoNode = node;
        }
    }
    if( !node ) return false;
    memcpy(ost->frameBuffer, node->buffer, node->bufferSize);
    ost->frame->pts = ost->next_pts++;
    return write_frame( oc , ost->enc , video_st.st , ost->frame , video_pts , ost);
}
int SaveVideoFileThread::write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,AVStream *st,AVFrame *frame,int64_t & pts , OutputStream *ost )
{
    int ret;
    if( !frame ) return 0;
    // send the frame to the encoder
    ret = avcodec_send_frame(c, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame to the encoder\n" );
        return 0 ;
    }
    while (ret >= 0) {
        AVPacket pkt = { 0 };
        ret = avcodec_receive_packet(c, &pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error encoding a frame\n");
            return 0 ;
        }
        /* rescale output packet timestamp values from codec to stream timebase */
        av_packet_rescale_ts(&pkt, c->time_base, st->time_base);
        pkt.stream_index = st->index;

        //todo 修改部分 下一帧数据时间戳 pts 单位 ms
        pts = av_rescale_q( ost->next_pts, st->time_base, {1, 1000});

        /* Write the compressed frame to the media file. */
        ret = av_interleaved_write_frame(fmt_ctx, &pkt);
        av_packet_unref(&pkt);

        if (ret < 0) {
            fprintf(stderr, "Error while writing output packet\n");
            return 0 ;
        }
    }
    return ret == AVERROR_EOF ? 1 : 0;
}

void SaveVideoFileThread::slot_writeAudioFrameData(uint8_t *picture_buf, int buffer_size)
{
    //写帧
    //视频如果没有 , 丢弃帧,拉齐视音频时间
    if( (m_avFormat.hasCamera || m_avFormat.hasDesk)&& m_videoBeginFlag )
    {
        free( picture_buf );
        return;
    }
    audioDataQuene_Input( picture_buf , buffer_size );

    // 以下为写入音频帧的过程
    // int write_audio_frame( ... );
    //AVCodecContext *c;
    //c = audio_st.enc;
    // memcpy( audio_st.frameBuffer,picture_buf , buffer_size );
    //free( picture_buf );
    //audio_st.frame->pts = audio_st.next_pts;
    //audio_st.next_pts+= audio_st.frame->nb_samples;
    //AVRational rational;
    //rational.num = 1;
    //rational.den = c->sample_rate;
    //audio_st.frame->pts = av_rescale_q(audio_st.samples_count, rational,c->time_base);
    //audio_st.samples_count += audio_st.frame->nb_samples;
    //write_frame(oc, audio_st.enc, audio_st.st, audio_st.frame);

}
/* Add an output stream. */
void SaveVideoFileThread::add_audio_stream(OutputStream *ost, AVFormatContext *oc,AVCodec **codec,AVCodecID codec_id)
{
    AVCodecContext *c;
    int i;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "Could not find encoder for '%s'\n",avcodec_get_name(codec_id));
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = c;

    c->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    c->bit_rate    = 64000;
    c->sample_rate = 44100;
    c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
    c->channel_layout = AV_CH_LAYOUT_STEREO;
    ost->st->time_base ={ 1, c->sample_rate };


    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/* Add an output stream. */
void SaveVideoFileThread::add_video_stream(OutputStream *ost, AVFormatContext *oc,AVCodec **codec,AVCodecID codec_id)
{
    AVCodecContext *c;
    int i;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "Could not find encoder for '%s'\n",
                avcodec_get_name(codec_id));
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = c;

    c->codec_id = codec_id;

    c->bit_rate = m_avFormat.videoBitRate;
    /* Resolution must be a multiple of two. */
    c->width    = m_avFormat.width;
    c->height   = m_avFormat.height;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
    ost->st->time_base = { 1, m_avFormat.frame_rate };
    c->time_base       = ost->st->time_base;

    c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
    c->pix_fmt       = STREAM_PIX_FMT;
    if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        /* just for testing, we also add B-frames */
        c->max_b_frames = 2;
    }
    if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
        c->mb_decision = 2;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}
void SaveVideoFileThread::open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost)
{
    AVCodecContext *c;
    int nb_samples;
    int ret;

    c = ost->enc;

    /* open it */
    ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open audio codec\n");
        exit(1);
    }

    nb_samples = c->frame_size;

    ost->frame = av_frame_alloc();
    if (!ost->frame) {
    fprintf(stderr, "Error allocating an audio frame\n");
    exit(1);
    }
    ost->frame->format = c->sample_fmt;
    ost->frame->channel_layout = c->channel_layout;
    ost->frame->sample_rate = c->sample_rate;
    ost->frame->nb_samples = nb_samples;
    //申请空间
    mAudioOneFrameSize = av_samples_get_buffer_size(NULL, c->channels,
    c->frame_size, c->sample_fmt, 1);
    qDebug() << "mAudioOneFrameSize:" << mAudioOneFrameSize;
    ost->frameBuffer=(uint8_t *)av_malloc(mAudioOneFrameSize);
    ost->frameBufferSize = mAudioOneFrameSize;
    ///这句话必须要(设置这个 frame 里面的采样点个数)
    int oneChannelBufferSize = mAudioOneFrameSize / c->channels; //计算出一个声道的数据
    int nb_samplesize = oneChannelBufferSize /
    av_get_bytes_per_sample(c->sample_fmt); //计算出采样点个数
    ost->frame->nb_samples = nb_samplesize;
    ///这 2 种方式都可以
    //avcodec_fill_audio_frame(ost->frame, c->channels, c->sample_fmt,(const uint8_t*)ost->frameBuffer, mAudioOneFrameSize, 0);
    av_samples_fill_arrays(ost->frame->data, ost->frame->linesize,ost->frameBuffer,c->channels,ost->frame->nb_samples, c->sample_fmt, 0);

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }

    /* create resampler context */
    ost->swr_ctx = swr_alloc();
    if (!ost->swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        exit(1);
    }
}

void SaveVideoFileThread::open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost)
{
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;

    av_dict_set(&opt,"preset","superfast",0);
    av_dict_set(&opt,"tune","zerolatency",0);

    c->thread_count=4;

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec\n");
        exit(1);
    }

    /* allocate and init a re-usable frame */
    ost->frame = av_frame_alloc();
    ost->frame->format = c->pix_fmt;
    ost->frame->width=c->width;
    ost->frame->height = c->height;

    int numBytes_yuv = avpicture_get_size(AV_PIX_FMT_YUV420P, c->width,c->height);

    uint8_t * out_buffer_yuv = (uint8_t *) av_malloc(numBytes_yuv * sizeof(uint8_t));

    avpicture_fill((AVPicture *) ost->frame, out_buffer_yuv,
                   AV_PIX_FMT_YUV420P,c->width,c->height);

    ost->frameBuffer = out_buffer_yuv;
    ost->frameBufferSize = numBytes_yuv;

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
}


void SaveVideoFileThread::slot_setInfo(STRU_AV_FORMAT &avFormat)
{
    m_avFormat=avFormat;

    char filename[260]="";
    std::string path=m_avFormat.fileName.toStdString();
    sprintf(filename,"%s",path.c_str());
    //编码的准备
    /* allocate the output media context */
    avformat_alloc_output_context2(&oc, NULL,"flv", filename); //保存flv格式，为了未来推流rtmp（flv）
    if (!oc) {
        printf("Could not deduce output format from file extension: using MPEG.\n");
        avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
    }
    if (!oc)
        return;

    fmt = oc->oformat;

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (m_avFormat.hasCamera||m_avFormat.hasDesk) {
        add_video_stream(&video_st, oc, &video_codec,AV_CODEC_ID_H264);
        have_video = 1;
        encode_video = 1;
    }
    if (m_avFormat.hasAudio) {
        add_audio_stream(&audio_st, oc, &audio_codec, AV_CODEC_ID_AAC);
        have_audio = 1;
        encode_audio = 1;
    }

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (have_video)
        open_video(oc, video_codec, &video_st);
    if (have_audio)
        open_audio(oc, audio_codec, &audio_st);

    av_dump_format(oc, 0, filename, 1);

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open '%s'\n", filename);
            return;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(oc, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        return;
    }

    this->start(); //触发run()
}

void SaveVideoFileThread::slot_openVideo()
{
    if(m_avFormat.hasCamera||m_avFormat.hasDesk){
        m_picInPicRead->slot_openVideo();
        m_videoBeginFlag=true;
    }

    if(m_avFormat.hasAudio)
        m_audioRead->slot_openAudio();
}

void SaveVideoFileThread::close_stream(AVFormatContext *oc, OutputStream *ost)
{
    if (!ost) return;
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    //回收
    av_free(ost->frameBuffer);
    av_frame_free(&ost->tmp_frame);
    sws_freeContext(ost->sws_ctx);
    swr_free(&ost->swr_ctx);
}

void SaveVideoFileThread::slot_closeVideo()
{
     if(m_avFormat.hasCamera||m_avFormat.hasDesk)
         m_picInPicRead->slot_closeVideo();
     if(m_avFormat.hasAudio)
         m_audioRead->slot_closeAudio();

     isStop=true;
}

void SaveVideoFileThread::run()
{

    while(1)
    {
        /* select the stream to encode */
        if ( have_video &&
             ( !have_audio|| (av_compare_ts(video_st.next_pts, video_st.enc->time_base,
                                            audio_st.next_pts, audio_st.enc->time_base) <= 0)) )
        {
            if (!write_video_frame(oc, &video_st, video_pts))
                msleep(1);
        }
        else
        {
            if (!write_audio_frame(oc, &audio_st))
                msleep(1);
        }
        if(isStop){
            if( ( have_audio && (m_audioDataList.size() == 0))||(have_video && (m_videoDataList.size() == 0)))
            break;
        }
    }
    while(1){
        if ( isStop )
        {
            if( ( have_audio && (m_audioDataList.size() == 0))||(have_video && (m_videoDataList.size() == 0)))

            {
                if( m_audioDataList.size() != 0 )

                {
                    write_audio_frame(oc, &audio_st);

                }else if( m_videoDataList.size() != 0 )

                {
                    write_video_frame(oc, &video_st, video_pts);
                }
            }
            if( m_audioDataList.size() == 0 && m_videoDataList.size() == 0)
                break;
        }
    }


    /* Write the trailer, if any. The trailer must be written before you
         * close the CodecContexts open when you wrote the header; otherwise
         * av_write_trailer() may try to use memory that was freed on
         * av_codec_close(). */
    av_write_trailer(oc);

    /* Close each codec. */
    if (m_avFormat.hasCamera||m_avFormat.hasDesk)
        close_stream(oc, &video_st);
    if (m_avFormat.hasAudio)
        close_stream(oc, &audio_st);

    if (!(fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&oc->pb);

    /* free the stream */
    avformat_free_context(oc);
}

void SaveVideoFileThread::videoDataQuene_Input(uint8_t * buffer, int size, int64_t time)
{
    BufferDataNode * node = (BufferDataNode*)malloc(sizeof(BufferDataNode));
    node->bufferSize = size;
    node->time = time;
    node->buffer = buffer;
    m_videoMutex.lock();
    m_videoDataList.append(node);
    m_videoMutex.unlock();
}

BufferDataNode *SaveVideoFileThread::videoDataQuene_get(int64_t time)
{
    BufferDataNode * node = NULL;
    m_videoMutex.lock();
    while(m_videoDataList.size()!= 0 )
    {
        node = m_videoDataList.takeFirst(); //取出元素并在链表移除该点
        if( m_videoDataList.size() == 0) break;
        if( time > node->time ) //淘汰掉小于下一帧时间的数据
        {
            av_free(node->buffer);
            node->buffer = NULL;
            free(node);
            node = NULL;
        }else
        {
            break;
        }
    }
    m_videoMutex.unlock();
    return node;
}

void SaveVideoFileThread::audioDataQuene_Input(const uint8_t *buffer, const int &size)
{
    BufferDataNode *node = (BufferDataNode*)malloc(sizeof(BufferDataNode));
    node->bufferSize = size;
    node->buffer = (uint8_t*)buffer;
    m_audioMutex.lock();
    m_audioDataList.append(node);
    m_audioMutex.unlock();
}

BufferDataNode* SaveVideoFileThread::audioDataQuene_get()
{
    BufferDataNode *node = NULL;
    m_audioMutex.lock();
    if (m_audioDataList.size() != 0 ){

        node = m_audioDataList.takeFirst();
    }
    m_audioMutex.unlock();
    return node;
}

const STRU_AV_FORMAT &SaveVideoFileThread::avFormat() const
{
    return m_avFormat;
}
