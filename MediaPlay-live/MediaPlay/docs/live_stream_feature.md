# MediaPlay 直播功能实现文档

> **修订说明（v2）**：原方案让 PlayerDialog new 第二个 RecorderDialog 实例，
> 与 UploadDialog 里已有的 RecorderDialog 实例各自持有独立的 SaveVideoFileThread，
> 导致实例内互斥标志 `m_isRecording/m_isLiveMode` 跨实例失效；且复用
> SaveVideoFileThread 时 `slot_setInfo()` 没有 `wait()` 旧线程就 `start()` 新线程，
> 存在 FFmpeg 上下文竞态。
>
> **本版核心改变**：直播推流用 LiveDialog 自己 new 的**全新的、独立的**
> `SaveVideoFileThread` 实例，开播 new、停播 `wait()+delete`，与 RecorderDialog
> 完全隔离。**RecorderDialog / SaveVideoFileThread 现有代码一行都不改**，
> 不需要任何互斥标志，录制和直播各管各的。

## 一、整体方案

### 当前架构（VOD 点播）

```
录制 → 编码 FLV → TCP 上传 → MediaServer(8000) → 存文件 + 写DB
                                                           ↓
观看 → TCP 请求列表 → MediaServer 返回 rtmp 路径 → VideoPlayer 播放 rtmp://IP/vod/path
```

### 新增直播架构

```
【推流端（主播）】
LiveDialog new 独立 SaveVideoFileThread → 编码 FLV → FFmpeg 直接推 → nginx-rtmp(1935)/videotest/[streamKey]
                                                  ↑
                      同时通过 OnlineDialog 的 TCP 连接通知 MediaServer(8000) 注册这路直播

【拉流端（观众）】
请求直播列表 → MediaServer(8000) → 返回活跃直播信息
                                        ↓
VideoPlayer 播放 rtmp://IP:1935/videotest/[streamKey]
```

### 核心思路

- **推流**：LiveDialog 自己 `new SaveVideoFileThread`，传入 RTMP URL 作为输出目标。`SaveVideoFileThread` 现有代码不改——`avformat_alloc_output_context2` 会自动识别 `rtmp://` 前缀走 RTMP 推流
- **隔离**：直播的 `SaveVideoFileThread` 实例和 RecorderDialog 里的那个**完全独立**，各自有自己的 `oc/video_st/audio_st`，不存在共享上下文问题，不需要互斥
- **生命周期**：开播 `new`，停播 `slot_closeVideo() → wait() → delete`，不复用，无竞态
- **服务管理**：MediaServer 新增包类型，管理"谁在直播"的状态（开始/停止）
- **拉流**：复用现有 `VideoPlayer`，它本来就能播 `rtmp://` URL，无需改动

---

## 二、nginx-rtmp 配置改动

### 当前 nginx.conf 的 rtmp 块（参考）

```nginx
rtmp {
    server {
        listen 1935;
        chunk_size 4096;

        application videotest {
            live on;
            record off;
        }

        application vod {
            play /home/colin/video/flv;
        }
    }
}
```

**说明**：本方案不依赖 nginx 的 HTTP 回调钩子，由客户端主动向 MediaServer 发包注册/注销直播，实现更简单，与现有架构一致。nginx.conf 不需要改动。

---

## 三、数据库改动

### 新增直播状态表 t_LiveStream

```sql
CREATE TABLE t_LiveStream (
    streamId    INT PRIMARY KEY AUTO_INCREMENT,  -- 直播ID
    userId      INT NOT NULL,                    -- 主播用户ID
    streamKey   VARCHAR(64) NOT NULL,            -- 推流唯一标识（如 "user_5"）
    title       VARCHAR(128) DEFAULT '',         -- 直播标题
    startTime   DATETIME NOT NULL,               -- 开始时间
    status      INT DEFAULT 1,                   -- 1=直播中 0=已结束
    UNIQUE KEY uk_streamKey (streamKey)
);
```

在 MySQL 中执行上面这条 SQL 建表。

---

## 四、MediaServer 服务端改动

### 4.1 packdef.h 新增协议包类型和结构体

在现有协议定义末尾追加：

