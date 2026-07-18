#ifndef PLAYERDIALOG_H
#define PLAYERDIALOG_H

#include <QDialog>
#include "videoplayer.h"
#include <QTimer>
#include"onlinedialog.h"
#include "livedialog.h"

QT_BEGIN_NAMESPACE
namespace Ui { class PlayerDialog; }
QT_END_NAMESPACE

class PlayerDialog : public QDialog
{
    Q_OBJECT

public:
    PlayerDialog(QWidget *parent = nullptr);
    ~PlayerDialog();

private slots:

    void on_pb_start_clicked();

    void slot_setOneImage(QImage img);

    void on_pb_resume_clicked();

    void on_pb_pause_clicked();

    void on_pb_stop_clicked();

    void slot_PlayerStateChanged(int state);

    void slot_getTotalTime(qint64 uSec);

    void slot_TimerTimeOut();

    //事件过滤器
    bool eventFilter(QObject *obj,QEvent *event);
    void on_pb_online_clicked();
    void on_pb_live_clicked();
    void slot_PlayUrl(QString url);
    void slot_PlayVideoWithId(QString url, int videoId); //播放推荐/上传历史里的点播视频，带videoId用于上报
    void slot_cacheUserId(int userId, QString userName); //登录成功后缓存userId，上报播放记录要用
private:
    void reportPlaybackIfNeeded(); //把当前正在播的点播视频的观看时长/完播率上报给服务端

    Ui::PlayerDialog *ui;

    VideoPlayer* m_player;
    QTimer m_timer;
    OnlineDialog *m_onlineDialog;
    LiveDialog *m_liveDialog;

    //停止的状态
    bool isStop;

    int m_currentPlayingVideoId; //当前播的是哪个点播视频，-1表示本地文件/直播/未播放，不参与热度上报
    int m_userId; //当前登录用户ID，上报播放记录要带上

};
#endif // PLAYERDIALOG_H
