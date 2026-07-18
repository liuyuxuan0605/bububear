#include "onlinedialog.h"
#include "ui_onlinedialog.h"
#include<QCryptographicHash>
#include<QMessageBox>
#include<QFileInfo>
#include<QDir>
#include<QThread>
#include"movielable.h"

#define MD5_KEY 12345

static QByteArray GetMD5(QString val){
    QCryptographicHash hash(QCryptographicHash::Md5);
    QString tmp=QString("%1_%2").arg(val).arg(MD5_KEY);
    hash.addData(tmp.toStdString().c_str());
    QByteArray bt=hash.result();
    return bt.toHex();
}

// 断点续传 v2：计算文件内容的原始 MD5（32 位小写十六进制，不带盐），
// 用作文件稳定指纹（握手身份 + 服务端终校）。必须算【整文件】，与续传偏移无关。
static QString ComputeFileContentMd5(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Md5);
    const qint64 chunk = 64 * 1024;
    char buf[64 * 1024];
    while (!f.atEnd()) {
        qint64 r = f.read(buf, chunk);
        if (r > 0) hash.addData(buf, r);
    }
    f.close();
    return QString(hash.result().toHex()); // toHex() 默认小写
}

OnlineDialog* OnlineDialog::m_online=NULL;

OnlineDialog::OnlineDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::OnlineDialog),m_id(0)
{
    ui->setupUi(this);
    m_online=this;
    qsrand(QTime(0,0,0).msecsTo(QTime::currentTime()));
    //注册信号中使用的结构
    qRegisterMetaType<Hobby>("Hobby");

    m_login=new LoginDialog();
    m_login->hide();

    connect(ui->pb_play1,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play2,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play3,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play4,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play5,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play6,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play7,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play8,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play9,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play10,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play11,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play12,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play13,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play14,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));
    connect(ui->pb_play15,SIGNAL(SIG_labelClicked()),this,SLOT(slot_PlayClicked()));

    connect(ui->pb_myplay1,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay2,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay3,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay4,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay5,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay6,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay7,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay8,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay9,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay10,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay11,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay12,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay13,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay14,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));
    connect(ui->pb_myplay15,SIGNAL(SIG_labelClicked()),this,SLOT(slot_MyPlayClicked()));

    connect(m_login,SIGNAL(SIG_loginCommit(QString,QString)),this,SLOT(slot_loginCommit(QString,QString)));
    connect(m_login,SIGNAL(SIG_registerCommit(QString,QString,Hobby)),this,SLOT(slot_registerCommit(QString,QString,Hobby)));

    m_tcp=new TcpClientMediator;
    connect(m_tcp,SIGNAL(SIG_ReadyData(unsigned int, char*, int)),this,SLOT(slot_ReadyData(unsigned int, char*, int)));
    // v2：worker 线程通过信号把提示转交 GUI 线程弹出（QMessageBox 只能在 GUI 线程创建）
    connect(this,SIGNAL(SIG_UploadMessage(QString,QString)),this,SLOT(slot_ShowUploadMessage(QString,QString)));

    bool ok = m_tcp->OpenNet(DEF_SSERVER_IP, DEF_SSERVER_PORT);
    if(!ok){
        QMessageBox::about(this,"提示","连接服务器失败");
    }

    m_uploadDialog=new UploadDialog;
   // connect(m_uploadDialog,SIGNAL(SIG_UploadFile(QString,QString,Hobby)),this,SLOT(slot_UploadFile(QString,QString,Hobby)));
    connect(this,SIGNAL(SIG_updateProcess(qint64,qint64)),m_uploadDialog,SLOT(slot_updateProcess(qint64,qint64)));
    m_uploadDialog->hide();

    m_uploadWorker=new UploadWork;
    m_uploadthread=new QThread;
    connect(m_uploadDialog,SIGNAL(SIG_UploadFile(QString,QString,Hobby)),m_uploadWorker,SLOT(slot_UploadFile(QString,QString,Hobby)));
    m_uploadWorker->moveToThread(m_uploadthread);
    m_uploadthread->start();
}