```c
// ===== 直播相关新增包类型 =====
#define _DEF_PACK_LIVE_START_RQ   10011   // 开始直播请求
#define _DEF_PACK_LIVE_START_RS   10012   // 开始直播回复
#define _DEF_PACK_LIVE_STOP_RQ    10013   // 停止直播请求
#define _DEF_PACK_LIVE_STOP_RS    10014   // 停止直播回复
#define _DEF_PACK_LIVE_LIST_RQ    10015   // 获取直播列表请求
#define _DEF_PACK_LIVE_LIST_RS    10016   // 获取直播列表回复（一包一路直播）
#define _DEF_PACK_LIVE_LIST_END   10017   // 直播列表发送完毕标志

// ⚠️ 注意：以下结构体必须加构造函数 memset(this,0,sizeof(*this))，
// 否则客户端 `STRU_LIVE_START_RQ rq;` 在栈上分配时 m_szTitle 是垃圾数据，
// 当标题 UTF-8 字节数 ≥ 127 时 strncpy 不会补 '\0'，m_szTitle[127] 仍是栈垃圾，
// 服务端 sprintf 拼 SQL 会越界读到 '\0' 才停。项目现有结构体
// (STRU_LOGIN_RQ 等) 都有这个构造函数，新增结构体必须保持一致。

// 开始直播请求
typedef struct STRU_LIVE_START_RQ {
    PackType m_nType;          // = _DEF_PACK_LIVE_START_RQ
    int      m_nUserId;        // 用户ID
    char     m_szTitle[128];   // 直播标题
    STRU_LIVE_START_RQ() { memset(this, 0, sizeof(*this)); }
} STRU_LIVE_START_RQ;

// 开始直播回复
typedef struct STRU_LIVE_START_RS {
    PackType m_nType;          // = _DEF_PACK_LIVE_START_RS
    int      m_nResult;        // 0=失败 1=成功
    char     m_szStreamKey[64]; // 推流标识，客户端用这个拼 rtmp 地址
    STRU_LIVE_START_RS() { memset(this, 0, sizeof(*this)); }
} STRU_LIVE_START_RS;

// 停止直播请求
typedef struct STRU_LIVE_STOP_RQ {
    PackType m_nType;          // = _DEF_PACK_LIVE_STOP_RQ
    int      m_nUserId;        // 用户ID
    STRU_LIVE_STOP_RQ() { memset(this, 0, sizeof(*this)); }
} STRU_LIVE_STOP_RQ;

// 停止直播回复
typedef struct STRU_LIVE_STOP_RS {
    PackType m_nType;          // = _DEF_PACK_LIVE_STOP_RS
    int      m_nResult;        // 0=失败 1=成功
    STRU_LIVE_STOP_RS() { memset(this, 0, sizeof(*this)); }
} STRU_LIVE_STOP_RS;

// 获取直播列表请求
typedef struct STRU_LIVE_LIST_RQ {
    PackType m_nType;          // = _DEF_PACK_LIVE_LIST_RQ
    int      m_nUserId;        // 请求者用户ID
    STRU_LIVE_LIST_RQ() { memset(this, 0, sizeof(*this)); }
} STRU_LIVE_LIST_RQ;

// 直播列表回复（每路直播发一个包）
typedef struct STRU_LIVE_LIST_RS {
    PackType m_nType;          // = _DEF_PACK_LIVE_LIST_RS
    int      m_nStreamId;      // 直播ID
    int      m_nUserId;        // 主播ID
    char     m_szAnchorName[40]; // 主播用户名
    char     m_szTitle[128];   // 直播标题
    char     m_szStreamKey[64]; // 推流标识（客户端用这个拼 rtmp 播放地址）
    STRU_LIVE_LIST_RS() { memset(this, 0, sizeof(*this)); }
} STRU_LIVE_LIST_RS;

// 直播列表发送完毕
typedef struct STRU_LIVE_LIST_END {
    PackType m_nType;          // = _DEF_PACK_LIVE_LIST_END
    int      m_nCount;         // 本次共发了几路直播信息
    STRU_LIVE_LIST_END() { memset(this, 0, sizeof(*this)); }
} STRU_LIVE_LIST_END;
```

---

### 4.2 clogic.h 新增函数声明

在 `CLogic` 类的 public 函数声明区域追加：

```cpp
void LiveStartRq(sock_fd clientfd, char* szbuf, int nlen);
void LiveStopRq(sock_fd clientfd, char* szbuf, int nlen);
void LiveListRq(sock_fd clientfd, char* szbuf, int nlen);
```

---

### 4.3 clogic.cpp 新增三个处理函数

在文件末尾追加：

