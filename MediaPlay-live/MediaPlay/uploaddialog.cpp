#include "uploaddialog.h"
#include "ui_uploaddialog.h"
#include"QFileInfo"
#include"QFileDialog"
#include"QDebug"
#include"videoplayer.h"
#include <QProcess>
#include <QTime>
#include<QDir>
#include<QMovie>
#include<QMessageBox>

UploadDialog::UploadDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::UploadDialog)
{
    ui->setupUi(this);

    qRegisterMetaType<Hobby>("Hobby");

    m_recorderDialog=new RecorderDialog();

}

UploadDialog::~UploadDialog()
{
    delete ui;
}

//点击浏览
void UploadDialog::on_pb_view_clicked()
{
    QString path=QFileDialog::getOpenFileName(this,"打开","./","(*.flv *.mp4)");
    if(path.isEmpty()) return;
    ui->le_path->setText(path);

    QDir dir;
    if(!dir.exists(QDir::currentPath()+"/temp/"))
        dir.mkpath(QDir::currentPath()+"/temp/");

    //显示缩略图
    QString imgPath=SaveVideoJpg(path);
    if(imgPath.isEmpty())
    {
        // 缩略图生成失败（比如视频太短/关键帧太少），不能让用户带着空GIF去上传
        QMessageBox::about(this,"提示","视频缩略图生成失败，请更换视频文件");
        ui->pb_beginupload->setEnabled(false);
        return;
    }
    QFileInfo info(path);

   //加载到控件
    QMovie *movie=new QMovie(imgPath);
    ui->lb_gif->setMovie(movie);
    movie->start();

    m_filePath=path;
    m_imgPath=imgPath;
    ui->pb_beginupload->setEnabled(true);
}

QString UploadDialog::SaveVideoJpg( QString FilePath )
{
    std::string tmp = FilePath.toStdString();
    char* file_path = (char*)tmp.c_str();
    AVFormatContext *pFormatCtx;
    AVCodecContext *pCodecCtx;
    AVCodec *pCodec;
    AVFrame *pFrame, *pFrameRGB;
    AVPacket *packet;
    uint8_t *out_buffer;
    static struct SwsContext *img_convert_ctx;
    int videoStream, i, numBytes;
    int ret, got_picture;
    av_register_all(); //初始化 FFMPEG调用了这个才能正常适用编码器和解码器
    //Allocate an AVFormatContext.
    pFormatCtx = avformat_alloc_context();
    if (avformat_open_input(&pFormatCtx, file_path, NULL, NULL) != 0) {

        qDebug() << "can't open the file." ;

        return QString ();
    }
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {

        qDebug() <<"Could't find stream infomation.";

        return QString ();
    }
    videoStream = -1;

    ///循环查找视频中包含的流信息，直到找到视频类型的流
    ///便将其记录下来 保存到 videoStream 变量中///这里现在只处理视频流音频流先不管他
    for (i = 0; i < pFormatCtx->nb_streams; i++) {

        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {

            videoStream = i;

        }
    }
    ///如果 videoStream 为-1 说明没有找到视频流
    if (videoStream == -1) {

        qDebug() <<"Didn't find a video stream.";

        return QString ();
    }

    ///查找解码器
    pCodecCtx = pFormatCtx->streams[videoStream]->codec;
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if (pCodec == NULL) {

        qDebug() <<"Codec not found." ;

        return QString ();
    }

    ///打开解码器
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {

        qDebug() <<"Could not open codec." ;

        return QString ();
    }
    pFrame = av_frame_alloc();
    pFrameRGB = av_frame_alloc();
    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,

                                     pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,

                                     AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);
    numBytes = avpicture_get_size(AV_PIX_FMT_RGB32,
                                  pCodecCtx->width,pCodecCtx->height);
    out_buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    avpicture_fill((AVPicture *) pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,

                   pCodecCtx->width, pCodecCtx->height);
    int y_size = pCodecCtx->width * pCodecCtx->height;
    packet = (AVPacket *) malloc(sizeof(AVPacket)); //分配一个 packet
    av_new_packet(packet, y_size); //分配 packet 的数据
    int index = 0;
    int ncount=0;
    while(1)
    {
        if (av_read_frame(pFormatCtx, packet) < 0)
        {
            break; //这里认为视频读取完了
        }

        if (packet->stream_index == videoStream) {
            ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture,packet);
            if (ret < 0) {
                qDebug() <<"decode error." ;
                return QString ();
            }
            if (got_picture && pFrame->key_frame == 1 && pFrame->pict_type ==AV_PICTURE_TYPE_I) {
                sws_scale(img_convert_ctx,(uint8_t const * const *) pFrame->data,
                          pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);

                QImage tmpImg((uchar*)out_buffer,pCodecCtx->width,pCodecCtx->height,QImage::Format_RGB32);
                QImage image = tmpImg.scaled(640 ,320 ,Qt::KeepAspectRatio );

                image.save(QString("./temp/%1.jpg").arg( ncount ,2 ,10,QChar('0') ) );
                ncount++;
                if( ncount == 6 ) {
                    av_free_packet(packet);
                    break;
                }
            }
        }
        av_free_packet(packet);
    }

    // 兜底：视频太短/关键帧太少时，上面的循环可能一张关键帧都抓不到（ncount==0）
    // 重新从头解码，这次不再要求必须是关键帧，保证短视频也能出缩略图，而不是静默生成空文件
    if(ncount < 6)
    {
        av_seek_frame(pFormatCtx, videoStream, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(pCodecCtx);
        while(ncount < 6)
        {
            if (av_read_frame(pFormatCtx, packet) < 0)
            {
                break; //整个视频都读完了还是凑不够6张，有多少算多少
            }
            if (packet->stream_index == videoStream) {
                ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
                if (ret >= 0 && got_picture) {
                    sws_scale(img_convert_ctx,(uint8_t const * const *) pFrame->data,
                              pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);

                    QImage tmpImg((uchar*)out_buffer,pCodecCtx->width,pCodecCtx->height,QImage::Format_RGB32);
                    QImage image = tmpImg.scaled(640 ,320 ,Qt::KeepAspectRatio );

                    image.save(QString("./temp/%1.jpg").arg( ncount ,2 ,10,QChar('0') ) );
                    ncount++;
                }
            }
            av_free_packet(packet);
        }
    }

    av_free(out_buffer);
    av_free(pFrameRGB);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);

    if(ncount == 0)
    {
        // 一张画面都没抽出来（视频本身有问题/时长太短/解码异常），没必要再往下调ffmpeg合成gif
        qDebug() << "SaveVideoJpg: 未能提取到任何画面帧，视频可能损坏或过短:" << FilePath;
        return QString();
    }

    QProcess p(0);
    p.start("cmd");//开启 cmd 窗口
    p.waitForStarted();
    QString curPath = QDir::currentPath();
    curPath.replace("/","\\\\");
    QString strcd = QString("%1/ \n").arg(QCoreApplication::applicationDirPath());
    strcd.replace("/","\\\\");
    strcd = QString("cd /d ")+strcd;

    p.write( strcd.toLocal8Bit() );

    QString imgName = QString("%1\\\\temp\\\\%2.gif")
            .arg(curPath).arg(QTime::currentTime().toString("hhmmsszzz"));

    QString cmd = QString("ffmpeg -r 2 -i %1\\\\temp\\\\").arg(curPath);

    cmd += "%02d.jpg ";

    cmd += imgName;

    cmd += " \n";

    qDebug() << "cmd "<< cmd;

    p.write( cmd.toLocal8Bit() );

    p.closeWriteChannel();

    p.waitForFinished();

    qDebug()<< QString::fromLocal8Bit( p.readAllStandardOutput() );

    p.close();

    // 校验ffmpeg是否真的把gif内容写出来了，避免把一个0字节的空文件当成"生成成功"往下传
    QFileInfo gifInfo(imgName);
    if(!gifInfo.exists() || gifInfo.size() == 0)
    {
        qDebug() << "SaveVideoJpg: GIF生成失败，ffmpeg输出为空:" << imgName;
        return QString();
    }

    return imgName;
}

