#include "playerdialog.h"
#include "ui_playerdialog.h"
#include<QFileDialog>
#include <QDebug>

//#define _DEF_PATH "D:\\pingmujietu\\1.mp4"
//#define _DEF_PATH "rtmp://192.168.43.239:1935/vod//101.mp4"
#define _DEF_LIVE_PATH "rtmp://192.168.43.239:1935/videotest/100"
#define _DEF_PATH "http://111.40.196.9/PLTV/88888888/224/3221225628/index.m3u8"

PlayerDialog::PlayerDialog(QWidget *parent): QDialog(parent), ui(new Ui::PlayerDialog),
    m_currentPlayingVideoId(-1), m_userId(0)
{
    ui->setupUi(this);
    m_player=new VideoPlayer;
    connect(m_player,SIGNAL(SIG_getOneImage(QImage)),this,SLOT(slot_setOneImage(QImage)));

    slot_PlayerStateChanged(PlayerState::Stop);
    //m_player->setFileName(_DEF_PATH);
    //connect(&m_timer,SIGNAL(timeout()),this,SLOT());

    connect(m_player,SIGNAL(SIG_PlayerStateChanged(int)),
            this,SLOT(slot_PlayerStateChanged(int)));
    connect(m_player,SIGNAL(SIG_TotalTime(qint64)),
            this,SLOT(slot_getTotalTime(qint64)));
    connect(&m_timer,SIGNAL(timeout()),this,SLOT(slot_TimerTimeOut()));
    m_timer.setInterval(500);
    //安装事件过滤器，让该对象成为被观察对象  this去执行函数
    ui->slider_progress->installEventFilter(this);

    m_onlineDialog=new OnlineDialog();
    m_onlineDialog->hide();

    connect(m_onlineDialog,SIGNAL(SIG_PlayUrl(QString)),this,SLOT(slot_PlayUrl(QString)));
    connect(m_onlineDialog,SIGNAL(SIG_PlayVideoWithId(QString,int)),this,SLOT(slot_PlayVideoWithId(QString,int)));
    connect(m_onlineDialog,SIGNAL(SIG_LoginSuccess(int,QString)),this,SLOT(slot_cacheUserId(int,QString)));

    m_liveDialog = new LiveDialog(this);
    m_liveDialog->hide();

    // 让 LiveDialog 复用 OnlineDialog 已登录的 TCP 连接
    m_liveDialog->setTcpMediator(m_onlineDialog->getTcpMediator());

    // 登录成功后，把用户信息传给 LiveDialog
    connect(m_onlineDialog, SIGNAL(SIG_LoginSuccess(int,QString)),
            m_liveDialog, SLOT(setUserInfo(int,QString)));

    // 直播包由 OnlineDialog 识别后转发给 LiveDialog（buf 由 OnlineDialog 统一 delete）
    connect(m_onlineDialog, SIGNAL(SIG_LivePacket(char*,int)),
            m_liveDialog, SLOT(slot_handleLivePacket(char*,int)));

    // 观看直播：复用现有播放槽
    connect(m_liveDialog, SIGNAL(SIG_WatchLive(QString)),
            this, SLOT(slot_PlayUrl(QString)));
}

PlayerDialog::~PlayerDialog()
{
    reportPlaybackIfNeeded(); //窗口关闭前，把还在播的视频上报一下，不依赖ui，放在delete ui之前也安全

    delete ui;
    delete m_player;
    if(m_onlineDialog){
        delete m_onlineDialog;
        m_onlineDialog=NULL;
    }

    if (m_liveDialog) {
            delete m_liveDialog;
            m_liveDialog = NULL;
        }
}