```cpp
// ===== 开始直播 =====
void CLogic::LiveStartRq(sock_fd clientfd, char* szbuf, int nlen)
{
    STRU_LIVE_START_RQ* rq = (STRU_LIVE_START_RQ*)szbuf;
    STRU_LIVE_START_RS  rs;
    rs.m_nType = _DEF_PACK_LIVE_START_RS;

    // 1. 查询用户名
    char sqlBuf[_DEF_SQLIEN] = "";
    sprintf(sqlBuf, "SELECT name FROM t_UserData WHERE id = %d;", rq->m_nUserId);
    list<string> resList;
    if (!m_sql->SelectMysql(sqlBuf, 1, resList) || resList.size() == 0) {
        rs.m_nResult = 0;
        m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }
    string userName = resList.front();

    // 2. 生成 streamKey（用 "user_用户ID" 作为唯一标识）
    char streamKey[64] = "";
    sprintf(streamKey, "user_%d", rq->m_nUserId);

    // 3. 若该用户已有直播记录，先清掉（重新开播）
    sprintf(sqlBuf, "DELETE FROM t_LiveStream WHERE userId = %d;", rq->m_nUserId);
    m_sql->UpdataMysql(sqlBuf);

    // 4. 插入新的直播记录
    sprintf(sqlBuf,
        "INSERT INTO t_LiveStream (userId, streamKey, title, startTime, status) "
        "VALUES (%d, '%s', '%s', NOW(), 1);",
        rq->m_nUserId, streamKey, rq->m_szTitle);

    if (!m_sql->UpdataMysql(sqlBuf)) {
        rs.m_nResult = 0;
        m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }

    // 5. 返回成功和 streamKey
    rs.m_nResult = 1;
    strcpy(rs.m_szStreamKey, streamKey);
    m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
}

// ===== 停止直播 =====
void CLogic::LiveStopRq(sock_fd clientfd, char* szbuf, int nlen)
{
    STRU_LIVE_STOP_RQ* rq = (STRU_LIVE_STOP_RQ*)szbuf;
    STRU_LIVE_STOP_RS  rs;
    rs.m_nType = _DEF_PACK_LIVE_STOP_RS;

    char sqlBuf[_DEF_SQLIEN] = "";
    sprintf(sqlBuf,
        "UPDATE t_LiveStream SET status = 0 WHERE userId = %d AND status = 1;",
        rq->m_nUserId);

    rs.m_nResult = m_sql->UpdataMysql(sqlBuf) ? 1 : 0;
    m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
}

// ===== 获取直播列表 =====
void CLogic::LiveListRq(sock_fd clientfd, char* szbuf, int nlen)
{
    // 查询所有正在直播的条目，关联用户名
    char sqlBuf[_DEF_SQLIEN] = "";
    sprintf(sqlBuf,
        "SELECT ls.streamId, ls.userId, u.name, ls.title, ls.streamKey "
        "FROM t_LiveStream ls "
        "JOIN t_UserData u ON ls.userId = u.id "
        "WHERE ls.status = 1 "
        "ORDER BY ls.startTime DESC;");

    list<string> resList;
    if (!m_sql->SelectMysql(sqlBuf, 5, resList)) {
        STRU_LIVE_LIST_END endPkt;
        endPkt.m_nType  = _DEF_PACK_LIVE_LIST_END;
        endPkt.m_nCount = 0;
        m_tcp->SendData(clientfd, (char*)&endPkt, sizeof(endPkt));
        return;
    }

    int count = 0;
    int nSize = resList.size() / 5;

    for (int i = 0; i < nSize; ++i) {
        STRU_LIVE_LIST_RS rs;
        rs.m_nType     = _DEF_PACK_LIVE_LIST_RS;
        rs.m_nStreamId = atoi(resList.front().c_str()); resList.pop_front();
        rs.m_nUserId   = atoi(resList.front().c_str()); resList.pop_front();
        strncpy(rs.m_szAnchorName, resList.front().c_str(), 39); resList.pop_front();
        rs.m_szAnchorName[39] = '\0';
        strncpy(rs.m_szTitle,      resList.front().c_str(), 127); resList.pop_front();
        rs.m_szTitle[127] = '\0';
        strncpy(rs.m_szStreamKey,  resList.front().c_str(), 63);  resList.pop_front();
        rs.m_szStreamKey[63] = '\0';

        m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
        ++count;
    }

    // 最后发一个结束包告诉客户端列表发完了
    STRU_LIVE_LIST_END endPkt;
    endPkt.m_nType  = _DEF_PACK_LIVE_LIST_END;
    endPkt.m_nCount = count;
    m_tcp->SendData(clientfd, (char*)&endPkt, sizeof(endPkt));
}
```

---

### 4.4 注册新包处理函数

在 `CLogic::setNetPackMap()` 函数末尾追加三行：

