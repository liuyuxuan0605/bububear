#include "clogic.h"
#include "crypto_util.h"
#include <openssl/md5.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

// v2 断点续传：.meta 元数据（二进制，便于原子写与续传时读回）
struct UploadMeta {
    char m_szMd5[33];
    int64_t m_nFileSize;
    int64_t m_nReceived;
    int m_UserId;
    char m_szHobby[DEF_HOBBY_COUNT];
    char m_szFileName[_MAX_PATH];
    char m_szGifName[_MAX_PATH];
    char m_szFileType[_MAX_SIZE];
    char m_szUserName[_MAX_SIZE];
    char m_szRtmp[_MAX_PATH];
    char m_szFinalPath[_MAX_PATH];
};

// 计算文件 MD5（全文件），结果写入 out（32 位十六进制小写 + \0）
static void Md5OfFile(const char* path, char out[33])
{
    out[0] = 0;
    MD5_CTX ctx; MD5_Init(&ctx);
    FILE* fp = fopen(path, "rb");
    if (!fp) return;
    unsigned char buf[64*1024];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0)
        MD5_Update(&ctx, buf, r);
    fclose(fp);
    unsigned char dig[MD5_DIGEST_LENGTH];
    MD5_Final(dig, &ctx);
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out[2*i]   = hex[dig[i] >> 4];
        out[2*i+1] = hex[dig[i] & 0xf];
    }
    out[32] = 0;
}

// 跨设备拷贝（rename 失败时回退），写完 fsync
static bool CopyFileRaw(const char* src, const char* dst)
{
    FILE* fin = fopen(src, "rb");  if (!fin) return false;
    FILE* fout = fopen(dst, "wb"); if (!fout) { fclose(fin); return false; }
    unsigned char buf[64*1024];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, r, fout) != r) { fclose(fin); fclose(fout); return false; }
    }
    fflush(fout);
    fsync(fileno(fout));
    fclose(fin); fclose(fout);
    return true;
}

// 持久化已收字节（续传点权威来源）：先写 .tmp 再 rename，原子
static bool WriteMeta(FileInfo* info)
{
    UploadMeta meta;
    memset(&meta, 0, sizeof(meta));
    strncpy(meta.m_szMd5, info->m_szMd5, 32);
    meta.m_nFileSize = info->m_nFileSize;
    meta.m_nReceived = info->m_nReceived;
    meta.m_UserId    = info->m_nUserId;
    memcpy(meta.m_szHobby, info->m_Hobby, DEF_HOBBY_COUNT);
    strncpy(meta.m_szFileName, info->m_szFileName, _MAX_PATH-1);
    strncpy(meta.m_szGifName, info->m_szGifName, _MAX_PATH-1);
    strncpy(meta.m_szFileType, info->m_szFileType, _MAX_SIZE-1);
    strncpy(meta.m_szUserName, info->m_UserName, _MAX_SIZE-1);
    strncpy(meta.m_szRtmp, info->m_szRtmp, _MAX_PATH-1);
    strncpy(meta.m_szFinalPath, info->m_szFilePath, _MAX_PATH-1);

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", info->m_szMetaPath);
    FILE* fp = fopen(tmp, "wb");
    if (!fp) return false;
    fwrite(&meta, 1, sizeof(meta), fp);
    fclose(fp);
    rename(tmp, info->m_szMetaPath);
    return true;
}

static bool ReadMeta(const char* path, UploadMeta* meta)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    size_t r = fread(meta, 1, sizeof(*meta), fp);
    fclose(fp);
    return r == sizeof(*meta);
}

void CLogic::setNetPackMap()
{
    NetPackMap(_DEF_PACK_REGISTER_RQ)    = &CLogic::RegisterRq;
    NetPackMap(_DEF_PACK_LOGIN_RQ)       = &CLogic::LoginRq;
    NetPackMap(_DEF_PACK_UPLOAD_RQ)      =&CLogic::UploadRq;
    NetPackMap(_DEF_PACK_FILEBLOCK_RQ)   =&CLogic::UploadBlockRq;
    NetPackMap(DEF_PACK_DOWNLOAD_RQ)     =&CLogic::DownloadRq;
    NetPackMap(_DEF_PACK_UPLOADHISTORY_RQ) = &CLogic::UploadHistoryRq;
    NetPackMap(_DEF_PACK_LIVE_START_RQ) = &CLogic::LiveStartRq;
    NetPackMap(_DEF_PACK_LIVE_STOP_RQ)  = &CLogic::LiveStopRq;
    NetPackMap(_DEF_PACK_LIVE_LIST_RQ)  = &CLogic::LiveListRq;
    NetPackMap(_DEF_PACK_PLAY_REPORT_RQ) = &CLogic::PlayReportRq;
    NetPackMap(_DEF_PACK_VIDEO_CLICK_RQ) = &CLogic::VideoClickRq;
    // 断点续传 v2
    NetPackMap(_DEF_PACK_UPLOAD_V2_RQ)  = &CLogic::UploadV2Rq;
    NetPackMap(_DEF_PACK_UPLOAD_BLOCK)  = &CLogic::UploadBlockRqV2;
    NetPackMap(_DEF_PACK_UPLOAD_END_RQ) = &CLogic::UploadEndRqV2;
}

#define RootPath "/home/colin/video/"
#define _DEF_COUT_FUNC_    cout << "clientfd:"<< clientfd << __func__ << endl;

void CLogic::close()
{
    for(auto ite=m_mapFileIDToFileInfo.begin();ite!=m_mapFileIDToFileInfo.end();++ite)
    {
        delete ite->second;
    }
    m_mapFileIDToFileInfo.clear();
    m_mapIDToUserFD.clear();
    // v2 清理
    for(auto ite=m_mapTaskIdToFileInfo.begin();ite!=m_mapTaskIdToFileInfo.end();++ite)
    {
        if(ite->second->pFile) fclose(ite->second->pFile);
        delete ite->second;
    }
    m_mapTaskIdToFileInfo.clear();
    m_mapBusy.clear();
    m_mapMd5ToFinalPath.clear();
    m_mapLastPushed.clear();
}