//Qt 线程：QThread 定义子类 start() -> run()
//打开文件播放
void PlayerDialog::on_pb_start_clicked()
{
    //开始播放 ->一段时间内 获取图片
    //m_player->start();
    //打开浏览选择文件
    QString path=QFileDialog::getOpenFileName(this,"打开文件","./",
            "视频文件 (*.flv *.rmvb *.avi *.MP4 *.mkv);; 所有文件(*.*);;");
    //判断
    if(path.isEmpty()) return;
    //首先要关闭，判断当前的状态
    if(m_player->playerState()!=PlayerState::Stop)
    {
        m_player->stop(true);
    }
    reportPlaybackIfNeeded(); //上报上一个视频的观看情况（如果是可上报的点播视频）
    m_currentPlayingVideoId=-1; //本地文件没有服务端videoId，不参与热度上报

    //设置m_play fileName
    m_player->setFileName(path);
    //play
    m_player->start();
    slot_PlayerStateChanged(PlayerState::Playing);
}

void PlayerDialog::slot_setOneImage(QImage img)
{
    //pixmap和image的区别
    //实现视频加速渲染-OpenGL
    ui->wdg_show->slot_setImage(img);
}

void PlayerDialog::on_pb_resume_clicked()
{
    if(m_player->playerState()!=PlayerState::Pause) return;
    m_player->play();

    //切换
    ui->pb_resume->hide();
    ui->pb_pause->show();
}
void PlayerDialog::on_pb_pause_clicked()
{
    if(m_player->playerState()!=PlayerState::Playing) return;
    m_player->pause();

    //切换
    ui->pb_resume->show();
    ui->pb_pause->hide();
}
void PlayerDialog::on_pb_stop_clicked()
{
    m_player->stop(true);
}
void PlayerDialog::on_pb_live_clicked()
{
    m_liveDialog->show();
}
//播放状态切换槽函数
void PlayerDialog::slot_PlayerStateChanged(int state)
{
    switch( state )
    {
    case PlayerState::Stop:
        qDebug()<< "VideoPlayer::Stop";
        m_timer.stop();
        ui->slider_progress->setValue(0);
        ui->lb_totalTime->setText("00:00:00");
        ui->lb_curTime->setText("00:00:00");
        ui->pb_pause->hide();
        ui->pb_resume->show();
    {
        QImage img;
        img.fill( Qt::black);
        slot_setOneImage(img);
    }
        this->update();
        isStop = true;
        break;
    case PlayerState::Playing:
        qDebug()<< "VideoPlayer::Playing";
        ui->pb_resume->hide();
        ui->pb_pause->show();
        m_timer.start();
        this->update();
        isStop = false;
        break;
    }
}

void PlayerDialog::slot_getTotalTime(qint64 uSec)
{
    qint64 Sec = uSec/1000000;
    ui->slider_progress->setRange(0,Sec);//精确到秒
    QString hStr = QString("00%1").arg(Sec/3600);
    QString mStr = QString("00%1").arg(Sec/60);
    QString sStr = QString("00%1").arg(Sec%60);
    QString str =
    QString("%1:%2:%3").arg(hStr.right(2)).arg(mStr.right(2)).arg(sStr.right(2));
    ui->lb_totalTime->setText(str);
}
//获取当前视频时间定时器
void PlayerDialog::slot_TimerTimeOut()
{
    if (QObject::sender() == &m_timer)
    {
        qint64 Sec = m_player->getCurrentTime()/1000000;
        ui->slider_progress->setValue(Sec);
        QString hStr = QString("00%1").arg(Sec/3600);
        QString mStr = QString("00%1").arg(Sec/60%60);
        QString sStr = QString("00%1").arg(Sec%60);
        QString str =
                QString("%1:%2:%3").arg(hStr.right(2)).arg(mStr.right(2)).arg(sStr.right(2));
        ui->lb_curTime->setText(str);
        if(ui->slider_progress->value() == ui->slider_progress->maximum()
                && m_player->playerState() == PlayerState::Stop)
        {
            reportPlaybackIfNeeded(); //轮询发现自然播放结束，上报观看情况
            slot_PlayerStateChanged( PlayerState::Stop );
        }else if(ui->slider_progress->value() + 1
                 ==
                 ui->slider_progress->maximum()
                 && m_player->playerState() == PlayerState::Stop)
        {
            reportPlaybackIfNeeded();
            slot_PlayerStateChanged( PlayerState::Stop );
        }
    }
}

