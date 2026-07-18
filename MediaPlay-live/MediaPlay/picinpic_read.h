#ifndef PICINPIC_READ_H
#define PICINPIC_READ_H
#include <QObject>
#include<QImage>
#include<QImageReader>
#include"common.h"
#include<QBuffer>
#include<QPainter>
#include<QTime>
#include<QTimer>
#include<QApplication>
#include<QDesktopWidget>
#include<QScreen>
#include<QThread>

class PicInPic_Read: public QThread //采集摄像头画面 + 桌面画面，完成 RGB→YUV420P 格式转换
{
    Q_OBJECT
public:
    explicit PicInPic_Read(QObject *parent = 0);
signals:
    void SIG_sendVideoFrame( QImage img ); // 用于预览
    void SIG_sendVideoFrameData(uint8_t* picture_buf, int buffer_size ); //采集的数据格式 YUV420P
    void SIG_sendPicInPic( QImage img ); //用于显示画中画
    void SIG_getDeskImg();
public slots:
    void slot_getVideoFrame(); //定时器周期获取画面
    void slot_openVideo(); //开启采集
    void slot_closeVideo(); //关闭采集
    void slot_getDeskImg();
private:
    void run();

    VideoCapture cap; //opencv 采集摄像头对象
    QTimer * timer; //定时器
    int ImageToYuvBuffer(QImage &image,uint8_t **buffer); //RGB24 转为 yuv420p

    bool isStop;
    QImage m_deskImg;
};
#endif // PICINPIC_READ_H
