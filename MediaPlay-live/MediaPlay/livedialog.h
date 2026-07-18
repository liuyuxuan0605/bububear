#ifndef LIVEDIALOG_H
#define LIVEDIALOG_H

#include "netapi/net/packdef.h"        // 必须放最前面！含 winsock2.h，要在 windows.h 之前
#include <QDialog>
#include <QListWidget>
#include <QMap>
#include "savevideofilethread.h"      // 直播推流用，独立于 RecorderDialog 的实例
#include "picturewidget.h"            // 预览窗口（和 RecorderDialog 用法一样）
#include "netapi/mediator/TcpClientMediator.h"

namespace Ui {
class LiveDialog;
}

// 用于在列表里存每一路直播的信息
struct LiveInfo {
    int    streamId;
    int    userId;
    QString anchorName;
    QString title;
    QString streamKey;
};

class LiveDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LiveDialog(QWidget *parent = nullptr);
    ~LiveDialog();

signals:
    // 通知 PlayerDialog 用 VideoPlayer 播放这个 RTMP URL（观众拉流）
    void SIG_WatchLive(QString rtmpUrl);

public slots:
    // 由 PlayerDialog 在登录后设置用户 ID 和用户名
    void setUserInfo(int userId, QString userName);
    void setTcpMediator(TcpClientMediator* tcp);
    // 接收 OnlineDialog 转发出来的直播包
    void slot_handleLivePacket(char* buf, int nlen);

private slots:
    void on_pb_startLive_clicked();
    void on_pb_stopLive_clicked();
    void on_pb_refresh_clicked();
    void on_pb_watch_clicked();
    void slot_setImage(QImage img);

private:
    void refreshLiveList();
    void appendLiveItem(LiveInfo* info);
    void startLivePush(QString rtmpUrl);   // new 独立 SaveVideoFileThread 并推流
    void stopLivePush();

    Ui::LiveDialog *ui;
    TcpClientMediator*     m_tcp;          // 非拥有指针，由 OnlineDialog 持有
    int                    m_userId;
    QString                m_userName;
    bool                   m_isLiving;
    QString                m_currentStreamKey;

    SaveVideoFileThread*   m_liveThread;
    PictureWidget*         m_previewWidget;  // 主播预览（和 RecorderDialog 用法一样）

    QMap<int, LiveInfo*>   m_liveMap;
};

#endif // LIVEDIALOG_H