#include<QStyle>
#include<QMouseEvent>

bool PlayerDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->slider_progress ) {
        if ( event->type() == QEvent::MouseButtonPress ) {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
            int min = ui->slider_progress->minimum();
            int max = ui->slider_progress->maximum();
            int value = QStyle::sliderValueFromPosition(
            min,  max , mouseEvent->pos().x(), ui->slider_progress->width());
            m_timer.stop();
            ui->slider_progress->setValue(value);
            m_player->seek((qint64)value*1000000);
            m_timer.start();
            return true;
        } else {
            return false;
        }
    }
    //空格--暂停/恢复，左右--退回/快进，上下-音量调整
    return QDialog::eventFilter(obj,event);
}

//弹出网络模块
void PlayerDialog::on_pb_online_clicked()
{
    m_onlineDialog->show();
}
//播放url（直播观看走这条，不带videoId，不参与热度上报）
void PlayerDialog::slot_PlayUrl(QString url)
{
    if(url.isEmpty()) return;
    //首先要关闭，判断当前的状态
    if(m_player->playerState()!=PlayerState::Stop)
    {
        m_player->stop(true);
    }
    reportPlaybackIfNeeded(); //切到直播前，上报上一个点播视频的观看情况
    m_currentPlayingVideoId=-1; //直播没有videoId，不参与热度上报

    //设置m_play fileName
    m_player->setFileName(url);
    //play
    m_player->start();

    slot_PlayerStateChanged(PlayerState::Playing);
}

//播放推荐/上传历史里的点播视频，带videoId，用于播放结束后上报观看时长/完播率
void PlayerDialog::slot_PlayVideoWithId(QString url, int videoId)
{
    if(url.isEmpty()) return;
    if(m_player->playerState()!=PlayerState::Stop)
    {
        m_player->stop(true);
    }
    reportPlaybackIfNeeded(); //上报上一个视频的观看情况

    m_currentPlayingVideoId=videoId;
    m_player->setFileName(url);
    m_player->start();
    slot_PlayerStateChanged(PlayerState::Playing);
}

//登录成功后缓存userId，上报播放记录时要带上
void PlayerDialog::slot_cacheUserId(int userId, QString userName)
{
    m_userId=userId;
}

//把当前正在播的点播视频的观看时长/完播率上报给服务端，非点播场景（本地文件/直播/尚未播放）直接跳过
void PlayerDialog::reportPlaybackIfNeeded()
{
    if(m_currentPlayingVideoId<=0) return;
    if(m_userId<=0) return; //没登录没法带userId上报

    qint64 totalUsec=m_player->getTotalTime();
    qint64 curUsec=m_player->getCurrentTime();
    if(totalUsec<=0)
    {
        //没真正打开成功过（比如打开失败），没什么好上报的
        m_currentPlayingVideoId=-1;
        return;
    }

    int totalSec=(int)(totalUsec/1000000);
    int watchSec=(int)(curUsec/1000000);
    if(watchSec>totalSec) watchSec=totalSec; //防止时钟误差导致"看的比总时长还长"

    STRU_PLAY_REPORT_RQ rq;
    rq.m_nType=_DEF_PACK_PLAY_REPORT_RQ; //构造函数只是memset清零，必须手动设置包类型，否则服务端按 type>=_DEF_PACK_BASE 校验会直接丢包
    rq.m_nUserId=m_userId;
    rq.m_nVideoId=m_currentPlayingVideoId;
    rq.m_nWatchSeconds=watchSec;
    rq.m_nTotalSeconds=totalSec;
    m_onlineDialog->getTcpMediator()->SendData(0,(char*)&rq,sizeof(rq));

    m_currentPlayingVideoId=-1; //防止同一段播放被重复上报
}