void CLogic::OnClientDisconnect(sock_fd clientfd)
{
    // 客户端异常断开（不发 END 直接关 socket）：清掉该连接关联的 v2 task + busy 锁，
    // 但【保留】.part/.meta 文件，供客户端断点续传。
    std::lock_guard<std::mutex> lk(m_v2Mutex);
    for (auto it = m_mapTaskIdToFileInfo.begin(); it != m_mapTaskIdToFileInfo.end(); ) {
        FileInfo* info = it->second;
        if (info && info->m_clientfd == clientfd) {
            if (info->pFile) { fclose(info->pFile); info->pFile = NULL; }
            char busyKey[160];
            snprintf(busyKey, sizeof(busyKey), "%d_%s", info->m_nUserId, info->m_szMd5);
            m_mapBusy.erase(busyKey);
            delete info;
            it = m_mapTaskIdToFileInfo.erase(it);
        } else {
            ++it;
        }
    }
}

//注册
void CLogic::RegisterRq(sock_fd clientfd,char* szbuf,int nlen)
{
    _DEF_COUT_FUNC_
   
    STRU_REGISTER_RQ*rq=(STRU_REGISTER_RQ*)szbuf;
    STRU_REGISTER_RS rs;

    char sqlBuf[ _DEF_SQLIEN]=" ";
    sprintf(sqlBuf,"select name from t_UserData where name='%s';",rq->user);
    list<string> resList;
    bool res=m_sql->SelectMysql(sqlBuf,1,resList);
    if(!res){
        cout<<"SelectMysql error:"<<sqlBuf<<endl;
        return;
    }

    if(resList.size()>0){
        rs.result=user_is_exist;
    }else{
        char sqlBuf[ _DEF_SQLIEN]=" ";
        string salt=genSalt();
        string passHash=sha256Hex(salt+rq->password);
        sprintf(sqlBuf,"insert into t_UserData(name,password,salt,food,funny,ennegy,dance,music,video,outside,edu) values('%s','%s','%s',%d,%d,%d,%d,%d,%d,%d,%d);",
                rq->user,passHash.c_str(),salt.c_str(),rq->food,rq->funny,rq->ennegy,rq->dance,rq->music,rq->video,rq->outside,rq->edu);
        m_sql->UpdataMysql(sqlBuf);

        sprintf(sqlBuf,"select id from t_UserData where name='%s';",rq->user);
        list<string> resID;
        m_sql->SelectMysql(sqlBuf,1,resID);
        int id=0;
        if(resID.size()>0){
            id=atoi(resID.front().c_str());
        }
       // rs.userid=id;

        //新注册的用户创建一个路径
        char path[_MAX_PATH]="";
        sprintf(path,"%sflv/%s/",RootPath,rq->user); //home/colin/video/flv/uer

        umask(0);
        mkdir(path,S_IRWXU|S_IRWXG|S_IRWXO);

        rs.result=register_success;
    }
    m_tcp->SendData(clientfd,(char*)&rs,sizeof(rs));
}

//登录
void CLogic::LoginRq(sock_fd clientfd ,char* szbuf,int nlen)
{
    _DEF_COUT_FUNC_
    STRU_LOGIN_RQ*rq=(STRU_LOGIN_RQ*)szbuf;
    STRU_LOGIN_RS rs;
    char sqlBuf[ _DEF_SQLIEN]=" ";
    sprintf(sqlBuf,"select password,salt,id from t_UserData where name='%s';",rq->user);
    list<string> resList;
    bool res=m_sql->SelectMysql(sqlBuf,3,resList);
    if(!res){
        cout<<"SelectMysql error:"<<sqlBuf<<endl;
        return;
    }
    if(resList.size()>0){
        string storedHash=resList.front(); resList.pop_front();
        string salt=resList.front(); resList.pop_front();
        string calcHash=sha256Hex(salt+rq->password);
        if(strcmp(storedHash.c_str(),calcHash.c_str())==0){
            rs.result=login_success;
            rs.userid=atoi(resList.front().c_str());
            // 注意：这里不要再 SendData，统一由函数末尾发送一次，
            // 否则登录成功会发两个包，客户端弹两次"登录成功"

            //存储映射关系
            this->m_mapIDToUserFD[rs.userid]=clientfd;
        }else{
            rs.result=password_error;
        }
    }else{
        rs.result=user_not_exist;
    }

    m_tcp->SendData( clientfd , (char*)&rs , sizeof rs );
}
//上传请求
void CLogic::UploadRq(sock_fd clientfd, char *szbuf, int nlen)
{
    _DEF_COUT_FUNC_

    STRU_UPLOAD_RQ *rq=(STRU_UPLOAD_RQ*)szbuf;

    FileInfo *info=new FileInfo;
    info->m_nPos=0;
    memcpy(info->m_Hobby,rq->m_szHobby,DEF_HOBBY_COUNT);

    info->m_nUserId = rq->m_UserId;
    info->m_nFileID = rq->m_nFileId;
    info->m_VideoID = 0;

    info->m_nFileSize = rq->m_nFileSize;


    strcpy(info->m_szFileName, rq->m_szFileName);
    strcpy(info->m_szFileType, rq->m_szFileType);

    //找到用户名
    char sqlBuf[_DEF_SQLIEN] = "";
    sprintf(sqlBuf, "select name from t_UserData where id = %d;", info->m_nUserId);
    list<string> resList;
    if(!m_sql->SelectMysql(sqlBuf, 1, resList))
    {
        cout<<"SelectMysql error:"<<sqlBuf<<endl;
        delete info;
        return;
    }
    if(resList.size()<=0)
    {
        delete info;
        return;
    }

    strcpy(info->m_UserName,resList.front().c_str());
    sprintf(info->m_szFilePath,"%sflv/%s/%s",RootPath,info->m_UserName,info->m_szFileName);
    sprintf( info->m_szRtmp,"//%s/%s",info->m_UserName,info->m_szFileName);

    if(strcmp(rq->m_szFileType, "gif") != 0)
    {
        strcpy(info->m_szGifName, rq->m_szGifName);
        sprintf(info->m_szGifPath,"%sflv/%s/%s",RootPath,info->m_UserName,info->m_szGifName);
    }
   info->pFile=fopen(info->m_szFilePath,"w");

   m_mapFileIDToFileInfo[info->m_nFileID]=info;
}
//上传的文件块
void CLogic::UploadBlockRq(sock_fd clientfd, char *szbuf, int nlen)
{
     _DEF_COUT_FUNC_

     STRU_FILEBLOCK_RQ *rq = (STRU_FILEBLOCK_RQ *)szbuf;
     if (m_mapFileIDToFileInfo.find(rq->m_nFileId) == m_mapFileIDToFileInfo.end())
         return;

     FileInfo* info = m_mapFileIDToFileInfo[rq->m_nFileId];

     int64_t res = fwrite(rq->m_szFileContent, 1, rq->m_nBlockLen, info->pFile);
     info->m_nPos += res;

     if (rq->m_nBlockLen < _DEF_CONTENT_SIZE || info->m_nPos >= info->m_nFileSize)
     {
         // 写完了
         fclose(info->pFile);

         // 判断 不是gif 写表记录  返回信息
         if (strcmp(info->m_szFileType, "gif") != 0)
         {
             // 写表
             char sqlBuf[_DEF_SQLIEN] = "";
             sprintf(sqlBuf,
                     "INSERT INTO t_VideoInfo (userId, videoName, picName, videoPath, picPath, rtmp, food, funny, ennegy, dance, music, video, outside, edu, hotdegree) values (%d, '%s', '%s', '%s', '%s', '%s', %d, %d, %d, %d, %d, %d, %d, %d, %d);"
                     ,info->m_nUserId,info->m_szFileName,info->m_szGifName,info->m_szFilePath,info->m_szGifPath,info->m_szRtmp,
                     info->m_Hobby[0],info->m_Hobby[1],info->m_Hobby[2],info->m_Hobby[3],info->m_Hobby[4],info->m_Hobby[5],info->m_Hobby[6],info->m_Hobby[7], 0);
             if(!m_sql->UpdataMysql(sqlBuf)){
                   cout<<"UpdataMysql error:"<<sqlBuf<<endl;
             }
             // 返回
             STRU_UPLOAD_RS rs;
             rs.m_nResult = 1;
             m_tcp->SendData(clientfd,(char*)&rs, sizeof(rs));
         }

        m_mapFileIDToFileInfo.erase(rq->m_nFileId);
        delete info;
        info = NULL;
    }
}