```cpp
NetPackMap(_DEF_PACK_LIVE_START_RQ) = &CLogic::LiveStartRq;
NetPackMap(_DEF_PACK_LIVE_STOP_RQ)  = &CLogic::LiveStopRq;
NetPackMap(_DEF_PACK_LIVE_LIST_RQ)  = &CLogic::LiveListRq;
```

---

## 五、MediaPlay 客户端改动

### 5.1 packdef.h 同步新增

客户端的 `packdef.h`（`D:/MediaPlay/netapi/net/packdef.h`）末尾追加与服务端完全相同的结构体定义（4.1 节的全部内容）。

---

### 5.2 SaveVideoFileThread：现有代码不改

> **v2 核心改变**：原方案要求修改 `SaveVideoFileThread::slot_setInfo()` 重置 `isStop`
> 以及处理线程复用竞态。本版**不修改 SaveVideoFileThread 任何代码**——
> 直播用 `new` 全新实例、停播 `wait()+delete`，根本不存在复用，
> `isStop` 在构造函数里已经是 `false`（savevideofilethread.cpp:20），
> 全新实例第一次调用必然正常工作。
>
> RecorderDialog 及其内部的 SaveVideoFileThread 完全保持原样，本地录制功能不受任何影响。

`SaveVideoFileThread` 现有的 `slot_setInfo()` 里 `avformat_alloc_output_context2(&oc, NULL, "flv", filename)` 会自动识别 `rtmp://` 前缀走 RTMP 推流路径，`avio_open` 也能正确打开 RTMP 写连接。**不需要改这个类的任何代码。**

---

### 5.3 新建 livedialog.h / livedialog.cpp / livedialog.ui

这是直播功能的独立对话框。**与 RecorderDialog 完全平行，不依赖、不复用 RecorderDialog 的任何东西。**

LiveDialog 自己 `new` 一个独立的 `SaveVideoFileThread` 实例专门用于推流，和 RecorderDialog 里那个毫无关系——各自独立的 `oc/video_st/audio_st`，不存在共享上下文问题，不需要互斥标志。

#### livedialog.h

```cpp
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

```

#### livedialog.cpp

```cpp
#include "livedialog.h"
#include "ui_livedialog.h"
#include <QMessageBox>
#include <QScreen>
#include <QApplication>
#include "common.h"   // FRAME_RATE

LiveDialog::LiveDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::LiveDialog),
      m_tcp(nullptr), m_userId(-1), m_isLiving(false),
      m_liveThread(nullptr)
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
    // ⚠️ m_tcp 非拥有，不在这里 delete（它由 OnlineDialog 持有并负责释放）
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

// 预览帧显示（和 RecorderDialog::slot_setImage 写法一致）
// 注意：LiveDialog.h 里需要补一个 slot_setImage 的声明（见下方说明）
```

> **注意**：上面 `slot_setImage` 的槽函数和 RecorderDialog 里的一模一样。
> 如果不想在 LiveDialog 里加预览，可以去掉 `SIG_sendVideoFrame` 的 connect
> 和 `slot_setImage`。最简版只保留 `SIG_sendPicInPic` → 预览窗口即可。

如果需要预览帧显示到 LiveDialog 自身，在 `livedialog.h` 的 `private slots:` 里加：

```cpp
void slot_setImage(QImage img);
```

`livedialog.cpp` 里实现（和 RecorderDialog 完全一样的写法）：

```cpp
void LiveDialog::slot_setImage(QImage img)
{
    // 如果 LiveDialog.ui 里有 lb_showImage 控件就用，没有就空实现
    // 最简版可以不要这个槽，只靠 m_previewWidget 显示预览
}
```

#### livedialog.ui（控件清单，用 Qt Designer 创建）

| 控件类型 | objectName | 文字 | 说明 |
|---|---|---|---|
| QLineEdit | le_title | （占位文字"直播标题"）| 输入直播间名称 |
| QPushButton | pb_startLive | 开始直播 | |
| QPushButton | pb_stopLive | 停止直播 | 初始禁用 |
| QListWidget | lw_liveList | — | 显示直播列表 |
| QPushButton | pb_refresh | 刷新列表 | |
| QPushButton | pb_watch | 观看直播 | |

---

### 5.4 RecorderDialog：不改

> **v2 核心改变**：原方案要给 RecorderDialog 加 `m_isRecording/m_isLiveMode`
> 互斥标志、加 `slot_StartLivePush/slot_StopLivePush`、改 `on_pb_start_clicked/
> on_pb_stop_clicked`。本版**全部删除**——直播推流由 LiveDialog 自己的独立
> `SaveVideoFileThread` 实例完成，和 RecorderDialog 毫无关系。
>
> **RecorderDialog / savevideofilethread.cpp / savevideofilethread.h 一行都不改。**
> 本地录制功能完全保持原样。