OnlineDialog::~OnlineDialog()
{
    if(m_login){
        delete m_login;
        m_login=NULL;
    }
    if(m_tcp){
        delete m_tcp;
        m_tcp=NULL;
    }
    if(m_uploadWorker){
       delete m_uploadWorker;
        m_uploadWorker=NULL;
    }
    if(m_uploadthread){
        m_uploadthread->quit();
        m_uploadthread->wait(100);
        if(m_uploadthread->isRunning()){
            m_uploadthread->terminate();
            m_uploadthread->wait(100);
        }
        delete m_uploadthread;
        m_uploadthread=NULL;
    }
    delete ui;
}
//登录模块
void OnlineDialog::on_pb_login_clicked()
{
    m_login->show();
}

//登录提交
void OnlineDialog::slot_loginCommit(QString name, QString password)
{
    m_user=name;

    std::string strname=name.toStdString();
    char* bufName=(char*)strname.c_str();

    QByteArray bt=GetMD5(password);
    STRU_LOGIN_RQ rq;
    strcpy_s(rq.user,_MAX_SIZE,bufName);
    memcpy(rq.password,bt.data(),bt.length());

    if(m_tcp->SendData(0,(char*)&rq,sizeof(rq))<0){
        QMessageBox::about(this,"提示","网络故障");
    }
}

