#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <QThread>
#include <QImage>
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavdevice/avdevice.h"
#include "libavutil/time.h"
#include "SDL_assert.h"
}

#include "PacketQueue.h"

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000
#define SDL_AUDIO_BUFFER_SIZE 1024

enum PlayerState
{
    Playing = 0,
    Pause,
    Stop
};

class VideoPlayer ;
typedef struct VideoState {
    AVFormatContext *pFormatCtx;//相当于视频”文件指针”
    ///////////////音频///////////////////////////////////
    AVStream *audio_st; //音频流
    PacketQueue *audioq;//音频缓冲队列
    AVCodecContext *pAudioCodecCtx ;//音频解码器信息指针
    int audioStream;//视频音频流索引
    double audio_clock; ///<pts of last decoded frame 音频时钟
    SDL_AudioDeviceID audioID; //音频 ID
    AVFrame out_frame; //设置参数，供音频解码后的 swr_alloc_set_opts 使用。
    /// 音频回调函数使用的量
    uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int audio_buf_size = 0;
    unsigned int audio_buf_index = 0;
    AVFrame *audioFrame;
    //////////////////////////////////////////////////////
    ///////////////视频///////////////////////////////////
    AVStream *video_st; //视频流
    PacketQueue *videoq;//视频队列
    AVCodecContext *pCodecCtx ;//音频解码器信息指针
    int videoStream;//视频音频流索引
    double video_clock; ///<pts of last decoded frame 视频时钟
    SDL_Thread *video_tid;
    //视频线程 id
    /////////////////////////////////////////////////////
    /// 播放控制的变量
    bool isPause=false;//暂停标志
    bool quit=false; //停止
    bool readFinished=true; //读线程文件读取完毕
    bool readThreadFinished=true; //读取线程是否结束
    bool videoThreadFinished=true; // 视频线程是否结束
    SDL_TimerID timer_id;
    /////////////////////////////////////////////////////
    //// 跳转相关的变量
    int
    seek_req; //跳转标志 -- 读线程
    int64_t
    seek_pos; //跳转的位置 -- 微秒
    int
    seek_flag_audio;//跳转标志 -- 用于音频线程中
    int
    seek_flag_video;//跳转标志 -- 用于视频线程中
    double
    seek_time; //跳转的时间(秒)值和 seek_pos 是一样的
    //////////////////////////////////////////////////////
    int64_t start_time; //单位 微秒
    VideoState()
    {
        audio_clock = video_clock=start_time = 0;
    }
    VideoPlayer* m_player;//用于调用函数
} VideoState;

class VideoPlayer : public QThread
{
    Q_OBJECT
public:

//qt通过信号槽 实现qt 多线程通信
signals:
    void SIG_getOneImage(QImage img);//不传地址，使用图片的复制品
    void SIG_PlayerStateChanged(int flag);
    void SIG_TotalTime(qint64 uSec);
public:
    VideoPlayer();
    ~VideoPlayer()
    {
        stop(true);
    }
    void run();

public slots:
    void SendGetOneImage(QImage &img);

private:
    QString m_fileName;

    VideoState m_videoState;
    PlayerState m_playerState;
public:
    ///播放控制
    void play();
    void pause();
    void stop( bool isWait);
    void seek(int64_t pos);
    void setFileName(const QString &fileName);
    double getCurrentTime();
    int64_t getTotalTime();
    //为了完成播放的控制, 需要在 VideoPlayer 中添加成员 PlayerState m_playerState;播放状态是一个枚举,代表着播放状态.
    PlayerState playerState() const;

};

#endif // VIDEOPLAYER_H