---

### 5.5 PlayerDialog：连接 LiveDialog 与 OnlineDialog

⚠️ 这里有两处现有代码需要补的东西：

**① 用户登录成功后的 `userId`/`userName` 现在出不来**。它们是
`OnlineDialog` 的私有成员（`onlinedialog.cpp` 登录成功时赋值给
`m_id`/`m_user`），目前没有 getter，也没有信号对外广播。需要给
`OnlineDialog` 新增一个信号，登录成功时 emit 出去：

```cpp
// onlinedialog.h 新增
signals:
    void SIG_LoginSuccess(int userId, QString userName);
    // 把收到的直播包转发给 LiveDialog（buf 所有权仍归 OnlineDialog，
    // 由 slot_ReadyData 末尾统一 delete[]，LiveDialog 只读不释放）
    void SIG_LivePacket(char* buf, int nlen);
public:
    // 暴露内部 TCP 中介器，供 LiveDialog 复用（非拥有，调用方不要 delete）
    TcpClientMediator* getTcpMediator() const { return m_tcp; }
```

```cpp
// onlinedialog.cpp: slot_loginRs() 里，判断 result == login_success 之后追加
emit SIG_LoginSuccess(m_id, m_user);
```

```cpp
// onlinedialog.cpp: slot_ReadyData() 的 switch 里新增直播包 case
void OnlineDialog::slot_ReadyData(unsigned int lSendIP, char *buf, int nlen)
{
    int nType = *(int*)buf;
    switch(nType)
    {
        // ... 原有 case 不变 ...

        // ===== 直播包：转发给 LiveDialog，不在这里处理 =====
        case _DEF_PACK_LIVE_START_RS:
        case _DEF_PACK_LIVE_STOP_RS:
        case _DEF_PACK_LIVE_LIST_RS:
        case _DEF_PACK_LIVE_LIST_END:
            emit SIG_LivePacket(buf, nlen);   // LiveDialog 只读 buf，不 delete
            break;
    }

    delete[] buf;   // 始终由 OnlineDialog 统一回收，避免 double free
}
```

> ⚠️ 关键点：`buf` 的 `delete[]` 始终由 `OnlineDialog::slot_ReadyData` 末尾执行。
> `LiveDialog::slot_handleLivePacket` **只读不释放**。Qt 同线程信号槽是
> DirectConnection 同步调用，`emit SIG_LivePacket` 会先把 `LiveDialog` 的槽
> 执行完才返回，所以 `LiveDialog` 用完 `buf` 后 `OnlineDialog` 才 `delete[]`，
> 不会出现 use-after-free。

**② `LiveDialog` 不能自己 `new` 第二条 TCP 连接**。必须复用 `OnlineDialog`
已登录的 TCP 连接，否则服务端不知道 LiveDialog 这条连接属于谁。

**③ PlayerDialog 不再 new RecorderDialog**（v2 改变）。原方案要在这里 new
RecorderDialog，现在不需要了——直播推流由 LiveDialog 自己管。PlayerDialog
只需要 new 一个 LiveDialog：

```cpp
// playerdialog.h 新增
#include "livedialog.h"
...
private slots:
	void on_pb_live_clicked();

...
private:
    LiveDialog *m_liveDialog;
```

```cpp
// playerdialog.cpp 构造函数中追加

m_liveDialog = new LiveDialog(this);
m_liveDialog->hide();

// ⚠️ 让 LiveDialog 复用 OnlineDialog 已登录的 TCP 连接
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
```

新增打开直播窗口的按钮（在 `playerdialog.ui` 里添加一个按钮 `pb_live`，文字"直播"）：

```cpp
// playerdialog.cpp
void PlayerDialog::on_pb_live_clicked()
{
    m_liveDialog->show();
}
```

#### ⚠️ PlayerDialog 析构函数要清理 LiveDialog

现有 `PlayerDialog::~PlayerDialog()`（playerdialog.cpp:36-44）只清理了
`ui`、`m_player`、`m_onlineDialog`。新增的 `m_liveDialog` 需要清理：

```cpp
// playerdialog.cpp: ~PlayerDialog() 追加
PlayerDialog::~PlayerDialog()
{
    delete ui;
    delete m_player;
    if (m_onlineDialog) {
        delete m_onlineDialog;
        m_onlineDialog = NULL;
    }
    // ⚠️ 新增：清理直播对话框
    // LiveDialog 析构里会先 stopLivePush()（wait+delete 内部线程），再 delete ui
    if (m_liveDialog) {
        delete m_liveDialog;
        m_liveDialog = NULL;
    }
}
```