//注册提交
void OnlineDialog::slot_registerCommit(QString name, QString password, Hobby hy)
{
    std::string strname=name.toStdString();
    char* bufName=(char*)strname.c_str();

    QByteArray bt=GetMD5(password);
    STRU_REGISTER_RQ rq;
    strcpy_s(rq.user,_MAX_SIZE,bufName);
    memcpy(rq.password,bt.data(),bt.length());
    rq.dance  =hy.dance  ;
    rq.edu    =hy.edu    ;
    rq.ennegy =hy.ennegy ;
    rq.food   =hy.food   ;
    rq.funny  =hy.funny  ;
    rq.music  =hy.music  ;
    rq.outside=hy.outside;
    rq.video  =hy.video  ;
    if(m_tcp->SendData(0,(char*)&rq,sizeof(rq))<0){
        QMessageBox::about(this,"提示","网络故障");
    }
}
//Tcp网络接收
void OnlineDialog::slot_ReadyData(unsigned int lSendIP, char *buf, int nlen)
{
    int nType=*(int*)buf;
    switch(nType)
    {
        case _DEF_PACK_LOGIN_RS:
            slot_loginRs(lSendIP,buf,nlen);
        break;
        case _DEF_PACK_REGISTER_RS:
            slot_registerRs(lSendIP,buf,nlen);
        break;
        case _DEF_PACK_UPLOAD_RS:
            slot_uploadRs(lSendIP,buf,nlen);
        break;
        case DEF_PACK_DOWNLOAD_RS:
            slot_downloadRs(lSendIP,buf,nlen);
        break;
        case _DEF_PACK_FILEBLOCK_RQ:
            slot_fileblockrq(lSendIP,buf,nlen);
        break;
        case _DEF_PACK_UPLOADHISTORY_RS:
            slot_uploadHistoryRs(lSendIP,buf,nlen);
        break;
        // v2 断点续传：握手/终验回复，在 GUI 线程拷贝后唤醒阻塞的 worker 上传流程
        case _DEF_PACK_UPLOAD_V2_RS:
        {
            QMutexLocker lock(&m_v2Mutex);
            if (m_v2HsPending) {
                m_v2HsRs = *(STRU_UPLOAD_V2_RS*)buf;
                m_v2HsPending = false;
                m_v2HsCond.wakeOne();
            }
        }
        break;
        case _DEF_PACK_UPLOAD_END_RS:
        {
            QMutexLocker lock(&m_v2Mutex);
            if (m_v2EndPending) {
                m_v2EndRs = *(STRU_UPLOAD_END_RS*)buf;
                m_v2EndPending = false;
                m_v2EndCond.wakeOne();
            }
        }
        break;
        case _DEF_PACK_LIVE_START_RS:
        case _DEF_PACK_LIVE_STOP_RS:
        case _DEF_PACK_LIVE_LIST_RS:
        case _DEF_PACK_LIVE_LIST_END:
            emit SIG_LivePacket(buf, nlen);   // LiveDialog 只读 buf，不 delete
        break;
    }

    delete[] buf;
}
//用户登录回复
void OnlineDialog::slot_loginRs(unsigned int lSendIP, char *buf, int nlen)
{
    STRU_LOGIN_RS*rs=(STRU_LOGIN_RS*)buf;
    switch(rs->result){
        case user_not_exist:
            QMessageBox::about(m_login,"提示","用户不存在，登录失败");
        break;
        case password_error:
            QMessageBox::about(m_login,"提示","密码错误，登录失败");
        break;
        case login_success:
            QMessageBox::about(m_login,"提示","登录成功");
            ui->lb_name->setText(QString("您好,%1").arg(m_user));
            m_login->hide();
            m_id=rs->userid;
            emit SIG_LoginSuccess(m_id, m_user);

            //下载列表文件
            STRU_DOWNLOAD_RQ rq;
            rq.m_nUserId=m_id;
            m_tcp->SendData(0,(char*)&rq,sizeof(rq));

        break;
    }
}
//用户注册回复
void OnlineDialog::slot_registerRs(unsigned int lSendIP, char *buf, int nlen)
{
    STRU_REGISTER_RS*rs=(STRU_REGISTER_RS*)buf;
    switch(rs->result){
        case user_is_exist:
            QMessageBox::about(m_login,"提示","用户已存在，注册失败");
        break;
        case register_success:
            QMessageBox::about(m_login,"提示","注册成功");
        break;

    }
}
//上传的回复
void OnlineDialog::slot_uploadRs(unsigned int lSendIP, char *buf, int nlen)
{
    STRU_UPLOAD_RS *rs=(STRU_UPLOAD_RS*)buf;
    switch(rs->m_nResult)
    {
       case 1:
           QMessageBox::about(m_login,"提示","上传成功");
       break;
    }
}
//下载回复
void OnlineDialog::slot_downloadRs(unsigned int lSendIP, char *buf, int nlen)
{
    STRU_DOWNLOAD_RS *rs=(STRU_DOWNLOAD_RS*)buf;

    // 防御脏数据：文件名为空会拼出 "./temp/"（目录路径），open 必失败；
    // fileSize<=0 的记录也没有后续文件块，直接跳过
    if (rs->m_szFileName[0] == '\0' || rs->m_nFileSize <= 0)
    {
        qDebug() << "跳过非法下载头: fileName为空或fileSize<=0, videoId=" << rs->m_nVideoId;
        return;
    }

    //文件头 给FileInfo赋值
    FileInfo *info =new FileInfo;
    info->videoId=rs->m_nVideoId;
    info->fileId=rs->m_nFileId;
    info->fileName=rs->m_szFileName;
    info->fromHistory=false; // 推荐影视页的下载头，GIF 落到 pb_playN

    QDir dir;
    if(!dir.exists(QDir::currentPath()+"/temp/"))
    {
        dir.mkpath(QDir::currentPath()+"/temp/");
    }
    info->filePath=QString("./temp/%1").arg(rs->m_szFileName);
    qDebug() << "准备写入文件:" << info->filePath;

    info->filePos=0;
    info->fileSize=rs->m_nFileSize;
    info->rtmpUrl=QString("rtmp://%1/vod%2").arg(DEF_SSERVER_IP).arg(rs->m_rtmp);
    qDebug()<<"rtmp--"<<info->rtmpUrl;
    info->pFile=new QFile(info->filePath);

    if(info->pFile->open(QIODevice::WriteOnly))
    {
        // 同 videoId 的旧传输还在途（推荐/历史两条路径都可能下同一 GIF）：
        // 直接覆盖会泄漏旧 info 且旧 QFile 不关闭，先清掉旧节点
        auto old = m_mapVideoIDToFileInfo.find(info->videoId);
        if(old != m_mapVideoIDToFileInfo.end())
        {
            old.value()->pFile->close();
            delete old.value()->pFile;
            delete old.value();
            m_mapVideoIDToFileInfo.erase(old);
        }
        m_mapVideoIDToFileInfo[info->videoId]=info;
    }else{
        qDebug() << "文件打开失败:" << info->pFile->errorString();
        delete info->pFile;
        delete info;
    }
}
//下载文件块
void OnlineDialog::slot_fileblockrq(unsigned int lSendIP, char *buf, int nlen)
{
    STRU_FILEBLOCK_RQ *rq=(STRU_FILEBLOCK_RQ*)buf;

    // 服务端现在按"定长头+实际数据"变长发送（末块不再是整个64KB结构体），
    // 校验长度自洽：m_nBlockLen 合法且 nlen 装得下，防止错位/畸形包越界读
    const int nHeadLen = sizeof(STRU_FILEBLOCK_RQ) - _DEF_CONTENT_SIZE;
    if (rq->m_nBlockLen <= 0 || rq->m_nBlockLen > _DEF_CONTENT_SIZE
            || nlen < nHeadLen + rq->m_nBlockLen)
    {
        qDebug() << "非法文件块: nlen=" << nlen << "blockLen=" << rq->m_nBlockLen;
        return;
    }

    auto ite=m_mapVideoIDToFileInfo.find(rq->m_nFileId);
    if(ite==m_mapVideoIDToFileInfo.end())
    {
        qDebug() << "找不到对应的文件信息，key:" << rq->m_nFileId;
        return;
    }
    FileInfo* info=m_mapVideoIDToFileInfo[rq->m_nFileId];

    int64_t res=info->pFile->write(rq->m_szFileContent,rq->m_nBlockLen);
    if(res < 0)
        {
            qDebug() << "写入文件失败:" << info->pFile->errorString();
            return;
        }
    info->filePos+=res;
    if(info->filePos>=info->fileSize)
    {
        //关闭文件 回收QFile对象（此前只close不delete，每张GIF泄漏一个QFile）
        info->pFile->close();
        delete info->pFile;
        info->pFile=NULL;
        //删除节点
        m_mapVideoIDToFileInfo.erase(ite);
        //设置到控件
        // 关键：按下载头记录的 fromHistory 决定落到哪个页面的控件，
        // 不能看 sw_page 当前页——GIF 传输是异步的，用户中途切页会把
        // 推荐的图安到上传历史的卡片上（反之亦然），rtmp/videoId 全串
        QString pbNum;
        if(!info->fromHistory) {
            // 推荐影视页
            pbNum = QString("pb_play%1").arg(info->fileId + 1);
        } else {
            // 上传历史页
            pbNum = QString("pb_myplay%1").arg(info->fileId + 1);
        }
         movielable* pb_play=ui->sw_page->findChild<movielable *>(pbNum);
         if (!pb_play) {
             qDebug() << "找不到控件：" << pbNum;
             delete info;
             return;
         }
        QMovie *LastMovie=pb_play->movie();
        if(LastMovie &&LastMovie->isValid())
        {
            delete LastMovie;
        }
         QMovie *movie=new QMovie(info->filePath);
         pb_play->setMovie(movie);
         pb_play->setRtmpUrl(info->rtmpUrl);
         pb_play->setVideoId(info->videoId);
        //回收info
         delete info;
         info=NULL;
    }
}
//上传历史
void OnlineDialog::slot_uploadHistoryRs(unsigned int lSendIP, char *buf, int nlen)
{
    STRU_DOWNLOAD_RS *rs = (STRU_DOWNLOAD_RS*)buf;

    // 防御脏数据（同 slot_downloadRs）：空文件名会拼出目录路径，open 必失败
    if (rs->m_szFileName[0] == '\0' || rs->m_nFileSize <= 0)
    {
        qDebug() << "跳过非法历史头: fileName为空或fileSize<=0, videoId=" << rs->m_nVideoId;
        return;
    }

    FileInfo *info = new FileInfo;
    info->videoId = rs->m_nVideoId;
    info->fileId = rs->m_nFileId;
    info->fileName = rs->m_szFileName;
    info->fromHistory = true; // 上传历史页的下载头，GIF 落到 pb_myplayN

    QDir dir;
    if(!dir.exists(QDir::currentPath() + "/temp/"))
        dir.mkpath(QDir::currentPath() + "/temp/");

    info->filePath = QString("./temp/%1").arg(rs->m_szFileName);
    info->filePos = 0;
    info->fileSize = rs->m_nFileSize;
    info->rtmpUrl = QString("rtmp://%1/vod%2").arg(DEF_SSERVER_IP).arg(rs->m_rtmp);
    info->pFile = new QFile(info->filePath);

    if(info->pFile->open(QIODevice::WriteOnly)) {
        // 同 videoId 旧传输在途则先清掉（同 slot_downloadRs，防泄漏/文件句柄悬挂）
        auto old = m_mapVideoIDToFileInfo.find(info->videoId);
        if(old != m_mapVideoIDToFileInfo.end())
        {
            old.value()->pFile->close();
            delete old.value()->pFile;
            delete old.value();
            m_mapVideoIDToFileInfo.erase(old);
        }
        m_mapVideoIDToFileInfo[info->videoId] = info;
    } else {
        qDebug() << "历史缩略图打开失败:" << info->pFile->errorString();
        delete info->pFile;
        delete info;
    }
}
//上传文件响应
void OnlineDialog::slot_UploadFile(QString filePath, QString imgPath, Hobby hy)
{
    //上传
    qDebug()<<"开始上传";

    UploadFile(imgPath,hy);
    UploadFile(filePath,hy,imgPath);
}
//上传文件（断点续传 v2）
// 流程：算整文件 MD5 指纹 → 发握手(得 taskId / 续传偏移) → seek 续传点 →
//       变长 64KB 块流式发送 → 发结束包触发服务端整文件 MD5 终验。
// 本函数在 worker 线程执行；握手/终验回复由 GUI 线程 slot_ReadyData 唤醒。
void OnlineDialog::UploadFile(QString filePath, Hobby hy,QString gifName)
{
    QFileInfo info(filePath);
    QString FileName = info.fileName();
    int64_t fileSize = info.size();
    if (fileSize <= 0) {
        emit SIG_UploadMessage("提示","文件为空或无法读取");
        return;
    }
    if (m_id <= 0) {
        emit SIG_UploadMessage("提示","请先登录");
        return;
    }

    m_uploadCanceled = false;

    // 1) 全文件 MD5 指纹（整文件，与续传偏移无关）
    QString md5 = ComputeFileContentMd5(filePath);
    if (md5.length() != 32) {
        emit SIG_UploadMessage("提示","计算文件指纹失败");
        return;
    }

    // 2) 握手请求
    STRU_UPLOAD_V2_RQ rq;
    rq.m_UserId = m_id;
    rq.m_nFileSize = fileSize;
    strncpy(rq.m_szMd5, md5.toUtf8().constData(), 32);
    rq.m_szMd5[32] = 0;
    strncpy(rq.m_szFileName, FileName.toUtf8().constData(), _MAX_PATH - 1);
    rq.m_szFileName[_MAX_PATH - 1] = 0;
    if (!gifName.isEmpty()) {
        QFileInfo gi(gifName);
        strncpy(rq.m_szGifName, gi.fileName().toUtf8().constData(), _MAX_PATH - 1);
        rq.m_szGifName[_MAX_PATH - 1] = 0;
    }
    int dot = filePath.lastIndexOf('.');
    QString ext = (dot >= 0) ? filePath.mid(dot + 1).toLower() : QString();
    strncpy(rq.m_szFileType, ext.toUtf8().constData(), _MAX_SIZE - 1);
    rq.m_szFileType[_MAX_SIZE - 1] = 0;
    memcpy(rq.m_szHobby, &hy, sizeof(hy));

    // 握手：在锁内置位 pending 并发送，杜绝「RS 早于 wait」的丢唤醒竞态
    STRU_UPLOAD_V2_RS hs;
    {
        QMutexLocker lock(&m_v2Mutex);
        m_v2HsPending = true;
        m_tcp->SendData(0, (char*)&rq, sizeof(rq));
        if (!m_v2HsCond.wait(&m_v2Mutex, 15000)) {
            m_v2HsPending = false;
            emit SIG_UploadMessage("提示","上传握手超时，请检查网络");
            return;
        }
        hs = m_v2HsRs;
    }

    if (hs.m_nResult == upload_v2_fail) {
        emit SIG_UploadMessage("提示","服务器拒绝上传");
        return;
    }
    if (hs.m_nResult == upload_v2_busy) {
        emit SIG_UploadMessage("提示","该文件正在上传中（占用未释放）");
        return;
    }
    if (hs.m_nResult == upload_v2_instant) { // 秒传
        emit SIG_updateProcess(fileSize, fileSize);
        emit SIG_UploadMessage("提示","秒传成功");
        return;
    }

    int taskId = hs.m_nTaskId;
    int64_t offset = hs.m_nResumeFrom;

    // 3) 读源文件，从续传点继续发送（变长 64KB 块）
    QFile src(filePath);
    if (!src.open(QIODevice::ReadOnly)) {
        emit SIG_UploadMessage("提示","无法打开源文件");
        return;
    }
    if (!src.seek(offset)) {
        src.close();
        emit SIG_UploadMessage("提示","定位文件失败");
        return;
    }
    emit SIG_updateProcess(offset, fileSize); // 续传时进度条从断点起

    const int CHUNK = _DEF_CONTENT_SIZE; // 64KB
    QByteArray payload(CHUNK, 0);
    QByteArray wire(sizeof(STRU_UPLOAD_BLOCK) + CHUNK, 0);
    STRU_UPLOAD_BLOCK* blk = reinterpret_cast<STRU_UPLOAD_BLOCK*>(wire.data());

    bool netOk = true;
    while (offset < fileSize) {
        if (m_uploadCanceled) break; // 原子中断：保留 .part，下次续传
        qint64 r = src.read(payload.data(), CHUNK);
        if (r <= 0) break;
        blk->m_nType   = _DEF_PACK_UPLOAD_BLOCK;
        blk->m_nTaskId = taskId;
        blk->m_nOffset = offset;
        blk->m_nLen    = (int)r;
        memcpy(wire.data() + sizeof(STRU_UPLOAD_BLOCK), payload.data(), r);
        if (m_tcp->SendData(0, wire.data(), sizeof(STRU_UPLOAD_BLOCK) + (int)r) <= 0) {
            netOk = false; // 网络中断：停止发送，不发 END，服务端保留 .part 供续传
            break;
        }
        offset += r;
        emit SIG_updateProcess(offset, fileSize);
    }
    src.close();

    if (m_uploadCanceled) {
        emit SIG_UploadMessage("提示","已取消上传，进度已保留，可稍后继续");
        return;
    }
    if (!netOk) {
        emit SIG_UploadMessage("提示","网络中断，上传已暂停，可稍后继续");
        return;
    }

    // 4) 结束请求（触发服务端整文件 MD5 终验），同样在锁内发送避免丢唤醒
    STRU_UPLOAD_END_RQ endrq;
    endrq.m_nTaskId = taskId;
    strncpy(endrq.m_szMd5, md5.toUtf8().constData(), 32);
    endrq.m_szMd5[32] = 0;

    STRU_UPLOAD_END_RS endrs;
    {
        QMutexLocker lock(&m_v2Mutex);
        m_v2EndPending = true;
        m_tcp->SendData(0, (char*)&endrq, sizeof(endrq));
        if (!m_v2EndCond.wait(&m_v2Mutex, 30000)) {
            m_v2EndPending = false;
            emit SIG_UploadMessage("提示","上传终验超时，请检查网络");
            return;
        }
        endrs = m_v2EndRs;
    }

    if (endrs.m_nResult == 1) {
        emit SIG_updateProcess(fileSize, fileSize);
        emit SIG_UploadMessage("提示","上传成功");
    } else {
        emit SIG_UploadMessage("提示","文件校验失败，请重新上传");
    }
}

