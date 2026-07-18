#ifndef ONLINEDIALOG_H
#define ONLINEDIALOG_H

#include <QDialog>
#include"logindialog.h"
#include"TcpClientMediator.h"
#include"packdef.h"
#include"uploaddialog.h"
#include"QObject"
#include<QThread>
#include<QMap>
#include<QMutex>
#include<QWaitCondition>
#include<atomic>

namespace Ui {
class OnlineDialog;
}

class UploadWork:public QObject
{
    Q_OBJECT
public slots:
    void slot_UploadFile(QString filePath,QString imgPath,Hobby hy);
};

class OnlineDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OnlineDialog(QWidget *parent = nullptr);
    ~OnlineDialog();

    static OnlineDialog* m_online;

    TcpClientMediator* getTcpMediator() const { return m_tcp; }

signals:
    void SIG_updateProcess(qint64 cur,qint64 max);
    void SIG_PlayUrl(QString url);
    void SIG_PlayVideoWithId(QString url, int videoId); // 点播推荐视频专用，直播继续走 SIG_PlayUrl
    void SIG_LoginSuccess(int userId, QString userName);
    // 把收到的直播包转发给 LiveDialog（buf 所有权仍归 OnlineDialog，
    // 由 slot_ReadyData 末尾统一 delete[]，LiveDialog 只读不释放）
    void SIG_LivePacket(char* buf, int nlen);

    // 断点续传 v2：worker 线程通过此信号把提示信息转交 GUI 线程弹出（QMessageBox 必须在 GUI 线程）
    void SIG_UploadMessage(QString title, QString text);

public slots:
    void on_pb_login_clicked();

    void slot_loginCommit(QString name,QString password);
    void slot_registerCommit(QString name,QString password,Hobby hy);
    void slot_ReadyData(unsigned int lSendIP , char* buf , int nlen);
    void slot_loginRs(unsigned int lSendIP, char *buf, int nlen);
    void slot_registerRs(unsigned int lSendIP, char *buf, int nlen);
    void slot_uploadRs(unsigned int lSendIP, char *buf, int nlen);
    void slot_downloadRs(unsigned int lSendIP, char *buf, int nlen);
    void slot_fileblockrq(unsigned int lSendIP, char *buf, int nlen);
    void slot_uploadHistoryRs(unsigned int lSendIP, char *buf, int nlen);

    void slot_UploadFile(QString filePath,QString imgPath,Hobby hy);
    void UploadFile(QString filePath,Hobby hy,QString gifName="");
    void CancelUpload(); // 原子中断当前上传（中断后服务端保留 .part/.meta，下次同文件可续传）
    void on_pb_upload_clicked();
    void slot_PlayClicked();//点播项被点击
    void slot_MyPlayClicked(); // 上传历史被点击
private slots:
    void on_pb_fresh_clicked();

    void on_pb_video_clicked();

    void on_pb_uploadHistory_clicked();

    void slot_ShowUploadMessage(QString title, QString text); // v2：在 GUI 线程安全弹出上传结果提示

private:
    void reportVideoClick(int videoId); //上报"视频被点击播放"，服务端收到给这条视频热度+5

    Ui::OnlineDialog *ui;

    LoginDialog *m_login;
    TcpClientMediator *m_tcp;

    QString m_user;
    int m_id;

    UploadDialog* m_uploadDialog;
    QThread *m_uploadthread;
    UploadWork* m_uploadWorker;
    QMap<int,FileInfo*>m_mapVideoIDToFileInfo;

    // ===== 断点续传 v2：worker 线程（UploadWork）发握手/结束请求后，
    // 在 m_v2Mutex 保护下阻塞等待 GUI 线程 slot_ReadyData 唤醒 =====
    QMutex        m_v2Mutex;
    QWaitCondition m_v2HsCond;        // 握手回复条件
    bool          m_v2HsPending = false;
    STRU_UPLOAD_V2_RS m_v2HsRs;
    QWaitCondition m_v2EndCond;       // 终验回复条件
    bool          m_v2EndPending = false;
    STRU_UPLOAD_END_RS m_v2EndRs;
    std::atomic<bool> m_uploadCanceled{false}; // 原子中断标志（CancelUpload 置位）

};

#endif // ONLINEDIALOG_H