//开始上传
void UploadDialog::on_pb_beginupload_clicked()
{
    // ★ 新增：上传前校验兴趣标签——8 个复选框一个都没勾，就弹窗提示并拦截上传
    bool bTagSelected = ui->cb_food->isChecked()    || ui->cb_funny->isChecked()  ||
                        ui->cb_ennegy->isChecked()  || ui->cb_dance->isChecked()  ||
                        ui->cb_music->isChecked()   || ui->cb_video->isChecked()  ||
                        ui->cb_outside->isChecked() || ui->cb_edu->isChecked();
    if (!bTagSelected)
    {
        // 弹窗提醒用户必须先选标签；用 about 是信息框（只有"确定"），不会误触上传
        QMessageBox::about(this, "提示", "请选择你的视频标签");
        return;  // 直接返回，不收集、不发送上传请求
    }

    Hobby hy;
    hy.dance    =ui->cb_dance   ->isChecked()?1:0;
    hy.edu      =ui->cb_edu     ->isChecked()?1:0;
    hy.ennegy   =ui->cb_ennegy  ->isChecked()?1:0;
    hy.food     =ui->cb_food    ->isChecked()?1:0;
    hy.funny    =ui->cb_funny   ->isChecked()?1:0;
    hy.music    =ui->cb_music   ->isChecked()?1:0;
    hy.outside  =ui->cb_outside ->isChecked()?1:0;
    hy.video    =ui->cb_video   ->isChecked()?1:0;

    emit SIG_UploadFile(m_filePath,m_imgPath,hy);
}
//更新进度条
void UploadDialog::slot_updateProcess(qint64 cur, qint64 max)
{
    ui->pgb_upload->setMaximum(max);
    ui->pgb_upload->setValue(cur);
}

void UploadDialog::clear(){
    ui->le_path->setText("");
    ui->pb_beginupload->setEnabled(false);
    emit SIG_GetOnePicture(QImage());
    ui->pgb_upload->setValue(0);
}

void UploadDialog::on_pb_page2_clicked()
{
    m_recorderDialog->show();
    m_recorderDialog->raise();
    m_recorderDialog->activateWindow();
}


void UploadDialog::on_pb_page1_clicked()
{
    on_pb_view_clicked();
}

