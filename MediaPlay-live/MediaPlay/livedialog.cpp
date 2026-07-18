#include "livedialog.h"
#include "ui_livedialog.h"
#include <QMessageBox>
#include <QScreen>
#include <QApplication>
#include "common.h"   // FRAME_RATE

LiveDialog::LiveDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::LiveDialog),m_tcp(nullptr), m_userId(-1), m_isLiving(false),m_liveThread(nullptr)
{
    ui->setupUi(this);

    setWindowTitle("直播");
    ui->pb_stopLive->setEnabled(false);

    // 预览窗口（和 RecorderDialog 构造函数里的写法一致）
    m_previewWidget = new PictureWidget;
    m_previewWidget->hide();
    m_previewWidget->move(0, 0);
}

LiveDialog::~LiveDialog()
{
    // 停播时确保线程彻底退出再析构
    stopLivePush();

    for (auto* info : m_liveMap) delete info;
    delete m_previewWidget;

    delete ui;
}

void LiveDialog::setUserInfo(int userId, QString userName)
{
    m_userId   = userId;
    m_userName = userName;
}

void LiveDialog::setTcpMediator(TcpClientMediator* tcp)
{
    m_tcp = tcp;
}

// -------- 开始直播 --------
void LiveDialog::on_pb_startLive_clicked()
{
    if (m_userId < 0) { QMessageBox::warning(this,"提示","请先登录"); return; }
    if (!m_tcp)       { QMessageBox::warning(this,"提示","网络未连接"); return; }

    QString title = ui->le_title->text().trimmed();
    if (title.isEmpty()) title = m_userName + "的直播间";

    STRU_LIVE_START_RQ rq;
    rq.m_nType   = _DEF_PACK_LIVE_START_RQ;
    rq.m_nUserId = m_userId;
    strncpy(rq.m_szTitle, title.toUtf8().data(), 127);
    rq.m_szTitle[127] = '\0';

    m_tcp->SendData(0, (char*)&rq, sizeof(rq));
}

// -------- 停止直播 --------
void LiveDialog::on_pb_stopLive_clicked()
{
    if (!m_isLiving) return;

    // 先停本地推流（closeVideo + wait + delete）
    stopLivePush();

    // 再通知服务端
    STRU_LIVE_STOP_RQ rq;
    rq.m_nType   = _DEF_PACK_LIVE_STOP_RQ;
    rq.m_nUserId = m_userId;
    m_tcp->SendData(0, (char*)&rq, sizeof(rq));
}

// -------- 刷新直播列表 --------
void LiveDialog::on_pb_refresh_clicked()
{
    refreshLiveList();
}

void LiveDialog::refreshLiveList()
{
    for (auto* info : m_liveMap) delete info;
    m_liveMap.clear();
    ui->lw_liveList->clear();

    STRU_LIVE_LIST_RQ rq;
    rq.m_nType   = _DEF_PACK_LIVE_LIST_RQ;
    rq.m_nUserId = m_userId;
    m_tcp->SendData(0, (char*)&rq, sizeof(rq));
}

// -------- 观看直播 --------
void LiveDialog::on_pb_watch_clicked()
{
    int row = ui->lw_liveList->currentRow();
    if (row < 0) { QMessageBox::warning(this,"提示","请先选择一路直播"); return; }

    QListWidgetItem* item = ui->lw_liveList->currentItem();
    int streamId = item->data(Qt::UserRole).toInt();
    if (!m_liveMap.contains(streamId)) return;

    LiveInfo* info = m_liveMap[streamId];
    QString rtmpUrl = QString("rtmp://%1:1935/videotest/%2")
                      .arg(DEF_SSERVER_IP).arg(info->streamKey);
    emit SIG_WatchLive(rtmpUrl);
}

