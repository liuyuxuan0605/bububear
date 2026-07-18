
#ifndef CLOGIC_H
#define CLOGIC_H

#include"TCPKernel.h"
#include <mutex>
#include <string>
#include <map>

class CLogic
{
public:
    CLogic( TcpKernel* pkernel )
    {
        m_pKernel = pkernel;
        m_sql = pkernel->m_sql;
        m_tcp = pkernel->m_tcp;
    }
public:
    //设置协议映射
    void setNetPackMap();
    /************** 发送数据*********************/
    void SendData( sock_fd clientfd, char*szbuf, int nlen )
    {
        m_pKernel->SendData( clientfd ,szbuf , nlen );
    }
    /************** 网络处理 *********************/
    //注册
    void RegisterRq(sock_fd clientfd, char*szbuf, int nlen);
    //登录
    void LoginRq(sock_fd clientfd, char*szbuf, int nlen);
    //上传
    void UploadRq(sock_fd clientfd, char*szbuf, int nlen);
    //上传的文件块
    void UploadBlockRq(sock_fd clientfd, char*szbuf, int nlen);
    //下载
    void DownloadRq(sock_fd clientfd, char*szbuf, int nlen);
    // 上传
    void UploadHistoryRq(sock_fd clientfd, char*szbuf, int nlen);
    //直播
    void LiveStartRq(sock_fd clientfd, char* szbuf, int nlen);
    void LiveStopRq(sock_fd clientfd, char* szbuf, int nlen);
    void LiveListRq(sock_fd clientfd, char* szbuf, int nlen);
    //播放时长/完播率上报
    void PlayReportRq(sock_fd clientfd, char* szbuf, int nlen);
    //视频被点击播放，热度+5
    void VideoClickRq(sock_fd clientfd, char* szbuf, int nlen);
    // 断点续传 v2
    void UploadV2Rq(sock_fd clientfd, char*szbuf, int nlen);
    void UploadBlockRqV2(sock_fd clientfd, char*szbuf, int nlen);
    void UploadEndRqV2(sock_fd clientfd, char*szbuf, int nlen);

    /*******************************************/
    void close();
    //获取文件列表
    void GetFileList(list<FileInfo *> &fileList, int userid);
private:
    TcpKernel* m_pKernel;
    CMysql * m_sql;
    Block_Epoll_Net * m_tcp;

    map<int,int>m_mapIDToUserFD;
    map<int,FileInfo*>m_mapFileIDToFileInfo;
    // ===== v2 断点续传 =====
    int m_nNextTaskId;
    std::mutex m_v2Mutex;
    std::map<int, FileInfo*> m_mapTaskIdToFileInfo;
    std::map<std::string, time_t> m_mapBusy;                 // key: "userId_md5"，占用锁
    std::map<std::string, std::string> m_mapMd5ToFinalPath; // 会话内秒传索引(md5->最终路径)
    void FinalizeUpload(sock_fd clientfd, FileInfo* info, int taskId);
    void CleanupTask(int taskId);
};

#endif // CLOGIC_H
