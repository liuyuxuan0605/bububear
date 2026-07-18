#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include "err_str.h"
#include <malloc.h>

#include<iostream>
#include<map>
#include<list>


//边界值
#define _DEF_SIZE           45
#define _DEF_BUFFERSIZE     1000
#define _DEF_PORT           8001
#define _DEF_SERVERIP       "0.0.0.0"
#define _DEF_LISTEN         128
#define _DEF_EPOLLSIZE      4096
#define _DEF_IPSIZE         16
#define _DEF_COUNT          10
#define _DEF_TIMEOUT        10
#define _DEF_SQLIEN         400
#define TRUE                true
#define FALSE               false



/*-------------数据库信息-----------------*/
#define _DEF_DB_NAME    "MediaServer"
#define _DEF_DB_IP      "localhost"
#define _DEF_DB_USER    "root"
#define _DEF_DB_PWD     "123456"
/*--------------------------------------*/
#define _MAX_PATH           (260)
#define _DEF_BUFFER         (4096)
#define _DEF_CONTENT_SIZE	(64*1024)
#define _MAX_SIZE           (40)
#define DEF_HOBBY_COUNT     (8)

//自定义协议   先写协议头 再写协议结构
//登录 注册 获取好友信息 添加好友 聊天 发文件 下线请求
#define _DEF_PACK_BASE	(10000)
#define _DEF_PACK_COUNT (100)

//注册
#define _DEF_PACK_REGISTER_RQ	(_DEF_PACK_BASE + 0 )
#define _DEF_PACK_REGISTER_RS	(_DEF_PACK_BASE + 1 )
//登录
#define _DEF_PACK_LOGIN_RQ	(_DEF_PACK_BASE + 2 )
#define _DEF_PACK_LOGIN_RS	(_DEF_PACK_BASE + 3 )
//上传
#define _DEF_PACK_UPLOAD_RQ (_DEF_PACK_BASE + 4)
#define _DEF_PACK_UPLOAD_RS (_DEF_PACK_BASE + 5)
#define _DEF_PACK_FILEBLOCK_RQ (_DEF_PACK_BASE + 6)
//下载
#define DEF_PACK_DOWNLOAD_RQ (_DEF_PACK_BASE + 7)
#define DEF_PACK_DOWNLOAD_RS (_DEF_PACK_BASE + 8)
// 上传历史
#define _DEF_PACK_UPLOADHISTORY_RQ  (_DEF_PACK_BASE + 9)
#define _DEF_PACK_UPLOADHISTORY_RS  (_DEF_PACK_BASE + 10)

// ===== 直播相关新增包类型 =====
#define _DEF_PACK_LIVE_START_RQ   (_DEF_PACK_BASE + 11)   // 开始直播请求
#define _DEF_PACK_LIVE_START_RS   (_DEF_PACK_BASE + 12)   // 开始直播回复
#define _DEF_PACK_LIVE_STOP_RQ    (_DEF_PACK_BASE + 13)   // 停止直播请求
#define _DEF_PACK_LIVE_STOP_RS    (_DEF_PACK_BASE + 14)   // 停止直播回复
#define _DEF_PACK_LIVE_LIST_RQ    (_DEF_PACK_BASE + 15)   // 获取直播列表请求
#define _DEF_PACK_LIVE_LIST_RS    (_DEF_PACK_BASE + 16)   // 获取直播列表回复（一包一路直播）
#define _DEF_PACK_LIVE_LIST_END   (_DEF_PACK_BASE + 17)   // 直播列表发送完毕标志

// ===== 热度统计相关新增包类型 =====
#define _DEF_PACK_PLAY_REPORT_RQ  (_DEF_PACK_BASE + 18)   // 播放时长/完播率上报（纯上报，无回复）
#define _DEF_PACK_VIDEO_CLICK_RQ  (_DEF_PACK_BASE + 19)   // 视频被点击播放上报（纯上报，无回复），热度+5

//返回的结果
//注册请求的结果
#define user_is_exist		(0)
#define register_success	(1)
//登录请求的结果
#define user_not_exist		(0)
#define password_error		(1)
#define login_success		(2)


typedef int PackType;

