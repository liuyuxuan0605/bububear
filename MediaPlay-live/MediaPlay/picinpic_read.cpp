#include "picinpic_read.h"
//视频采集 + RGB→YUV 转换
PicInPic_Read::PicInPic_Read(QObject*parent): QThread(parent)
{
    m_targetW = 0;
    m_targetH = 0;
    timer = new QTimer(this);
    connect( timer , SIGNAL(timeout()) , this , SLOT(slot_getVideoFrame()));

    connect(this,SIGNAL(SIG_getDeskImg()),this,SLOT(slot_getDeskImg()),Qt::BlockingQueuedConnection);
}
void PicInPic_Read::setTargetSize(int w, int h)
{
    m_targetW = w;
    m_targetH = h;
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
    // 用 primaryScreen 抓主屏幕（winId=0 即整个主屏幕），
    // 替代已废弃且易返回空的 QApplication::desktop()->winId()。
    QScreen *src = QApplication::primaryScreen();
    if (!src) return;
    QPixmap map = src->grabWindow(0);
    if (map.isNull()) return;   // 抓取失败（多屏/高DPI/主窗口最小化等）→ 不更新 m_deskImg
    //转化为 RGB24 QImage
    QImage image = map.toImage().convertToFormat(QImage::Format_RGB888);
    // ※ 关键修复：高 DPI 缩放下 grabWindow 抓到的是物理像素，而编码器尺寸是逻辑像素
    //   （来自 QScreen::geometry），两者不一致会导致编码时 memcpy 越界崩溃。
    //   这里把抓到的图缩放对齐到编码器目标尺寸，保证与 frameBuffer 严格一致。
    if (m_targetW > 0 && m_targetH > 0 &&
        (image.width() != m_targetW || image.height() != m_targetH)) {
        image = image.scaled(m_targetW, m_targetH,
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
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
    // ※ 防御：桌面截图可能因多屏/高DPI/主窗口最小化而抓空，
    //   此时 m_deskImg 为空或尺寸为 0，直接进入 ImageToYuvBuffer 会
    //   sws_getContext(0,0,...) 返回 NULL → sws_scale(NULL) 崩溃。
    //   这里直接跳过编码这一帧，仅发预览，等下一帧拿到有效桌面图再编码。
    if (image.isNull() || image.width()==0 || image.height()==0) {
        Q_EMIT SIG_sendVideoFrame( image );
        return;
    }
    // ※ 双保险：若桌面图尺寸与目标不一致（极端情况下上一帧残留等），
    //   先缩放对齐再编码，彻底杜绝 memcpy 越界。
    QImage encImg = image;
    if (m_targetW > 0 && m_targetH > 0 &&
        (encImg.width() != m_targetW || encImg.height() != m_targetH)) {
        encImg = encImg.scaled(m_targetW, m_targetH,
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    uint8_t * picture_buf = NULL;
    int buffer_size = ImageToYuvBuffer( encImg , &picture_buf );
    if (buffer_size <= 0 || !picture_buf) {
        // 转换失败（双保险），不编码空帧
        Q_EMIT SIG_sendVideoFrame( image );
        return;
    }
    Q_EMIT SIG_sendVideoFrameData( picture_buf, buffer_size );
    Q_EMIT SIG_sendVideoFrame( image );
}
int PicInPic_Read::ImageToYuvBuffer( QImage& image , uint8_t ** buffer )
{
    // ※ 入口防护：空图或尺寸为 0 直接返回 -1，调用方据此跳过编码
    if (image.isNull() || image.width()==0 || image.height()==0) return -1;
    int w = image.width();
    int h = image.height();
    int y_size = w * h;
    // image.invertPixels(QImage::InvertRgb);
    //==================== 创建 YUV 对应的空间 =========================
    AVFrame *pFrameYUV = av_frame_alloc();
    int numBytes2 = avpicture_get_size(AV_PIX_FMT_YUV420P, w, h);
    uint8_t *buffer2 = (uint8_t *)av_malloc(numBytes2*sizeof(uint8_t));
    avpicture_fill((AVPicture *)pFrameYUV, buffer2, AV_PIX_FMT_YUV420P, w, h);
    //==================== 创建转化器 ================================
    SwsContext *rgb_to_yuv_ctx = sws_getContext(w,h, AV_PIX_FMT_RGB24,
                                 w,h,AV_PIX_FMT_YUV420P,
                                 SWS_BICUBIC, NULL,NULL,NULL);
    // ※ 关键修复：原代码把 pFrameRGB->data[0] 指向 image.bits()，但 sws 源 linesize
    //   用的是 avpicture_fill 给的 w*3（无行对齐）；而 QImage Format_RGB888 底层会做
    //   4 字节行对齐，bytesPerLine() 可能是 ((w*3+3)&~3)。两者不一致 → sws_scale
    //   （尤其 SWS_BICUBIC 插值）逐行错位读取，越界访问 image 内存外 → 崩溃。
    //   改为直接用 image 的位图 + 真实 bytesPerLine 作为 sws 源，并去掉多余 RGB 中间帧。
    uint8_t *srcData[1] = { (uint8_t*)image.bits() };
    int srcLinesize[1] = { (int)image.bytesPerLine() };
    if (!rgb_to_yuv_ctx) {   // 尺寸非法(如奇数)导致转换器创建失败 → 避免 sws_scale(NULL) 崩溃
        av_free(buffer2);
        av_free(pFrameYUV);
        return -1;
    }
    sws_scale(rgb_to_yuv_ctx, srcData, srcLinesize, 0, h, pFrameYUV->data, pFrameYUV->linesize);
    //将转换完的数据提取到缓冲区
    uint8_t * picture_buf = (uint8_t *)av_malloc(numBytes2);
    memcpy(picture_buf,pFrameYUV->data[0],y_size);
    memcpy(picture_buf+y_size,pFrameYUV->data[1],y_size/4);
    memcpy(picture_buf+y_size+y_size/4,pFrameYUV->data[2],y_size/4);
    //写返回空间
    *buffer = picture_buf;
    sws_freeContext(rgb_to_yuv_ctx);
    // ※ 关键修复（真凶）：pFrameYUV->data[1]/data[2] 经 avpicture_fill 指向 buffer2 的内部偏移，
    //   若用 av_frame_free(&pFrameYUV) 释放，它会逐个对 data[0..2] 调 av_free：
    //   data[1]/data[2] 是 buffer2 中段地址 → 对堆中段地址 free = 堆损坏
    //   （表现：直播几秒后崩、录制直接崩，且极难定位）。
    //   正确做法：手动释放真实存储 buffer2，再用 av_free 释放帧结构体（C 释放不碰 data 指针）。
    av_free(buffer2);
    av_free(pFrameYUV);
    return y_size*3/2;
}
