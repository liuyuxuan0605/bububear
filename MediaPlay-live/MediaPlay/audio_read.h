#ifndef AUDIO_READ_H
#define AUDIO_READ_H
#include <QObject>
#include "common.h"
#include<QTimer>
#include<vector>
#include<QMessageBox>
#include<QTime>
#include<QAudioInput>
#include<QAudioFormat>
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000//// 1 second of 48khz 32bit audio
//计算方法采样率 * 采样时间间隔 * 一次采样字节数 = 数据量
//单通道 16 位 一次采样 2 字节 双通道 16 位 一次采样 4 字节
//aac 数据是一帧 1024 采样点 ffmpeg 使用 fltp 是 float 一个点 双声道*2 就是*8 即 8192 字节
//PCM 采样 可以是 48kHz 或者 44.1kHz , 以 1024 点为例, 换算
//48kHz : 1000 = 1024 : xx为21.3我们去采样时间间隔为 20ms
//单声道 队列投递一次字节数
#define OneAudioSize (4096)
//采样频率
#define AudioCollectFrequency (44100)
//采样间隔
#define AUDIO_INTERVAL (20)
//定时不准 一般延迟一些
//读一帧可能会有噪音 读两帧试试
//一次读取 8 帧 32768 4096
#define FrameOnce (1)
//声道数
#define AudioChannelCount (2)

class Audio_Read : public QObject
{
    Q_OBJECT
signals:
    void SIG_sendAudioFrameData( uint8_t* buf, int buffer_size);
public:
    Audio_Read();
    ~Audio_Read();
public slots:
    void slot_readMore();//定时获取音频数据投递到队列
    void slot_closeAudio(); //暂停采集
    void slot_openAudio(); //开始采集
    void UnInit(); //回收
private:
    QAudioInput* audio_in; //Qt 音频采集对象
    QIODevice *myBuffer_in;//IO 设备, 读写数据
    QAudioFormat format; //音频设备格式
    //采集状态
    enum ENUM_AUDIO_STATE{state_stop , state_play , state_pause };
    int m_playState;
    QTimer *timer;//定时器
    SwrContext *swr; // 用于音频的转换
    int nb_samples;
    std::vector<uint8_t> m_audiobuff;
    //缓冲区排队执行
    int m_buffPos; // 未取走数据的起始位置
};
#endif // AUDIO_READ_H