//协议结构
//注册
typedef struct STRU_REGISTER_RQ
{
    STRU_REGISTER_RQ():type(_DEF_PACK_REGISTER_RQ)
    {
        memset( user  , 0, sizeof(user));
        memset( password , 0, sizeof(password) );
        food   =0 ;
        funny  =0 ;
        ennegy =0 ;
        dance  =0 ;
        music  =0 ;
        video  =0 ;
        outside=0 ;
        edu    =0 ;
    }
    //需要用户名, 密码,
    PackType type;
    char user[_MAX_SIZE];
    char password[_MAX_SIZE];
    int food    ;
    int funny   ;
    int ennegy  ;
    int dance   ;
    int music   ;
    int video   ;
    int outside ;
    int edu     ;

}STRU_REGISTER_RQ;

typedef struct STRU_REGISTER_RS
{
    //回复结果
    STRU_REGISTER_RS(): type(_DEF_PACK_REGISTER_RS) , result(register_success)
    {
    }
    PackType type;
    int result;

}STRU_REGISTER_RS;

//登录
typedef struct STRU_LOGIN_RQ
{
    //登录需要：用户名  密码
    STRU_LOGIN_RQ():type(_DEF_PACK_LOGIN_RQ)
    {
        memset( user , 0, sizeof(user) );
        memset( password , 0, sizeof(password) );
    }
    PackType type;
    char user[_MAX_SIZE];
    char password[_MAX_SIZE];

}STRU_LOGIN_RQ;

typedef struct STRU_LOGIN_RS
{
    // 需要 结果 , 用户的id
    STRU_LOGIN_RS(): type(_DEF_PACK_LOGIN_RS) , result(login_success),userid(0)
    {
    }
    PackType type;
    int result;
    int userid;

}STRU_LOGIN_RS;

//上传文件请求
typedef struct STRU_UPLOAD_RQ
{
    STRU_UPLOAD_RQ()
    {
        m_nType = _DEF_PACK_UPLOAD_RQ;
        m_nFileId = 0;
        m_UserId = 0;
        m_nFileSize=0;

        memset(m_szFileName , 0 ,_MAX_PATH);
        memset(m_szGifName, 0 ,_MAX_PATH);
        memset(m_szFileType, 0 ,_MAX_SIZE);
        memset(m_szHobby,0,DEF_HOBBY_COUNT);
    }
    PackType m_nType; //包类型
    int m_UserId; //用于查数据库，获取用户名字，拼接路径
    int m_nFileId; //区分不同文件，可采用 md5 或 随机数 用户同时只能传一个所以相同概率较低
    int64_t m_nFileSize; //文件大小，用于文件传输结束
    char m_szHobby[DEF_HOBBY_COUNT]; //喜好标签
    char m_szFileName[_MAX_PATH]; //文件名，用于存储文件
    char m_szGifName[_MAX_PATH]; //gif文件名，方便数据库写表
    char m_szFileType[_MAX_SIZE]; //用于区分视频和图片
}STRU_UPLOAD_RQ;

//上传文件请求回复
typedef struct STRU_UPLOAD_RS
{
    STRU_UPLOAD_RS()
    {
        m_nType = _DEF_PACK_UPLOAD_RS;
        m_nResult = 0;
    }
    PackType m_nType; //包类型
    int m_nResult;
}STRU_UPLOAD_RS;

//文件块请求
typedef struct STRU_FILEBLOCK_RQ
{
    STRU_FILEBLOCK_RQ()
    {
        m_nType = _DEF_PACK_FILEBLOCK_RQ;
        m_nUserId = 0;
        m_nFileId =0;
        m_nBlockLen =0;
        memset(m_szFileContent,0,_DEF_CONTENT_SIZE);
    }
    PackType m_nType; //包类型
    int m_nUserId; //用户 ID
    int m_nFileId; //文件 id 用于区分文件
    int m_nBlockLen; //文件写入大小
    char m_szFileContent[_DEF_CONTENT_SIZE];
}STRU_FILEBLOCK_RQ;

