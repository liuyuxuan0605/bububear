#include "recorderdialog.h"
#include "ui_recorderdialog.h"

RecorderDialog::RecorderDialog(QWidget *parent)
: QDialog(parent), ui(new Ui::RecorderDialog)
{
    ui->setupUi(this);
    m_picturewidget=new PictureWidget;  //显示画中画的预览窗口
    m_picturewidget->hide();
    m_picturewidget->move(0,0);

    this->setWindowFlags(Qt::WindowMinMaxButtonsHint|Qt::WindowCloseButtonHint);

    m_saveFileThread=new SaveVideoFileThread;  //编码线程的初始化
    //将编码线程中产生的预览帧（摄像头 / 桌面）绑定到 UI 控件显示，仅用于预览（显示画面），不参与编码。
    connect( m_saveFileThread,SIGNAL(SIG_sendPicInPic( QImage )),m_picturewidget,SLOT(slot_setImage(QImage)));
    connect( m_saveFileThread,SIGNAL(SIG_sendVideoFrame( QImage )),this,SLOT(slot_setImage(QImage)));
}

RecorderDialog::~RecorderDialog()
{
    delete ui;
}


void RecorderDialog::on_pb_start_clicked()
{

     this->showMinimized();
     m_picturewidget->show();

     STRU_AV_FORMAT format;
     format.fileName = m_saveUrl;
     format.frame_rate = FRAME_RATE;
     format.hasAudio = true;
     format.hasCamera = true;
     format.hasDesk = true;
     format.videoBitRate = 800000;

     //采样频率 44100
     //码率 64000
     //声道 2
     //精度位数 16
     //格式 flpt aac

     QScreen *src = QApplication::primaryScreen();
     QRect rect = src->geometry();
     format.width = rect.width();
     format.height = rect.height();

     m_saveFileThread->slot_setInfo(format);
     m_saveFileThread->slot_openVideo();

}


void RecorderDialog::on_pb_stop_clicked()
{
     m_picturewidget->hide();
     m_saveFileThread->slot_closeVideo();
}


void RecorderDialog::on_pb_setUrl_clicked()
{
    m_saveUrl=ui->le_url->text();
    m_saveUrl=m_saveUrl.replace("/","\\");
}

void RecorderDialog::slot_setImage(QImage img)
{
    QPixmap pixmap;
    if(!img.isNull()){
        pixmap=QPixmap::fromImage(img.scaled(ui->lb_showImage->size(),Qt::KeepAspectRatio));
    }else{
        pixmap=QPixmap::fromImage(img);
    }
    ui->lb_showImage->setPixmap(pixmap);
}