// ===================== 断点续传 v2 =====================

void CLogic::UploadV2Rq(sock_fd clientfd, char *szbuf, int nlen)
{
    _DEF_COUT_FUNC_
    STRU_UPLOAD_V2_RQ *rq = (STRU_UPLOAD_V2_RQ*)szbuf;
    STRU_UPLOAD_V2_RS rs;
    rs.m_nType = _DEF_PACK_UPLOAD_V2_RS;
    rs.m_nResult = upload_v2_fail;
    rs.m_nTaskId = 0;
    rs.m_nResumeFrom = 0;

    // 0. 基本校验
    if (rq->m_UserId <= 0 || rq->m_nFileSize <= 0 || strlen(rq->m_szMd5) != 32) {
        m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }

    // 1. 秒传（会话内 md5 索引）
    {
        std::lock_guard<std::mutex> lk(m_v2Mutex);
        auto it = m_mapMd5ToFinalPath.find(rq->m_szMd5);
        if (it != m_mapMd5ToFinalPath.end() && access(it->second.c_str(), F_OK) == 0) {
            rs.m_nResult = upload_v2_instant;
            rs.m_nResumeFrom = rq->m_nFileSize;
            m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
            return;
        }
    }

    // 2. 占用中（同 userId_md5 且未超时）
    char busyKey[160];
    snprintf(busyKey, sizeof(busyKey), "%d_%s", rq->m_UserId, rq->m_szMd5);
    {
        std::lock_guard<std::mutex> lk(m_v2Mutex);
        auto bit = m_mapBusy.find(busyKey);
        if (bit != m_mapBusy.end()) {
            if (time(NULL) - bit->second < 1800) { // 30 分钟占用超时
                rs.m_nResult = upload_v2_busy;
                m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
                return;
            }
            m_mapBusy.erase(bit); // 超时释放
        }
    }

    // 3. 查用户名 + 拼路径（沿用老逻辑）
    char sqlBuf[_DEF_SQLIEN] = "";
    sprintf(sqlBuf, "select name from t_UserData where id = %d;", rq->m_UserId);
    list<string> resList;
    if (!m_sql->SelectMysql(sqlBuf, 1, resList) || resList.size() <= 0) {
        m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }
    const char* userName = resList.front().c_str();

    FileInfo *info = new FileInfo;
    memset(info, 0, sizeof(FileInfo));
    info->m_nUserId = rq->m_UserId;
    info->m_clientfd = clientfd;
    info->m_nFileSize = rq->m_nFileSize;
    info->m_nPos = 0;
    info->m_nReceived = 0;
    info->m_nLastActive = time(NULL);
    strncpy(info->m_UserName, userName, _MAX_SIZE-1);
    strncpy(info->m_szMd5, rq->m_szMd5, 32);
    strncpy(info->m_szFileName, rq->m_szFileName, _MAX_PATH-1);
    strncpy(info->m_szFileType, rq->m_szFileType, _MAX_SIZE-1);
    strncpy(info->m_szGifName, rq->m_szGifName, _MAX_PATH-1);
    memcpy(info->m_Hobby, rq->m_szHobby, DEF_HOBBY_COUNT);
    sprintf(info->m_szFilePath, "%sflv/%s/%s", RootPath, userName, rq->m_szFileName);
    sprintf(info->m_szRtmp, "//%s/%s", userName, rq->m_szFileName);
    if (strcmp(info->m_szFileType, "gif") != 0)
        sprintf(info->m_szGifPath, "%sflv/%s/%s", RootPath, userName, rq->m_szGifName);
    sprintf(info->m_szPartPath, "%s.part", info->m_szFilePath);
    sprintf(info->m_szMetaPath, "%s.meta", info->m_szFilePath);

    int64_t resumeFrom = 0;
    rs.m_nResult = upload_v2_new;

    // 4. 续传 or 新建
    if (access(info->m_szMetaPath, F_OK) == 0) {
        UploadMeta meta;
        if (ReadMeta(info->m_szMetaPath, &meta)
            && strcmp(meta.m_szMd5, info->m_szMd5) == 0
            && meta.m_nReceived <= info->m_nFileSize) {
            info->pFile = fopen(info->m_szPartPath, "rb+");
            if (info->pFile) {
                fseek(info->pFile, 0, SEEK_END);
                int64_t sz = ftell(info->pFile);
                info->m_nReceived = (sz < meta.m_nReceived) ? sz : meta.m_nReceived;
                if (info->m_nReceived > info->m_nFileSize) info->m_nReceived = info->m_nFileSize;
                resumeFrom = info->m_nReceived;
                rs.m_nResult = upload_v2_resume;
            } else {
                remove(info->m_szMetaPath); // .part 丢失，退回新建
            }
        } else {
            remove(info->m_szMetaPath);
            remove(info->m_szPartPath);
        }
    }
    if (rs.m_nResult != upload_v2_resume) {
        info->pFile = fopen(info->m_szPartPath, "wb");
        info->m_nReceived = 0;
        resumeFrom = 0;
        rs.m_nResult = upload_v2_new;
    }
    if (!info->pFile) {
        delete info;
        m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }

    // 5. 分配 taskId，登记
    {
        std::lock_guard<std::mutex> lk(m_v2Mutex);
        info->m_nTaskId = m_nNextTaskId++;
        m_mapTaskIdToFileInfo[info->m_nTaskId] = info;
        m_mapBusy[busyKey] = time(NULL);
    }
    rs.m_nTaskId = info->m_nTaskId;
    rs.m_nResumeFrom = resumeFrom;
    m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
}