//文件信息
typedef struct STRU_FILEINFO
{
public:
    STRU_FILEINFO():m_nFileID(0),m_VideoID(0),m_nFileSize(0),m_nPos(0),
        m_nUserId(0),pFile(0),m_nTaskId(0),m_nReceived(0),m_nLastActive(0)
    {
        memset(m_szFilePath, 0 , _MAX_PATH);
        memset(m_szFileName, 0 , _MAX_PATH);
        memset(m_szGifPath , 0 , _MAX_PATH );

        memset(m_szGifName, 0 , _MAX_PATH);

        memset(m_szFileType, 0 , _MAX_SIZE);

        memset(m_Hobby, 0 , DEF_HOBBY_COUNT);

        memset(m_UserName, 0 , _MAX_SIZE);

        memset(m_szRtmp ,0 , _MAX_PATH);

        memset(m_szMd5, 0, 33);
        memset(m_szPartPath, 0, _MAX_PATH);
        memset(m_szMetaPath, 0, _MAX_PATH);
    }
    int m_nFileID;//下載的時候是用來做 UI 控件編號的， 上傳的時候是一個隨機數， 區分文件。
    int m_VideoID;//真是文件 ID 與 Mysql 的一致
    int64_t m_nFileSize;
    int64_t m_nPos;
    int m_nUserId;
    FILE* pFile;
    char m_szFilePath[_MAX_PATH];
    char m_szFileName[_MAX_PATH];
    char m_szGifPath[_MAX_PATH];
    char m_szGifName[_MAX_PATH];
    char m_szFileType[_MAX_SIZE];
    char m_Hobby[DEF_HOBBY_COUNT];
    char m_UserName[_MAX_SIZE];
    char m_szRtmp[_MAX_PATH];
    // ===== v2 断点续传扩展字段 =====
    int m_nTaskId;             // v2：服务端分配的任务ID
    int64_t m_nReceived;       // v2：已收字节（续传点权威来源，fflush 后写入 .meta）
    char m_szMd5[33];          // v2：文件内容指纹
    char m_szPartPath[_MAX_PATH]; // v2：.part 临时文件路径
    char m_szMetaPath[_MAX_PATH]; // v2：.meta 元数据路径
    time_t m_nLastActive;      // v2：最后活跃时间（占用超时释放）
}FileInfo;

//下载文件请求
typedef struct STRU_DOWNLOAD_RQ
{
    STRU_DOWNLOAD_RQ()
    {
        m_nType = DEF_PACK_DOWNLOAD_RQ;
        m_nUserId = 0;
    }
    PackType m_nType; //包类型
    int m_nUserId; //用户 ID
}STRU_DOWNLOAD_RQ;
//下载文件回复
typedef struct STRU_DOWNLOAD_RS
{
    STRU_DOWNLOAD_RS()
    {
        m_nType = DEF_PACK_DOWNLOAD_RS;
        m_nFileId = 0;
        memset(m_szFileName , 0 ,_MAX_PATH);
        memset(m_rtmp , 0 ,_MAX_PATH);
    }
    PackType m_nType; //包类型
    int m_nFileId;
    int64_t m_nFileSize;
    int m_nVideoId;
    char m_szFileName[_MAX_PATH];
    char m_rtmp[_MAX_PATH]; // 播放地址 如//1/103.MP3 用户本地需要转化为 rtmp://服务器 ip/app 名/ + 这个字符串 //本项目
}STRU_DOWNLOAD_RS;

// 上传历史请求
typedef struct STRU_UPLOADHISTORY_RQ
{
    STRU_UPLOADHISTORY_RQ()
    {
        m_nType = _DEF_PACK_UPLOADHISTORY_RQ;
        m_nUserId = 0;
    }
    PackType m_nType;
    int m_nUserId;
}STRU_UPLOADHISTORY_RQ;

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

// 播放时长/完播率上报（纯上报，服务端不回复）
typedef struct STRU_PLAY_REPORT_RQ {
    PackType m_nType;          // = _DEF_PACK_PLAY_REPORT_RQ
    int      m_nUserId;        // 用户ID
    int      m_nVideoId;       // 视频ID
    int      m_nWatchSeconds;  // 本次实际观看了多少秒
    int      m_nTotalSeconds;  // 视频总时长（秒）
    STRU_PLAY_REPORT_RQ() { memset(this, 0, sizeof(*this)); }
} STRU_PLAY_REPORT_RQ;

// 视频被点击播放（用户双击卡片开始播放的瞬间发送，纯上报，服务端不回复）
typedef struct STRU_VIDEO_CLICK_RQ {
    PackType m_nType;          // = _DEF_PACK_VIDEO_CLICK_RQ
    int      m_nUserId;        // 用户ID
    int      m_nVideoId;       // 视频ID
    STRU_VIDEO_CLICK_RQ() { memset(this, 0, sizeof(*this)); }
} STRU_VIDEO_CLICK_RQ;