// v2：在 GUI 线程安全弹出上传提示（worker 线程通过 SIG_UploadMessage 调用）
void OnlineDialog::slot_ShowUploadMessage(QString title, QString text)
{
    QMessageBox::about(this, title, text);
}

// v2：原子中断当前上传。中断后【不】发结束包，服务端保留 .part/.meta，
// 下次上传同一文件（同 md5）可从已收字节续传。幂等：重复调用无害。
void OnlineDialog::CancelUpload()
{
    m_uploadCanceled = true;
}

//点击上传视频
void OnlineDialog::on_pb_upload_clicked()
{
   if(m_id==0){
       QMessageBox::about(this,"提示","先登录");
       return;
   }
    m_uploadDialog->clear();
    m_uploadDialog->show();
}

//点击卡片准备播放的瞬间，先上报一次"视频被点击"，服务端收到就给这条视频热度+5
void OnlineDialog::reportVideoClick(int videoId)
{
    if(m_id<=0 || videoId<=0) return; //没登录/没有效videoId就不上报
    STRU_VIDEO_CLICK_RQ rq;
    rq.m_nType=_DEF_PACK_VIDEO_CLICK_RQ; //构造函数只是memset清零，必须手动设置包类型，否则服务端会直接丢包
    rq.m_nUserId=m_id;
    rq.m_nVideoId=videoId;
    m_tcp->SendData(0,(char*)&rq,sizeof(rq));
}