void CLogic::UploadBlockRqV2(sock_fd clientfd, char *szbuf, int nlen)
{
    STRU_UPLOAD_BLOCK *rq = (STRU_UPLOAD_BLOCK*)szbuf;
    // 长度自洽检查
    if (rq->m_nLen <= 0 || rq->m_nLen > _DEF_CONTENT_SIZE) return;
    if (nlen < (int)(sizeof(STRU_UPLOAD_BLOCK) + rq->m_nLen)) return;

    FileInfo* info = NULL;
    {
        std::lock_guard<std::mutex> lk(m_v2Mutex);
        auto it = m_mapTaskIdToFileInfo.find(rq->m_nTaskId);
        if (it == m_mapTaskIdToFileInfo.end()) return;
        info = it->second;
    }

    char* payload = (char*)szbuf + sizeof(STRU_UPLOAD_BLOCK);
    // 仅拒 offset > received（跳块/乱序/恶意）；offset <= received 允许幂等覆盖（重传不翻倍）
    if (rq->m_nOffset > info->m_nReceived) return;

    fseek(info->pFile, rq->m_nOffset, SEEK_SET);
    size_t w = fwrite(payload, 1, rq->m_nLen, info->pFile);
    if (w != (size_t)rq->m_nLen) return; // 写入失败
    if (rq->m_nOffset + rq->m_nLen > info->m_nReceived)
        info->m_nReceived = rq->m_nOffset + rq->m_nLen;
    fflush(info->pFile);
    WriteMeta(info); // 续传点权威来源（fflush 后才更新）
    info->m_nLastActive = time(NULL);
    {
        std::lock_guard<std::mutex> lk(m_v2Mutex);
        char busyKey[160];
        snprintf(busyKey, sizeof(busyKey), "%d_%s", info->m_nUserId, info->m_szMd5);
        m_mapBusy[busyKey] = time(NULL);
    }
    // 完成判定交由 UploadEndRqV2（显式终验），此处不 rename、不回执
}

void CLogic::UploadEndRqV2(sock_fd clientfd, char *szbuf, int nlen)
{
    STRU_UPLOAD_END_RQ *rq = (STRU_UPLOAD_END_RQ*)szbuf;
    FileInfo* info = NULL;
    {
        std::lock_guard<std::mutex> lk(m_v2Mutex);
        auto it = m_mapTaskIdToFileInfo.find(rq->m_nTaskId);
        if (it == m_mapTaskIdToFileInfo.end()) return;
        info = it->second;
    }
    FinalizeUpload(clientfd, info, rq->m_nTaskId);
}

void CLogic::FinalizeUpload(sock_fd clientfd, FileInfo* info, int taskId)
{
    STRU_UPLOAD_END_RS rs;
    rs.m_nType = _DEF_PACK_UPLOAD_END_RS;
    rs.m_nResult = 0;

    char calc[33];
    Md5OfFile(info->m_szPartPath, calc);
    if (strcmp(calc, info->m_szMd5) != 0) {
        // 校验失败：删 .part/.meta，释放
        m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
        remove(info->m_szPartPath);
        remove(info->m_szMetaPath);
        CleanupTask(taskId);
        return;
    }

    // 校验通过：.part -> 正式文件名（跨设备健壮化）
    if (rename(info->m_szPartPath, info->m_szFilePath) != 0) {
        if (errno == EXDEV) {
            if (!CopyFileRaw(info->m_szPartPath, info->m_szFilePath)) {
                m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
                remove(info->m_szPartPath); remove(info->m_szMetaPath); CleanupTask(taskId); return;
            }
            remove(info->m_szPartPath);
        } else {
            m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
            remove(info->m_szPartPath); remove(info->m_szMetaPath); CleanupTask(taskId); return;
        }
    }
    remove(info->m_szMetaPath);

    // 写库（非 gif）
    if (strcmp(info->m_szFileType, "gif") != 0) {
        char sqlBuf[_DEF_SQLIEN] = "";
        sprintf(sqlBuf,
            "INSERT INTO t_VideoInfo (userId, videoName, picName, videoPath, picPath, rtmp, food, funny, ennegy, dance, music, video, outside, edu, hotdegree) values (%d, '%s', '%s', '%s', '%s', '%s', %d, %d, %d, %d, %d, %d, %d, %d, %d);",
            info->m_nUserId, info->m_szFileName, info->m_szGifName, info->m_szFilePath, info->m_szGifPath, info->m_szRtmp,
            info->m_Hobby[0], info->m_Hobby[1], info->m_Hobby[2], info->m_Hobby[3], info->m_Hobby[4], info->m_Hobby[5], info->m_Hobby[6], info->m_Hobby[7], 0);
        m_sql->UpdataMysql(sqlBuf);
    }

    // 会话内秒传索引
    {
        std::lock_guard<std::mutex> lk(m_v2Mutex);
        m_mapMd5ToFinalPath[info->m_szMd5] = info->m_szFilePath;
    }
    rs.m_nResult = 1;
    m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
    CleanupTask(taskId);
}