> 注意：`m_liveDialog` 析构不 `delete m_tcp`（它是非拥有的，由 `m_onlineDialog` 持有）。
> `m_liveDialog` 析构会先调用 `stopLivePush()` 确保 `m_liveThread` 已 `wait()+delete`，
> 所以即使关窗口时直播还在进行，也不会泄漏线程或 FFmpeg 上下文。

---

### 5.6 在 MediaPlay.pro 中注册新文件

```
HEADERS += livedialog.h
SOURCES += livedialog.cpp
FORMS   += livedialog.ui
```

---

## 六、完整数据流程

### 主播开始直播

```
PlayerDialog 点击"直播"按钮
    → LiveDialog.show()
    → 输入标题，点击"开始直播"
    → 发送 STRU_LIVE_START_RQ (packType=10011) 到 MediaServer:8000
    → MediaServer.CLogic::LiveStartRq()
        → 写入 t_LiveStream (streamKey="user_5", status=1)
        → 返回 STRU_LIVE_START_RS (streamKey="user_5")
    → OnlineDialog.slot_ReadyData() 识别为直播包
        → emit SIG_LivePacket(buf, nlen)
    → LiveDialog.slot_handleLivePacket()
        → 收到 LIVE_START_RS result=1
        → 调用 startLivePush("rtmp://IP:1935/videotest/user_5")
    → LiveDialog::startLivePush()
        → new SaveVideoFileThread（全新独立实例，isStop 构造时已为 false）
        → connect 预览信号
        → 构造 STRU_AV_FORMAT，fileName = "rtmp://IP:1935/videotest/user_5"
        → m_liveThread->slot_setInfo(format)
        → m_liveThread->slot_openVideo()
    → SaveVideoFileThread.slot_setInfo() / slot_openVideo()（现有代码，不改）
        → avformat_alloc_output_context2(&oc, NULL, "flv", "rtmp://IP:1935/videotest/user_5")
        → avio_open(&oc->pb, "rtmp://IP:1935/videotest/user_5", AVIO_FLAG_WRITE)
        → this->start() → run() 持续编码推流
```

### 观众观看直播

```
LiveDialog 点击"刷新列表"
    → 发送 STRU_LIVE_LIST_RQ (packType=10015) 到 MediaServer:8000
    → MediaServer.CLogic::LiveListRq()
        → SELECT ... FROM t_LiveStream WHERE status=1
        → 逐条发送 STRU_LIVE_LIST_RS
        → 发送 STRU_LIVE_LIST_END
    → LiveDialog 接收，填充 lw_liveList

观众选中一条直播，点击"观看直播"
    → 拼接 rtmpUrl = "rtmp://IP:1935/videotest/user_5"
    → emit SIG_WatchLive(rtmpUrl)
    → PlayerDialog.slot_PlayUrl(rtmpUrl)
    → VideoPlayer.setFileName(rtmpUrl)
    → avformat_open_input(&pFormatCtx, "rtmp://IP:1935/videotest/user_5", ...)
    → 正常解码播放（与 VOD 完全一致）
```

### 主播停止直播

```
LiveDialog 点击"停止直播"
    → LiveDialog::on_pb_stopLive_clicked()
        → 先调 stopLivePush()：
            ① m_liveThread->slot_closeVideo()   // 置 isStop=true
            ② m_liveThread->wait()               // 等 run() 退出（写 trailer、释放上下文）
            ③ delete m_liveThread                // 线程已退出，安全回收
            ④ m_liveThread = nullptr
        → 再发 STRU_LIVE_STOP_RQ (packType=10013) 到 MediaServer:8000
    → MediaServer.CLogic::LiveStopRq()
        → UPDATE t_LiveStream SET status=0 WHERE userId=5
        → 返回 STRU_LIVE_STOP_RS
    → LiveDialog 接收，回退按钮状态
    → FFmpeg 关闭 RTMP 推流连接
    → nginx-rtmp 检测到推流断开，该路流不再可拉取
```

> ⚠️ 顺序很重要：**先停推流再通知服务端**。如果先通知服务端把 status 置 0，
> 观众刷新列表看不到这路直播了，但主播还在推流，nginx-rtmp 那边流还活着，
> 状态不一致。先停推流，nginx-rtmp 那边流断了，再通知服务端改状态，一致。

---

## 七、实现顺序建议

按照以下顺序实现，每步可独立验证：