void OnlineDialog::slot_PlayClicked()
{
    movielable *pb_play=(movielable*)QObject::sender();
    // 空坑位（刷新后被 clear 的卡片）没有有效视频，点了不响应
    if(pb_play->videoId() <= 0 || pb_play->rtmpUrl().isEmpty())
        return;
    reportVideoClick(pb_play->videoId());
    Q_EMIT SIG_PlayVideoWithId(pb_play->rtmpUrl(), pb_play->videoId());
}

void OnlineDialog::slot_MyPlayClicked()
{
    movielable *pb_play = (movielable*)QObject::sender();
    if(pb_play->videoId() <= 0 || pb_play->rtmpUrl().isEmpty())
        return;
    reportVideoClick(pb_play->videoId());
    Q_EMIT SIG_PlayVideoWithId(pb_play->rtmpUrl(), pb_play->videoId());
}

//工作者上传流程
void UploadWork::slot_UploadFile(QString filePath, QString imgPath, Hobby hy)
{
    OnlineDialog::m_online->slot_UploadFile(filePath,imgPath,hy);
}

void OnlineDialog::on_pb_fresh_clicked()
{
    if(!m_id)
    {
        QMessageBox::about(this,"提示","先登录");
        return;
    }
    // 先清空上一批推荐卡片：看过的视频服务端不会再推，
    // 若新一批不足10个，不清空会导致旧卡片残留、看起来"还在列表里"
    for(int i = 1; i <= 15; ++i)
    {
        movielable* pb = ui->sw_page->findChild<movielable*>(QString("pb_play%1").arg(i));
        if(pb) pb->clear();
    }
    STRU_DOWNLOAD_RQ rq;
    rq.m_nUserId=m_id;
    m_tcp->SendData(0,(char*)&rq,sizeof(rq));
}


void OnlineDialog::on_pb_video_clicked()
{
    ui->sw_page->setCurrentIndex(0);  // 切回推荐影视页
}


void OnlineDialog::on_pb_uploadHistory_clicked()
{
    if(m_id == 0) {
        QMessageBox::about(this, "提示", "先登录");
        return;
    }
    ui->sw_page->setCurrentIndex(1);  // 切到 page_2（上传历史页）

    // 先清空旧卡片（同推荐页刷新逻辑）：历史条数变少时旧卡片会残留
    for(int i = 1; i <= 15; ++i)
    {
        movielable* pb = ui->sw_page->findChild<movielable*>(QString("pb_myplay%1").arg(i));
        if(pb) pb->clear();
    }

    // 发送上传历史请求
    STRU_UPLOADHISTORY_RQ rq;
    rq.m_nUserId = m_id;
    m_tcp->SendData(0, (char*)&rq, sizeof(rq));
}