// 功能：收尾一个上传任务——关文件、释放占用锁、从任务表删掉、释放内存
// 调用时机：①上传正常完成(FinalizeUpload 成功) ②上传失败/校验没过
void CLogic::CleanupTask(int taskId)
{
    std::lock_guard<std::mutex> lk(m_v2Mutex);
    auto it = m_mapTaskIdToFileInfo.find(taskId);
    if (it == m_mapTaskIdToFileInfo.end()) return;
    FileInfo* info = it->second;
    if (info->pFile) fclose(info->pFile);
    char busyKey[160];
    snprintf(busyKey, sizeof(busyKey), "%d_%s", info->m_nUserId, info->m_szMd5);
    m_mapBusy.erase(busyKey);
    m_mapTaskIdToFileInfo.erase(it);
    delete info;
}

// ===================== 启动扫描：孤儿临时文件 TTL 清理 =====================
// 判据：.meta 每收块 rename 重写、.part 追加写都会刷新 mtime，故 mtime = 最后活跃时间。
// 超过 ORPHAN_TTL_DAYS 天未活跃的 .part/.meta 视为孤儿（客户端断连后未续传），启动时清理。
// 仅匹配 *.meta / *.part 后缀，绝不删除 .flv 等成品；删除时配对删除同名兄弟文件。
#ifndef ORPHAN_TTL_DAYS
#define ORPHAN_TTL_DAYS 7
#endif
#ifndef ORPHAN_DRYRUN
#define ORPHAN_DRYRUN 1   // 1=只统计不删（安全默认），确认无误后改为 0 开启真删
#endif

// 删除 path 的同名"兄弟"文件（.meta<->.part 互转）
static void TryRemoveSibling(const char* path, bool isMeta)
{
    std::string s(path);
    if (s.size() <= 5) return;
    s.resize(s.size() - 5);                  // 去掉 ".meta" / ".part"
    s += (isMeta ? ".part" : ".meta");
    if (access(s.c_str(), F_OK) == 0) {
        if (ORPHAN_DRYRUN) {
            printf("[ORPHAN-CLEAN]   (dry-run) sibling would be removed: %s\n", s.c_str());
        } else if (remove(s.c_str()) == 0) {
            printf("[ORPHAN-CLEAN]   removed sibling: %s\n", s.c_str());
        }
    }
}

void CLogic::CleanupOrphanTempFiles()
{
    char root[1024];
    snprintf(root, sizeof(root), "%sflv/", RootPath); // /home/colin/video/flv/

    DIR* dUser = opendir(root);
    if (!dUser) {
        printf("[ORPHAN-CLEAN] cannot open root dir: %s (errno=%d), skip\n", root, errno);
        return;
    }

    printf("[ORPHAN-CLEAN] start, dir=%s, threshold=%d days%s\n",
           root, ORPHAN_TTL_DAYS, ORPHAN_DRYRUN ? " (DRY-RUN)" : "");

    time_t now = time(NULL);
    long long ttl = (long long)ORPHAN_TTL_DAYS * 24 * 3600;
    int candidates = 0, stale = 0, kept = 0, removed = 0;

    struct dirent* de;
    while ((de = readdir(dUser)) != NULL) {
        if (de->d_name[0] == '.') continue;            // 跳过 . 和 ..
        char userDir[2048];
        snprintf(userDir, sizeof(userDir), "%s%s/", root, de->d_name);

        DIR* dFile = opendir(userDir);
        if (!dFile) continue;

        struct dirent* fe;
        while ((fe = readdir(dFile)) != NULL) {
            const char* name = fe->d_name;
            size_t nl = strlen(name);
            bool isMeta = (nl > 5 && strcmp(name + nl - 5, ".meta") == 0);
            bool isPart = (nl > 5 && strcmp(name + nl - 5, ".part") == 0);
            if (!isMeta && !isPart) continue;          // 只处理 .meta/.part，防误删 .flv

            char full[2048];
            snprintf(full, sizeof(full), "%s%s", userDir, name);

            struct stat st;
            if (stat(full, &st) != 0) continue;

            ++candidates;
            long long age = (long long)now - (long long)st.st_mtime;
            if (age > ttl) {
                ++stale;
                printf("[ORPHAN-CLEAN] stale(%s, age=%lld d): %s\n",
                       isMeta ? "meta" : "part", age / 86400, full);
                TryRemoveSibling(full, isMeta);        // 配对删除兄弟
                if (!ORPHAN_DRYRUN && remove(full) == 0) ++removed;
            } else {
                ++kept;
            }
        }
        closedir(dFile);
    }
    closedir(dUser);

    printf("[ORPHAN-CLEAN] done. candidates=%d, stale=%d, kept=%d, removed=%d%s\n",
           candidates, stale, kept, removed,
           ORPHAN_DRYRUN ? " (DRY-RUN, nothing deleted)" : "");
}

void CLogic::DownloadRq(sock_fd clientfd, char *szbuf, int nlen)
{
     _DEF_COUT_FUNC_
             // 1. 校验接收的长度是否足够装下结构体
                 if (nlen < sizeof(STRU_DOWNLOAD_RQ))
                 {
                     printf("非法包：长度不够，丢弃！nlen=%d\n", nlen);
                     return;
                 }
                 // 2. 校验指针不能为空
                 if (!szbuf)
                 {
                     printf("szbuf 为空！\n");
                     return;
                 }
     STRU_DOWNLOAD_RQ *rq=(STRU_DOWNLOAD_RQ*)szbuf;
     list<FileInfo*> fileList;

     GetFileList(fileList,rq->m_nUserId);

     while(fileList.size()>0)
     {
         FileInfo *info=fileList.front();
         fileList.pop_front();

         STRU_DOWNLOAD_RS rs;
         strcpy( rs.m_rtmp , info->m_szRtmp );
         rs.m_nFileId = info->m_nFileID;
         rs.m_nVideoId = info->m_VideoID;
         rs.m_nFileSize = info->m_nFileSize;
         strcpy( rs.m_szFileName , info->m_szFileName );

         m_tcp->SendData( clientfd , (char*)&rs , sizeof(rs) );
         cout<<"Send  STRU_DOWNLOAD_RS: "<<endl;

         info->pFile = fopen( info->m_szFilePath , "r");
         if( !info->pFile )
         {
             cout<<"Open file failed: "<<info->m_szFilePath<<endl;
             delete info;
             continue;
         }
         if( info->pFile )
         {
             while(1)
             {
                 STRU_FILEBLOCK_RQ blockrq;
                 // 满块读取（_DEF_CONTENT_SIZE=64KB）。此前写死 1024：
                 // 每 1KB 有效数据发 64KB+ 结构体，带宽放大 64 倍
                 int64_t res = fread( blockrq.m_szFileContent, 1 , _DEF_CONTENT_SIZE , info->pFile );
                 if(res <= 0)
                 {
                     cout<<"res<=0"<<endl;
                     // 读取结束或出错
                     break;
                 }
                 blockrq.m_nBlockLen = res;
                 info->m_nPos += res;
                 blockrq.m_nFileId=info->m_VideoID;
                 blockrq.m_nUserId=rq->m_nUserId;

                 // 变长发送：定长头 + 实际数据，末块不再发整个 64KB 结构体。
                 // m_szFileContent 是最后一个成员，客户端按 m_nBlockLen 读，天然兼容
                 int nSendLen = sizeof(STRU_FILEBLOCK_RQ) - _DEF_CONTENT_SIZE + (int)res;
                 int ret = m_tcp->SendData(clientfd, (char*)&blockrq, nSendLen);
                 if (ret < 0) {
                     cout << "SendData failed, client disconnected" << endl;
                     break; // 退出循环，不再发送
                 }

                 if( info->m_nPos >= info->m_nFileSize )
                 {
                     fclose(info->pFile);
                     delete info;
                     info = NULL;
                     break;
                 }
             }
         }
     }
     cout<<"All video send done, close client: "<<clientfd<<endl;
}