// -------- 接收 OnlineDialog 转发出来的直播包 --------
// 注意：buf 由 OnlineDialog 在 slot_ReadyData 末尾统一 delete[]，
// 这里只读取数据，不要 delete，否则会 double free。
void LiveDialog::slot_handleLivePacket(char* buf, int /*nlen*/)
{
    PackType type = *(PackType*)buf;

    if (type == _DEF_PACK_LIVE_START_RS) {
        STRU_LIVE_START_RS* rs = (STRU_LIVE_START_RS*)buf;
        if (rs->m_nResult == 1) {
            m_isLiving         = true;
            m_currentStreamKey = QString::fromUtf8(rs->m_szStreamKey);
            ui->pb_startLive->setEnabled(false);
            ui->pb_stopLive->setEnabled(true);

            // 服务端确认开播后，本地开始推流
            QString rtmpPushUrl = QString("rtmp://%1:1935/videotest/%2")
                                  .arg(DEF_SSERVER_IP).arg(m_currentStreamKey);
            startLivePush(rtmpPushUrl);
            QMessageBox::information(this, "提示", "直播已开始！");
        } else {
            QMessageBox::warning(this, "提示", "开启直播失败");
        }
    }
    else if (type == _DEF_PACK_LIVE_STOP_RS) {
        STRU_LIVE_STOP_RS* rs = (STRU_LIVE_STOP_RS*)buf;
        if (rs->m_nResult == 1) {
            m_isLiving = false;
            ui->pb_startLive->setEnabled(true);
            ui->pb_stopLive->setEnabled(false);
            QMessageBox::information(this, "提示", "直播已结束");
        }
    }
    else if (type == _DEF_PACK_LIVE_LIST_RS) {
        STRU_LIVE_LIST_RS* rs = (STRU_LIVE_LIST_RS*)buf;
        LiveInfo* info = new LiveInfo;
        info->streamId   = rs->m_nStreamId;
        info->userId     = rs->m_nUserId;
        info->anchorName = QString::fromUtf8(rs->m_szAnchorName);
        info->title      = QString::fromUtf8(rs->m_szTitle);
        info->streamKey  = QString::fromUtf8(rs->m_szStreamKey);
        m_liveMap[info->streamId] = info;
        appendLiveItem(info);
    }
    else if (type == _DEF_PACK_LIVE_LIST_END) {
        STRU_LIVE_LIST_END* end = (STRU_LIVE_LIST_END*)buf;
        if (end->m_nCount == 0) {
            ui->lw_liveList->addItem("暂无正在进行的直播");
        }
    }
    // 不 delete[] buf
}

void LiveDialog::appendLiveItem(LiveInfo* info)
{
    QString text = QString("[%1] %2").arg(info->anchorName).arg(info->title);
    QListWidgetItem* item = new QListWidgetItem(text);
    item->setData(Qt::UserRole, info->streamId);
    ui->lw_liveList->addItem(item);
}

// ================================================================
// 核心：开播 new 独立 SaveVideoFileThread，停播 wait+delete
// 与 RecorderDialog 的 SaveVideoFileThread 完全独立，互不干扰
// ================================================================
void LiveDialog::startLivePush(QString rtmpUrl)
{
    // 安全起见：如果上一个没清理干净，先停掉
    if (m_liveThread) {
        stopLivePush();
    }

    // 每次开播 new 一个全新的实例
    // isStop 在构造函数里已经是 false（savevideofilethread.cpp:20），全新实例无需重置
    m_liveThread = new SaveVideoFileThread;

    // 预览：绑定编码线程的帧信号到预览窗口（和 RecorderDialog 构造函数写法一致）
    connect(m_liveThread, SIGNAL(SIG_sendPicInPic(QImage)),
            m_previewWidget, SLOT(slot_setImage(QImage)));
    connect(m_liveThread, SIGNAL(SIG_sendVideoFrame(QImage)),
            this, SLOT(slot_setImage(QImage)));

    // 构造推流参数（和 RecorderDialog::on_pb_start_clicked 写法一致，只是 fileName 换成 RTMP URL）
    STRU_AV_FORMAT format;
    format.fileName   = rtmpUrl;       // ← 关键：RTMP URL，FFmpeg 自动走推流
    format.frame_rate = FRAME_RATE;
    format.hasAudio   = true;
    format.hasCamera  = true;
    format.hasDesk    = false;         // 直播一般只推摄像头，不录屏
    format.videoBitRate = 800000;

    QScreen *src = QApplication::primaryScreen();
    QRect rect = src->geometry();
    format.width  = rect.width();
    format.height = rect.height();

    // 显示预览窗口（主播需要看到自己在播什么）
    this->showMinimized();
    m_previewWidget->show();

    // 启动编码推流（和 RecorderDialog 里调用顺序完全一致）
    m_liveThread->slot_setInfo(format);
    m_liveThread->slot_openVideo();
}

void LiveDialog::stopLivePush()
{
    if (!m_liveThread) return;

    m_previewWidget->hide();

    // ① 置 isStop=true，让 run() 里两个 while 循环 drain 完队列后退出
    m_liveThread->slot_closeVideo();

    // ② 关键：wait() 等待 run() 线程真正退出
    //    run() 退出后会执行 av_write_trailer / close_stream / avformat_free_context
    //    必须等这些全部完成，才能安全 delete
    //    （原 RecorderDialog 的 on_pb_stop_clicked 没有 wait，但因为录制不复用实例
    //     所以不会竞态；这里直播用 new/delete 生命周期管理，wait 是必须的）
    m_liveThread->wait();

    // ③ 线程已退出，FFmpeg 上下文已在 run() 末尾释放完毕，安全 delete
    delete m_liveThread;
    m_liveThread = nullptr;
}

void LiveDialog::slot_setImage(QImage img)
{
    // 如果 LiveDialog.ui 里有 lb_showImage 控件就用，没有就空实现
    // 最简版可以不要这个槽，只靠 m_previewWidget 显示预览
}