// ===== 断点续传 v2（独立包类型，不破坏老上传/下载/GIF 路径） =====
#define _DEF_PACK_UPLOAD_V2_RQ   (_DEF_PACK_BASE + 20)   // 上传握手（带 md5 指纹）
#define _DEF_PACK_UPLOAD_V2_RS   (_DEF_PACK_BASE + 21)   // 握手回复（taskId + 续传偏移）
#define _DEF_PACK_UPLOAD_BLOCK   (_DEF_PACK_BASE + 22)   // 文件块（变长：头 + payload）
#define _DEF_PACK_UPLOAD_END_RQ  (_DEF_PACK_BASE + 23)   // 上传结束（请求终验）
#define _DEF_PACK_UPLOAD_END_RS  (_DEF_PACK_BASE + 24)   // 终验结果

// 握手结果
#define upload_v2_new     (1)   // 全新上传
#define upload_v2_resume  (2)   // 断点续传
#define upload_v2_instant (3)   // 秒传（同 md5 已存在）
#define upload_v2_busy    (4)   // 同(userId,md5)占用中
#define upload_v2_fail    (0)   // 失败

#pragma pack(push,1)
// 上传握手请求：客户端→服务端
typedef struct STRU_UPLOAD_V2_RQ
{
    STRU_UPLOAD_V2_RQ()
    {
        m_nType = _DEF_PACK_UPLOAD_V2_RQ;
        m_UserId = 0;
        m_nFileSize = 0;
        memset(m_szMd5, 0, 33);
        memset(m_szFileName, 0, _MAX_PATH);
        memset(m_szGifName, 0, _MAX_PATH);
        memset(m_szFileType, 0, _MAX_SIZE);
        memset(m_szHobby, 0, DEF_HOBBY_COUNT);
    }
    PackType m_nType;
    int m_UserId;
    int64_t m_nFileSize;
    char m_szMd5[33];                 // 文件内容指纹（全文件 MD5），稳定身份 + 终校
    char m_szHobby[DEF_HOBBY_COUNT];
    char m_szFileName[_MAX_PATH];
    char m_szGifName[_MAX_PATH];
    char m_szFileType[_MAX_SIZE];
} STRU_UPLOAD_V2_RQ;

// 上传握手回复：服务端→客户端
typedef struct STRU_UPLOAD_V2_RS
{
    STRU_UPLOAD_V2_RS()
    {
        m_nType = _DEF_PACK_UPLOAD_V2_RS;
        m_nResult = 0;
        m_nTaskId = 0;
        m_nResumeFrom = 0;
    }
    PackType m_nType;
    int m_nResult;          // upload_v2_new / resume / instant / busy / fail
    int m_nTaskId;          // 服务端分配，后续块/结束包携带
    int64_t m_nResumeFrom;  // 断点续传起点（已收字节数）
} STRU_UPLOAD_V2_RS;

// 文件块：变长。线上格式 = [本结构体(定长头)] + [m_nLen 字节 payload]，结构体内不放数组
typedef struct STRU_UPLOAD_BLOCK
{
    STRU_UPLOAD_BLOCK()
    {
        m_nType = _DEF_PACK_UPLOAD_BLOCK;
        m_nTaskId = 0;
        m_nOffset = 0;
        m_nLen = 0;
    }
    PackType m_nType;
    int m_nTaskId;
    int64_t m_nOffset;      // 本块在数据流中的字节偏移
    int m_nLen;             // 本块实际长度，<= _DEF_CONTENT_SIZE
} STRU_UPLOAD_BLOCK;

// 上传结束请求：客户端→服务端（触发终验）
typedef struct STRU_UPLOAD_END_RQ
{
    STRU_UPLOAD_END_RQ()
    {
        m_nType = _DEF_PACK_UPLOAD_END_RQ;
        m_nTaskId = 0;
        memset(m_szMd5, 0, 33);
    }
    PackType m_nType;
    int m_nTaskId;
    char m_szMd5[33];
} STRU_UPLOAD_END_RQ;

// 上传结束回复：服务端→客户端（终验结果）
typedef struct STRU_UPLOAD_END_RS
{
    STRU_UPLOAD_END_RS()
    {
        m_nType = _DEF_PACK_UPLOAD_END_RS;
        m_nResult = 0;
    }
    PackType m_nType;
    int m_nResult;          // 0=校验失败需重传 1=成功
} STRU_UPLOAD_END_RS;
#pragma pack(pop)