1. **建数据库表**：执行第三节的 SQL，验证表创建成功
2. **改 MediaServer**：加 packdef.h 结构体 → 加 clogic.h 声明 → 加 clogic.cpp 三个函数 → 注册包处理，编译测试
3. **用 ffmpeg 命令行验证推流**：在终端执行
   `ffmpeg -re -i test.flv -c copy -f flv rtmp://IP:1935/videotest/user_5`
   另开一个 ffplay 验证能拉到：`ffplay rtmp://IP:1935/videotest/user_5`
4. **改 OnlineDialog**：加 `SIG_LoginSuccess` / `SIG_LivePacket` 信号 + `getTcpMediator()` + slot_ReadyData 里加直播包 case
5. **写 LiveDialog**：先只写信令部分（开始/停止/列表），不接推流，验证包收发正常
6. **接推流**：实现 `startLivePush` / `stopLivePush`，让摄像头画面推到 nginx-rtmp
7. **完整联调**：一端开播，另一端刷新列表 → 观看

---

## 八、注意事项

### 为什么直播用独立的 SaveVideoFileThread 实例，而不是复用 RecorderDialog 的

原方案（v1）在 PlayerDialog 里 new 第二个 RecorderDialog，让直播和录制
共享 RecorderDialog 的 SaveVideoFileThread 实例，靠 `m_isRecording/m_isLiveMode`
互斥标志做二选一。但项目里 **UploadDialog 已经持有一个 RecorderDialog 实例**
（`uploaddialog.cpp:20`），两个 RecorderDialog 实例各自有独立的
SaveVideoFileThread，互斥标志是实例内成员变量，**跨实例完全不可见**——
通过 UploadDialog 那个实例开始本地录制，根本不会阻止通过 PlayerDialog
那个新实例开直播，互斥形同虚设。

更深层的问题：`SaveVideoFileThread::slot_setInfo()` 结尾直接 `this->start()`
启动新线程，但 `slot_closeVideo()` 只置 `isStop=true`，**从不 `wait()`** 旧线程。
如果旧线程的 `run()` 还在写最后几帧 / 写 `av_write_trailer` / `avformat_free_context`，
新线程已经进来覆盖 `oc/video_st/audio_st`，新旧线程同时操作同一批 FFmpeg 上下文 →
崩溃、花屏或文件损坏。

**本版方案**：LiveDialog 自己 `new` 一个全新的 `SaveVideoFileThread` 实例，
- 全新实例，`isStop` 构造时已为 `false`，不存在"第二次 setInfo 时 isStop 还是 true"的问题
- 开播 new、停播 `wait()+delete`，不复用，不存在竞态
- 和 RecorderDialog 的实例完全独立，各自 `oc/video_st/audio_st`，不需要互斥
- RecorderDialog / SaveVideoFileThread 现有代码一行不改，本地录制功能不受影响

### FFmpeg RTMP 推流：已实测验证

`rtmp://` 协议是 FFmpeg 内置的原生实现（`libavformat/rtmpproto.c`），编译时不需要外部 `librtmp` 库；只有 `rtmpe`/`rtmps`/`rtmpt` 这些加密/隧道变体才依赖外部 `librtmp`。项目现有的点播播放（拉流读）已经证明 `ffmpeg-4.2.2` 这份预编译 dll 支持 `rtmp://` 协议，但拉流（读）和推流（写）是同一协议模块下的两条不同代码路径，此前未经验证。

**已实测**：使用项目自带的 `ffmpeg-4.2.2/bin/ffmpeg.exe`（与 App 实际链接的 dll 版本一致）执行

```bash
ffmpeg-4.2.2/bin/ffmpeg.exe -re -i 本地文件.mp4 -c copy -f flv rtmp://192.168.43.239:1935/videotest/test1
```

推流成功，另一端 `ffplay` 成功拉取播放。说明 nginx-rtmp 的 `videotest` application 推流/拉流均正常，`SaveVideoFileThread` 走 `avformat_alloc_output_context2` + `avio_open` 推 RTMP 这条路径在当前环境下是可行的，无需替换 FFmpeg 版本或额外配置。

### SaveVideoFileThread 的采集源

直播时需要持续提供视频帧。`SaveVideoFileThread` 内部 `new` 了 `PicInPic_Read`
和 `Audio_Read`，分别负责摄像头/桌面采集和麦克风采集。直播的独立实例会
`new` 自己的 `PicInPic_Read` / `Audio_Read`，和录制那个实例各自独立。
两个实例不会同时运行（正常人不会边录边播），即使同时也会各自打开采集设备。

### 客户端断连处理