void CLogic::UploadHistoryRq(sock_fd clientfd, char *szbuf, int nlen)
{
    STRU_UPLOADHISTORY_RQ *rq = (STRU_UPLOADHISTORY_RQ*)szbuf;

    char sqlBuf[1024] = "";
    list<string> resList;

    // 查询该用户上传的所有视频（过滤缩略图字段为空的脏数据，如压测工具插入的记录）
    sprintf(sqlBuf,
        "select videoId, picName, picPath, rtmp from t_VideoInfo where userId = %d and picName <> '' and picPath <> '' order by hotdegree desc;",
        rq->m_nUserId);

    if(!m_sql->SelectMysql(sqlBuf, 4, resList)) {
        cout << "SelectMysql error:" << sqlBuf << endl;
        return;
    }

    int nCount = 0;
    int nSize = resList.size() / 4;

    for(int i = 0; i < nSize; ++i)
    {
        FileInfo *info = new FileInfo;
        info->m_nPos = 0;

        info->m_VideoID = atoi(resList.front().c_str());
        resList.pop_front();

        strcpy(info->m_szFileName, resList.front().c_str());
        resList.pop_front();

        strcpy(info->m_szFilePath, resList.front().c_str());
        resList.pop_front();

        strcpy(info->m_szRtmp, resList.front().c_str());
        resList.pop_front();

        info->m_nFileID = nCount++;

        // 只发送 GIF 缩略图（picPath），不需要发送视频本体
        // 构造 GIF 的文件路径
        info->pFile = fopen(info->m_szFilePath, "r");  // picPath 是 GIF 路径
        if(!info->pFile) {
            delete info;
            continue;
        }
        fseek(info->pFile, 0, SEEK_END);
        info->m_nFileSize = ftell(info->pFile);
        fseek(info->pFile, 0, SEEK_SET);

        // 发送回复头（复用 STRU_DOWNLOAD_RS 结构，但包类型必须改成 UPLOADHISTORY_RS——
        // 构造函数默认 DOWNLOAD_RS，不改的话客户端会当成推荐页下载头处理，历史页永远空白）
        STRU_DOWNLOAD_RS rs;
        rs.m_nType = _DEF_PACK_UPLOADHISTORY_RS;
        strcpy(rs.m_szFileName, info->m_szFileName);
        rs.m_nFileId = info->m_nFileID;
        rs.m_nVideoId = info->m_VideoID;
        rs.m_nFileSize = info->m_nFileSize;
        strcpy(rs.m_rtmp, info->m_szRtmp);
        m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));

        // 发送 GIF 文件块（满块读取 + 变长发送，同 DownloadRq 修复）
        while(1)
        {
            STRU_FILEBLOCK_RQ blockrq;
            int64_t res = fread(blockrq.m_szFileContent, 1, _DEF_CONTENT_SIZE, info->pFile);
            if(res <= 0) break;

            blockrq.m_nBlockLen = res;
            info->m_nPos += res;
            blockrq.m_nFileId = info->m_VideoID;
            blockrq.m_nUserId = rq->m_nUserId;

            int nSendLen = sizeof(STRU_FILEBLOCK_RQ) - _DEF_CONTENT_SIZE + (int)res;
            m_tcp->SendData(clientfd, (char*)&blockrq, nSendLen);

            if(info->m_nPos >= info->m_nFileSize) break;
        }

        fclose(info->pFile);
        delete info;
    }
}

// 推荐辅助：执行一条4列(videoId,picName,picPath,rtmp)的推荐SQL，解析结果并校验缩略图，
// 合法条目追加进 fileList。slotIndex 是 UI 坑位号(m_nFileID)累加器；
// excludeIds/pickedIds 同步记录已选中的 videoId，供后续查询排除。返回实际加入条数。
int CLogic::FetchVideos(const char* sql, list<FileInfo*>& fileList,
                        int& slotIndex, string& excludeIds, std::set<int>& pickedIds)
{
    list<string> resList;
    if(!m_sql->SelectMysql((char*)sql, 4, resList))
    {
        cout<<"SelectMysql error:"<<sql<<endl;
        return 0;
    }
    int nAdded = 0;
    int nSize = resList.size()/4;
    for(int i=0; i<nSize; ++i)
    {
        FileInfo *info=new FileInfo;
        info->m_nPos=0;
        info->m_VideoID=atoi(resList.front().c_str());
        resList.pop_front();
        strcpy(info->m_szFileName,resList.front().c_str());
        resList.pop_front();
        strcpy(info->m_szFilePath,resList.front().c_str());
        resList.pop_front();
        strcpy(info->m_szRtmp,resList.front().c_str());
        resList.pop_front();

        // 批内去重：t_UserRecv 历史数据可能有同 (userId,videoId) 重复行，
        // join 会把同一视频返回多次，不去重会占掉多个坑位
        if(pickedIds.count(info->m_VideoID) > 0)
        {
            delete info;
            continue;
        }

        // 校验缩略图文件真实可读且非空（历史上传失败留下的脏数据不能占坑位）
        info->pFile=fopen(info->m_szFilePath,"r");
        if(!info->pFile)
        {
            cout<<"[FetchVideos] Open file failed: "<<info->m_szFilePath<<endl;
            delete info;
            continue;
        }
        fseek(info->pFile,0,SEEK_END);
        info->m_nFileSize=ftell(info->pFile);
        fclose(info->pFile);
        info->pFile=NULL;
        if(info->m_nFileSize==0)
        {
            cout<<"[FetchVideos] Thumbnail is empty, skip: "<<info->m_szFilePath<<endl;
            delete info;
            continue;
        }

        info->m_nFileID = slotIndex++;
        excludeIds += "," + to_string(info->m_VideoID);
        pickedIds.insert(info->m_VideoID);
        fileList.push_back(info);
        ++nAdded;
        cout << "[FetchVideos] videoId=" << info->m_VideoID
             << " slot=" << info->m_nFileID << " " << info->m_szFileName << endl;
    }
    return nAdded;
}

