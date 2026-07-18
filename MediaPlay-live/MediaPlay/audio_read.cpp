#include "audio_read.h"
//音频采集 + S16→FLTP 转换
Audio_Read::Audio_Read()
{
    //声卡采样格式
    // set up the format you want, eg.
    format.setSampleRate(AudioCollectFrequency);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    //format.setByteOrder(QAudioFormat::BigEndian);
    format.setSampleType(QAudioFormat::UnSignedInt);
    //format.setSampleType(QAudioFormat::SignedInt);
    QAudioDeviceInfo info = QAudioDeviceInfo::defaultInputDevice();
    if (!info.isFormatSupported(format)) {
        // qWarning()<<"default format not supported try to use nearest";
        QMessageBox::information(NULL , "提示", "打开音频设备失败");
        format = info.nearestFormat(format);
    }

    audio_in = NULL;
    m_playState = state_stop;
    timer = new QTimer;
    connect( timer , SIGNAL(timeout()),this,SLOT(slot_readMore()) );
    m_buffPos = 0;
}
Audio_Read::~Audio_Read()
{
    this->UnInit();
    delete audio_in;
}
//关于数据点个数aac采样点 一帧1024 频率 44.1kHz 那么采样间隔就是 1024/44.1= 23.2 ms
//一次读取两帧 aac即 2048 个采样点
void Audio_Read::slot_readMore()
{
    //视频没有开始采集 不采集音频
    if (!audio_in)
        return;
    std::vector<uint8_t> multipacketbuffer;
    qint64 len = audio_in->bytesReady();


    if (len < OneAudioSize)
    {
        return;
    }
    //一次性都读出来
    if( m_buffPos == 0)
    {
        m_audiobuff.resize( len );
    }else
    {
        int nSize = m_audiobuff.size();
        uint8_t* pBegin = &*m_audiobuff.begin();
        std::vector<uint8_t> tempbuff(nSize - m_buffPos , 0 );
        uint8_t* pTemp = &*tempbuff.begin();
        //先保存到临时空间 , 然后申请新空间
        memcpy( pTemp , pBegin + m_buffPos , nSize - m_buffPos);

        m_audiobuff.resize( len + tempbuff.size() );
        pBegin = &*m_audiobuff.begin();

        memcpy( pBegin , pTemp , tempbuff.size() );
        m_buffPos = tempbuff.size();
    }
    uint8_t* pAudioBuff = &*m_audiobuff.begin() + m_buffPos;

    int offset = 0;
    int packSize = len;
    //数组首地址
    //uint8_t * pInData =(uint8_t*)&*m_buffer.begin() ;

    //这次不读走 后面就丢了
    while( packSize ){
        qint64 l = myBuffer_in->read( (char*)(pAudioBuff + offset) , packSize );
        if( l > 0 )
        {
            packSize -= l;
            offset += l;
        }
    }

    //积累了多少帧
    int nFrameCount =( m_audiobuff.size()/OneAudioSize );

    multipacketbuffer.resize( OneAudioSize*nFrameCount );


    //所有帧数组首地址
    uint8_t * in_buffer =(uint8_t*)&*multipacketbuffer.begin() ;
    memcpy( in_buffer , &*m_audiobuff.begin() , OneAudioSize*nFrameCount );
    m_buffPos = OneAudioSize*nFrameCount;
    if( m_buffPos == m_audiobuff.size() )
        m_buffPos = 0;
    /// nb_samples 和 frame_size = 1024
    /// nb_samples 采样数
    ///一帧数据量：1024*2*av_get_bytes_per_sample(s16) = 4096 个字节。
    ///会编码：88200/(1024*2*av_get_bytes_per_sample(s16)) = 21.5 帧数据
    /// aac 规定每帧音频只能有 1024 个采样
    //采集的 pcm 数据是 s16 模式 是 uint 而编码的时候, 使用的是 fltp 需要转换 使用swr_convert
    //1024 字节 元素 float *4
    uint8_t * out_buffer[2]; //左右声道数据
    out_buffer[0] = (uint8_t *)malloc( OneAudioSize*nFrameCount);
    memset( out_buffer[0] , 0 ,OneAudioSize*nFrameCount);

    out_buffer[1] = (uint8_t *)malloc( OneAudioSize*nFrameCount);
    memset( out_buffer[1] , 0 ,OneAudioSize*nFrameCount);
    // 关于长度采样数 即 1024
    int count =swr_convert(swr, out_buffer , nFrameCount*OneAudioSize/4,

                           (const uint8_t**)&/*pInData*/in_buffer,nFrameCount*OneAudioSize/4);//len 为 4096
    //每次 读取 一帧
    for( int n = 0 ; n < nFrameCount ; n++)
    {

        uint8_t * audio_left = out_buffer[0] + n* OneAudioSize ;
        uint8_t * audio_right = out_buffer[1] + n* OneAudioSize ;

        //pInData存储 PCM 数据长度 4096 小端
        //转换为 ffmpeg FLTP 平面模式LLLLLLLLLLLLLLLLRRRRRRRRRRRRRRRR

        uint8_t * buffer = (uint8_t *)malloc( OneAudioSize * AudioChannelCount);
        memset( buffer , 0 ,OneAudioSize * AudioChannelCount);

        //第一帧左声道
        memcpy( buffer , audio_left , OneAudioSize );

        //第一帧右声道
        memcpy( buffer + OneAudioSize , audio_right , OneAudioSize );

        Q_EMIT SIG_sendAudioFrameData( (uint8_t*)buffer, OneAudioSize *
                                       AudioChannelCount );
    }
    delete [] out_buffer[0];
    delete [] out_buffer[1];
}
void Audio_Read::UnInit()
{
    if(timer)
        timer->stop();
    if(audio_in)
        audio_in->stop();
}
void Audio_Read:: slot_openAudio()
{
    if( m_playState == state_stop )
    {
        audio_in = new QAudioInput(format, this);
        myBuffer_in = audio_in->start();
    }
    else
    {
        if(audio_in)
        {
            delete audio_in;
            audio_in = new QAudioInput(format, this);
            myBuffer_in = audio_in->start();
        }
    }
    m_playState = state_play;
    //重采样设置选项----------------
    enum AVSampleFormat in_sample_fmt; //输入的采样格式
    enum AVSampleFormat out_sample_fmt;//输出的采样格式 16bit PCM
    int in_sample_rate;//输入的采样率
    int out_sample_rate;//输出的采样率
    int in_ch_layout;//输入的声道布局
    int out_ch_layout;//输入的声道布局
    //输入的采样格式
    in_sample_fmt = AV_SAMPLE_FMT_S16;
    //输出的采样格式 32bit PCM
    out_sample_fmt = AV_SAMPLE_FMT_FLTP;
    //输入的采样率
    in_sample_rate = AudioCollectFrequency;
    //输出的采样率
    out_sample_rate = 44100/*48000*/;
    //输入的声道布局
    in_ch_layout = AV_CH_LAYOUT_STEREO;
    //输出的声道布局
    out_ch_layout = AV_CH_LAYOUT_STEREO;
    swr = swr_alloc_set_opts(nullptr, out_ch_layout, out_sample_fmt, out_sample_rate,in_ch_layout, in_sample_fmt, in_sample_rate,0,nullptr);
    swr_init(swr);
    timer->start(AUDIO_INTERVAL );
}
void Audio_Read:: slot_closeAudio()
{
    if(timer){
        timer->stop();
    }
    if( m_playState == state_play )
    {
        m_playState = state_pause;
        if(audio_in)
            audio_in->stop();
    }
}


