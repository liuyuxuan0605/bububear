#include "picinpic_read.h"
//视频采集 + RGB→YUV 转换
PicInPic_Read::PicInPic_Read(QObject*parent): QThread(parent)
{
    timer = new QTimer(this);
    connect( timer , SIGNAL(timeout()) , this , SLOT(slot_getVideoFrame()));

    connect(this,SIGNAL(SIG_getDeskImg()),this,SLOT(slot_getDeskImg()),Qt::BlockingQueuedConnection);
}
void PicInPic_Read::slot_openVideo()
{
    cap.open(0);
    if(!cap.isOpened()){
        QMessageBox::information(NULL,tr("提示"),tr("视频没有打开"));
        return;
    }
    //宁多勿少（多了丢弃，少了需要再编码）
   // timer->start(1000/FRAME_RATE - 10 );
    this->start();
    isStop=false;
}
void PicInPic_Read::slot_closeVideo()
{
    //timer->stop();
    isStop=true;
    if(cap.isOpened())
        cap.release();
}

void PicInPic_Read::slot_getDeskImg()
{
    QScreen *src = QApplication::primaryScreen();
    QPixmap map = src->grabWindow( QApplication::desktop()->winId());
    //转化为 RGB24 QImage
    QImage image = map.toImage().convertToFormat(QImage::Format_RGB888);
    m_deskImg=image.copy();
}

void PicInPic_Read::run()
{
    int diff=1000/FRAME_RATE;
    int count=0;
    qint64 beginTime=QDateTime::currentMSecsSinceEpoch();
    while(1){
        if(isStop) break;
        slot_getVideoFrame();
        count++;
        qint64 curTime=QDateTime::currentMSecsSinceEpoch();
        while(curTime-beginTime<count*diff){
            if(isStop) break;

            QThread::msleep(5);
            curTime=QDateTime::currentMSecsSinceEpoch();
        }
    }
}
void PicInPic_Read::slot_getVideoFrame()
{
    qDebug()<<"time:"<<QTime::currentTime().toString("mm:ss zzz");
    Mat frame;
    if( !cap.read(frame) )
    {
        return;
    }
    cvtColor(frame,frame,CV_BGR2RGB);
    QImage iconImage((unsigned const char*)frame.data,frame.cols,frame.rows,QImage::Format_RGB888);
   // iconImage = iconImage.scaled( QSize(320,240) ,Qt::KeepAspectRatio );
    //投递画中画图片
    Q_EMIT SIG_sendPicInPic( iconImage.copy() );
    //获取桌面截图
    //QScreen *src = QApplication::primaryScreen();
   // QPixmap map = src->grabWindow( QApplication::desktop()->winId());
    //转化为 RGB24 QImage
   // QImage image = map.toImage().convertToFormat(QImage::Format_RGB888);
    Q_EMIT SIG_getDeskImg();
    QImage &image=m_deskImg;
    //添加鼠标
   // QPainter painter(&image);
   // painter.drawImage(QCursor::pos(),QImage(":/images/cursor.png"));
   // painter.end();

    //计算视频帧
    //long long time = 0;
    uint8_t * picture_buf = NULL;
    int buffer_size = ImageToYuvBuffer( image , &picture_buf );
    Q_EMIT SIG_sendVideoFrameData( picture_buf, buffer_size );
    Q_EMIT SIG_sendVideoFrame( image );
}
int PicInPic_Read::ImageToYuvBuffer( QImage& image , uint8_t ** buffer )
{
    int w = image.width();
    int h = image.height();
    int y_size = w * h;
    // image.invertPixels(QImage::InvertRgb);
    //==================== 创建 RGB 对应的空间 =========================
    AVFrame *pFrameRGB = av_frame_alloc();
    // Determine required buffer size and allocate buffer
    int numBytes1 = avpicture_get_size(AV_PIX_FMT_RGB24, w, h);
    uint8_t *buffer1 = (uint8_t *)av_malloc(numBytes1*sizeof(uint8_t));
    avpicture_fill((AVPicture *)pFrameRGB, buffer1, AV_PIX_FMT_RGB24, w, h);
    pFrameRGB->data[0] = image.bits();
    //==================== 创建 YUV 对应的空间 =========================
    AVFrame *pFrameYUV = av_frame_alloc();
    // Determine required buffer size and allocate buffer
    int numBytes2 = avpicture_get_size(AV_PIX_FMT_YUV420P, w, h);
    uint8_t *buffer2 = (uint8_t *)av_malloc(numBytes2*sizeof(uint8_t));
    avpicture_fill((AVPicture *)pFrameYUV, buffer2, AV_PIX_FMT_YUV420P, w, h);
    //
    //qWarning() << "numBytes2 " << numBytes2;
    //==================== 创建转化器 ================================
    SwsContext *rgb_to_yuv_ctx = sws_getContext(w,h, AV_PIX_FMT_RGB24,
                                 w,h,AV_PIX_FMT_YUV420P,
                                 SWS_BICUBIC, NULL,NULL,NULL);
    sws_scale(rgb_to_yuv_ctx, pFrameRGB->data, pFrameRGB->linesize, 0,h,pFrameYUV->data, pFrameYUV->linesize);
    //将转换完的数据提取到缓冲区
    uint8_t * picture_buf = (uint8_t *)av_malloc(numBytes2);
    memcpy(picture_buf,pFrameYUV->data[0],y_size);
    memcpy(picture_buf+y_size,pFrameYUV->data[1],y_size/4);
    memcpy(picture_buf+y_size+y_size/4,pFrameYUV->data[2],y_size/4);
    //写返回空间
    *buffer = picture_buf;
    //qWarning() << rc << endl;
    sws_freeContext(rgb_to_yuv_ctx);
    av_free(buffer1);
    av_free(buffer2);
    av_free(pFrameRGB);
    av_free(pFrameYUV);
    return y_size*3/2;
}