void CLogic::GetFileList(list<FileInfo*>&fileList,int userid)
{
    cout << "[GetFileList] Start, userid: " << userid << endl;
    // 推荐策略（每次刷新最多10个坑位）：
    //   t_UserRecv 语义 = 【看过】（VideoClickRq 点击播放时写入 + recvTime）
    //   ① 优先推没看过、且不在"上一批推送"里的视频（7个按标签匹配+热度，3个纯热度防茧房）
    //   ② 没看过的不够10条 → 用没看过但上一批推过的补（"换一批"轮转）
    //   ③ 还不够 → 用看过的补位，最早看过的(recvTime最旧)优先 → 不留空白坑位
    //   ④ 全部视频都看完 → 清空看过记录，开启新一轮循环
    // "换一批"：m_mapLastPushed 记住上一批10条；下次刷新①阶段避开它们，
    //   所以哪怕一条都没点开，连点两次刷新看到的也是不同批次（视频总量>10时）

    char sqlBuf[2048] = "";
    list<string> resList;
    const char* dirtyFilter = "picName <> '' and picPath <> ''"; // 统一脏数据过滤

    // ---- 统计还没看过的视频数（过滤条件必须与推荐查询一致，否则重置循环永远不触发）----
    sprintf(sqlBuf,"select count(videoId) from t_VideoInfo where %s and videoId not in "
                   "( select videoId from t_UserRecv where userId = %d);", dirtyFilter, userid);
    if(!m_sql->SelectMysql(sqlBuf,1,resList) || resList.size()==0)
    {
        cout<<"SelectMysql error or empty:"<<sqlBuf<<endl;
        return;
    }
    int nUnwatched=atoi(resList.front().c_str());
    cout << "[GetFileList] Unwatched video count: " << nUnwatched << endl;

    // ---- ④ 全部看完：清空"看过"记录，重开循环 ----
    if(nUnwatched == 0)
    {
        cout << "[GetFileList] All watched, reset t_UserRecv for new cycle" << endl;
        sprintf(sqlBuf,"delete from t_UserRecv where userId = %d;",userid);
        if(!m_sql->UpdataMysql(sqlBuf)){
            cout<<"UpdataMysql error:"<<sqlBuf<<endl;
            return;
        }
    }

    // ---- 取上一批推送记录，拼排除串 ----
    string lastPushedIds = "0"; // 占位，保证 not in (...) 语法合法
    {
        std::lock_guard<std::mutex> lk(m_recMutex);
        auto it = m_mapLastPushed.find(userid);
        if (it != m_mapLastPushed.end())
            for (int vid : it->second)
                lastPushedIds += "," + to_string(vid);
    }

    const int TOTAL_SLOTS = 10;
    int slotIndex = 0;
    string excludeIds = "0"; // 本次已选中的，防止同一批内重复
    std::set<int> pickedIds;
    string notWatched = "and v.videoId not in (select videoId from t_UserRecv where userId = "
                        + to_string(userid) + ") ";

    // ---- ① 前7坑位：没看过 + 不在上一批，按标签匹配度+热度 ----
    sprintf(sqlBuf,
        "select v.videoId, v.picName, v.picPath, v.rtmp "
        "from t_VideoInfo v, t_UserData u "
        "where u.id = %d and v.%s %s"
        "and v.videoId not in (%s) "
        "order by "
        "((u.food=1 and v.food=1) + (u.funny=1 and v.funny=1) + (u.ennegy=1 and v.ennegy=1) + "
        "(u.dance=1 and v.dance=1) + (u.video=1 and v.video=1) + (u.music=1 and v.music=1) + "
        "(u.outside=1 and v.outside=1) + (u.edu=1 and v.edu=1)) desc, "
        "v.hotdegree desc "
        "limit 0,7;",
        userid, dirtyFilter, notWatched.c_str(), lastPushedIds.c_str());
    FetchVideos(sqlBuf, fileList, slotIndex, excludeIds, pickedIds);

    // ---- ① 后3坑位：没看过 + 不在上一批，纯热度（防信息茧房）----
    if((int)fileList.size() < TOTAL_SLOTS)
    {
        sprintf(sqlBuf,
            "select v.videoId, v.picName, v.picPath, v.rtmp "
            "from t_VideoInfo v "
            "where v.%s %s"
            "and v.videoId not in (%s) and v.videoId not in (%s) "
            "order by v.hotdegree desc "
            "limit 0,%d;",
            dirtyFilter, notWatched.c_str(), lastPushedIds.c_str(), excludeIds.c_str(),
            TOTAL_SLOTS - (int)fileList.size());
        FetchVideos(sqlBuf, fileList, slotIndex, excludeIds, pickedIds);
    }

    // ---- ② 补位：没看过但上一批推过的（放开 lastPushed 限制）----
    if((int)fileList.size() < TOTAL_SLOTS)
    {
        sprintf(sqlBuf,
            "select v.videoId, v.picName, v.picPath, v.rtmp "
            "from t_VideoInfo v "
            "where v.%s %s"
            "and v.videoId not in (%s) "
            "order by v.hotdegree desc "
            "limit 0,%d;",
            dirtyFilter, notWatched.c_str(), excludeIds.c_str(),
            TOTAL_SLOTS - (int)fileList.size());
        FetchVideos(sqlBuf, fileList, slotIndex, excludeIds, pickedIds);
    }

    // ---- ③ 补位：看过的，最早看过的优先（recvTime 升序，NULL 视为最旧排最前）----
    // 没看过的视频不足10条时不留空白坑位，用"最早看过"的老视频补满
    if((int)fileList.size() < TOTAL_SLOTS)
    {
        sprintf(sqlBuf,
            "select v.videoId, v.picName, v.picPath, v.rtmp "
            "from t_VideoInfo v join t_UserRecv r on v.videoId = r.videoId and r.userId = %d "
            "where v.%s "
            "and v.videoId not in (%s) "
            "order by r.recvTime asc "
            "limit 0,%d;",
            userid, dirtyFilter, excludeIds.c_str(),
            TOTAL_SLOTS - (int)fileList.size());
        FetchVideos(sqlBuf, fileList, slotIndex, excludeIds, pickedIds);
    }

    // ---- 记录本批推送，供下次刷新"换一批" ----
    {
        std::lock_guard<std::mutex> lk(m_recMutex);
        m_mapLastPushed[userid] = pickedIds;
    }
    cout << "[GetFileList] Done, fileList size: " << fileList.size() << endl;
}

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