如果主播客户端崩溃，`LiveDialog` 析构会调 `stopLivePush()` 确保
`m_liveThread` 被 `wait()+delete`，FFmpeg 上下文不会泄漏。nginx-rtmp 会检测到
推流连接断开，观众端播放器会收到错误。但 MediaServer 的 `t_LiveStream` 里
这条记录 status 还是 1。

解决方案（简单版）：在 `PlayerDialog` 的析构或关闭事件中，发送
`STRU_LIVE_STOP_RQ` 做清理。或者给 `t_LiveStream` 增加一个 `lastHeartbeat`
字段，定期由客户端更新，MediaServer 定期清理超时的记录（进阶实现）。

### 现有代码需同步修复的问题

直播功能依赖以下两处**现有代码**的既有缺陷，建议在接入直播前一并修掉，否则会影响直播的稳定性。

#### 1. `TcpClientMediator::SendData` 在 `bool` 函数里 `return -1`（客户端）

`netapi/mediator/TcpClientMediator.cpp:49`：

```cpp
bool TcpClientMediator::SendData(unsigned int lSendIP, char* buf, int nlen)
{
    ...
    if (IsConnected()) {
        return m_pNet->SendData(0, buf, nlen);
    } else {
        m_pNet->UnInitNet();
        delete m_pNet;
        m_pNet = new TcpClient(this);
        if (this->OpenNet(m_szBufIP, m_port)) {
            return m_pNet->SendData(0, buf, nlen);
        } else {
            return -1;   // ⚠️ bool 函数返回 -1，隐式转换为 true
        }
    }
}
```

重连失败时本应返回 `false`，实际返回 `true`。直播场景下，网络断开重连失败时，
`LiveDialog::on_pb_startLive_clicked()` 调 `m_tcp->SendData(...)` 后看到返回 `true`，
以为开播请求已发送，实际没有——主播以为开播了，服务端却没收到 `STRU_LIVE_START_RQ`，
`t_LiveStream` 表里没有这条记录，观众刷新列表看不到这路直播。

**修复**：把 `return -1;` 改成 `return false;`。

```cpp
        if (this->OpenNet(m_szBufIP, m_port)) {
            return m_pNet->SendData(0, buf, nlen);
        } else {
            return false;   // 重连失败，明确返回 false
        }
```

#### 2. 服务端 `Block_Epoll_Net::recv_task` 对不完整包无保护（服务端）

`MediaServer/src/block_epoll_net.cpp:172-180`，内层 `while(nPackSize)` 循环中
如果 `recv` 返回 ≤ 0（连接中断），直接 `break`，但之后**仍然用不完整的 `nOffSet`
创建 `DataBuffer` 并处理**：

```cpp
pSzBuf = new char[nPackSize];
int nOffSet = 0;
nRelReadNum = 0;
while (nPackSize) {
    nRelReadNum = recv(ev->fd, pSzBuf + nOffSet, nPackSize, 0);
    if (nRelReadNum <= 0)
        break;                          // 连接断了，但 nOffSet < nPackSize
    nOffSet += nRelReadNum;
    nPackSize -= nRelReadNum;
}
// ⚠️ 这里没有判断 nOffSet 是否等于预期的包大小，直接处理
DataBuffer * buffer = new DataBuffer(ev->pNet, ev->fd, pSzBuf, nOffSet);
Buffer_Deal((void*) buffer);
```

`Buffer_Deal` → `m_recv_callback` → `CLogic::DealData` 会把这个不完整的缓冲区
强转为结构体指针（`STRU_LIVE_START_RQ*` 等），读取越界。直播功能新增了 6 种
网络包，触发面比原来更大。

**修复**：内层循环 `break` 后判断 `nOffSet` 是否达到包大小，不足则视为连接异常，
走错误处理分支（关闭连接、回收资源），不要处理不完整的包。

```cpp
pSzBuf = new char[nPackSize];
int nOffSet = 0;
nRelReadNum = 0;
bool bComplete = true;                  // 新增：标记包是否完整
while (nPackSize) {
    nRelReadNum = recv(ev->fd, pSzBuf + nOffSet, nPackSize, 0);
    if (nRelReadNum <= 0) {
        bComplete = false;              // 连接中断或出错，包不完整
        break;
    }
    nOffSet += nRelReadNum;
    nPackSize -= nRelReadNum;
}

if (!bComplete) {
    delete[] pSzBuf;
    break;  // 走 do..while(0) 末尾统一错误处理
}

DataBuffer * buffer = new DataBuffer(ev->pNet, ev->fd, pSzBuf, nOffSet);
Buffer_Deal((void*) buffer);
```