// ===== 播放时长/完播率上报（纯上报，不回复） =====
void CLogic::PlayReportRq(sock_fd clientfd, char* szbuf, int nlen)
{
    STRU_PLAY_REPORT_RQ* rq = (STRU_PLAY_REPORT_RQ*)szbuf;
    cout << "[PlayReportRq] userId=" << rq->m_nUserId << " videoId=" << rq->m_nVideoId
         << " watchSeconds=" << rq->m_nWatchSeconds << " totalSeconds=" << rq->m_nTotalSeconds << endl;

    char sqlBuf[_DEF_SQLIEN] = "";
    // playTime 显式写 NOW()，不依赖 t_VideoPlayLog 表定义里的默认值（避免因为建表时没设默认值导致这一列是 NULL）
    sprintf(sqlBuf,
        "INSERT INTO t_VideoPlayLog (videoId, userId, watchSeconds, totalSeconds, playTime) "
        "VALUES (%d, %d, %d, %d, NOW());",
        rq->m_nVideoId, rq->m_nUserId, rq->m_nWatchSeconds, rq->m_nTotalSeconds);
    m_sql->UpdataMysql(sqlBuf);

    // 以 t_VideoPlayLog 为唯一数据源：playCount 与 hotdegree 在同一条 SQL 内一起从日志派生，
    // 不再用独立的 "SET playCount = playCount + 1"，从根上消除两套计数器漂移。
    // - playCountAll  : 该视频历史总播放次数（= 日志行数），对应列表展示的"播放量"
    // - playCount30d  : 近30天有效播放次数（totalSeconds>0 才算）
    // - avgCompletion30d: 近30天平均完播率
    // hotdegree 公式（中枢惩罚式）：playCount30d * 25 * (avgCompletion30d - 0.3)
    //   - 完播率 > 0.3 才加分（权重25/次），越接近看完加得越多；
    //   - 完播率 <= 0.3（看一眼就关）整体算负，被 GREATEST(0,...) 托底为 0，实现"不感兴趣则热度不涨/清零"；
    //   - 阈值 0.3 与权重 25 可调，热度恒非负。
    char hotSqlBuf[800] = "";
    sprintf(hotSqlBuf,
        "UPDATE t_VideoInfo v LEFT JOIN ("
        "SELECT videoId, "
        "COUNT(*) AS playCountAll, "
        "COUNT(CASE WHEN playTime > NOW() - INTERVAL 30 DAY AND totalSeconds>0 THEN 1 END) AS playCount30d, "
        "AVG(CASE WHEN playTime > NOW() - INTERVAL 30 DAY AND totalSeconds>0 THEN watchSeconds/totalSeconds END) AS avgCompletion30d "
        "FROM t_VideoPlayLog WHERE videoId=%d "
        "GROUP BY videoId) p ON p.videoId=v.videoId "
        "SET v.playCount = IFNULL(p.playCountAll, 0), "
        "v.hotdegree = GREATEST(0, ROUND(IFNULL(p.playCount30d,0) * 25 * (IFNULL(p.avgCompletion30d,0) - 0.3))) "
        "WHERE v.videoId=%d;",
        rq->m_nVideoId, rq->m_nVideoId);
    bool hotOk = m_sql->UpdataMysql(hotSqlBuf);
    cout << "[PlayReportRq] playCount+hotdegree recompute ok=" << hotOk << endl;
}

// ===== 视频被点击播放（纯上报，不回复） =====
// 客户端双击卡片、准备开始播放的瞬间就发这个包，是同步的UI事件触发，不依赖播放时长/是否播完，
// 比 PlayReportRq 更容易保证真的会被发出去
void CLogic::VideoClickRq(sock_fd clientfd, char* szbuf, int nlen)
{
    STRU_VIDEO_CLICK_RQ* rq = (STRU_VIDEO_CLICK_RQ*)szbuf;
    cout << "[VideoClickRq] userId=" << rq->m_nUserId << " videoId=" << rq->m_nVideoId << endl;
    if (rq->m_nUserId <= 0 || rq->m_nVideoId <= 0) return;

    // t_UserRecv 语义改为【看过】：点击播放才写表。
    // 先删后插以刷新 recvTime（LRU）：重看一个视频会把它挪到"最近看过"，
    // 补位推荐时按 recvTime 升序取"最早看过的"优先推
    char sqlBuf[_DEF_SQLIEN] = "";
    sprintf(sqlBuf, "DELETE FROM t_UserRecv WHERE userId = %d AND videoId = %d;",
            rq->m_nUserId, rq->m_nVideoId);
    m_sql->UpdataMysql(sqlBuf);
    sprintf(sqlBuf, "INSERT INTO t_UserRecv(userId, videoId, recvTime) VALUES(%d, %d, NOW());",
            rq->m_nUserId, rq->m_nVideoId);
    if (!m_sql->UpdataMysql(sqlBuf))
        cout << "UpdataMysql error:" << sqlBuf << endl;

    // 热度仍只由 PlayReportRq（真实播放+完播率）决定，此处不写 hotdegree
}
