#include<TCPKernel.h>
#include "packdef.h"
#include<stdio.h>
#include<sys/time.h>
#include"clogic.h"

using namespace std;


//设置网络协议映射
void TcpKernel::setNetPackMap()
{
    //清空映射
    bzero( m_NetPackMap , sizeof(m_NetPackMap) );

    //协议映射赋值
    m_logic->setNetPackMap();

}


TcpKernel::TcpKernel()
{

}

TcpKernel::~TcpKernel()
{
    if( m_logic ) delete m_logic;

}

TcpKernel *TcpKernel::GetInstance()
{
    static TcpKernel kernel;
    return &kernel;
}

int TcpKernel::Open( int port)
{
    initRand();

    m_sql = new CMysql;
    // 数据库 使用127.0.0.1 地址  用户名root 密码colin123 数据库 wechat 没有的话需要创建 不然报错
    if(  !m_sql->ConnectMysql( _DEF_DB_IP , _DEF_DB_USER, _DEF_DB_PWD, _DEF_DB_NAME )  )
    {
        printf("Conncet Mysql Failed...\n");
        return FALSE;
    }
    else
    {
        printf("MySql Connect Success...\n");
    }
    //初始网络
    m_tcp = new Block_Epoll_Net;
    bool res = m_tcp->InitNet( port , &TcpKernel::DealData ) ;
    if( !res )
        err_str( "net init fail:" ,-1);

    m_logic = new CLogic(this);
    m_tcp->m_pLogic = m_logic;   // 注入，供断连清理调用

    setNetPackMap();

    return TRUE;
}

void TcpKernel::Close()
{
    m_logic->close();

    m_sql->DisConnect();

}

//随机数初始化
void TcpKernel::initRand()
{
    struct timeval time;
    gettimeofday( &time , NULL);
    srand( time.tv_sec + time.tv_usec );
}

// 各包类型的最小合法长度表：分发前统一校验，防止短包/畸形包
// 导致 handler 里 (STRU_XXX*)szbuf 强转后越界读。
// 定长包：最小长度 = sizeof(结构体)；
// 变长包（UPLOAD_BLOCK）：最小长度 = 定长头，payload 部分由 handler 自查 m_nLen。
static int GetMinPackLen(PackType type)
{
    switch (type)
    {
    case _DEF_PACK_REGISTER_RQ:      return sizeof(STRU_REGISTER_RQ);
    case _DEF_PACK_LOGIN_RQ:         return sizeof(STRU_LOGIN_RQ);
    case _DEF_PACK_UPLOAD_RQ:        return sizeof(STRU_UPLOAD_RQ);
    case _DEF_PACK_FILEBLOCK_RQ:     return sizeof(STRU_FILEBLOCK_RQ);
    case DEF_PACK_DOWNLOAD_RQ:       return sizeof(STRU_DOWNLOAD_RQ);
    case _DEF_PACK_UPLOADHISTORY_RQ: return sizeof(STRU_UPLOADHISTORY_RQ);
    case _DEF_PACK_LIVE_START_RQ:    return sizeof(STRU_LIVE_START_RQ);
    case _DEF_PACK_LIVE_STOP_RQ:     return sizeof(STRU_LIVE_STOP_RQ);
    case _DEF_PACK_LIVE_LIST_RQ:     return sizeof(STRU_LIVE_LIST_RQ);
    case _DEF_PACK_PLAY_REPORT_RQ:   return sizeof(STRU_PLAY_REPORT_RQ);
    case _DEF_PACK_VIDEO_CLICK_RQ:   return sizeof(STRU_VIDEO_CLICK_RQ);
    // v2 断点续传
    case _DEF_PACK_UPLOAD_V2_RQ:     return sizeof(STRU_UPLOAD_V2_RQ);
    case _DEF_PACK_UPLOAD_BLOCK:     return sizeof(STRU_UPLOAD_BLOCK); // 变长：仅校验定长头
    case _DEF_PACK_UPLOAD_END_RQ:    return sizeof(STRU_UPLOAD_END_RQ);
    default:                         return -1; // 未注册的请求类型，拒收
    }
}

void TcpKernel::DealData(sock_fd clientfd,char *szbuf,int nlen)
{
    // 连包类型都装不下的包，直接丢弃
    if( !szbuf || nlen < (int)sizeof(PackType) )
        return;

    PackType type = *(PackType*)szbuf;
    if( (type >= _DEF_PACK_BASE) && ( type < _DEF_PACK_BASE + _DEF_PACK_COUNT) )
    {
        // 最小长度校验：短包不进 handler，防止结构体强转越界读
        int nMinLen = GetMinPackLen(type);
        if( nMinLen < 0 || nlen < nMinLen )
        {
            printf("DealData: illegal pack, type=%d nlen=%d min=%d, drop\n", type, nlen, nMinLen);
            return;
        }
        PFUN pf = NetPackMap( type );
        if( pf )
        {
            (TcpKernel::GetInstance()->m_logic->*pf)( clientfd , szbuf , nlen);
        }
    }

    return;
}

void TcpKernel::EventLoop()
{
    printf("event loop\n");
    m_tcp->EventLoop();
}

void TcpKernel::SendData(sock_fd clientfd, char *szbuf, int nlen)
{
    m_tcp->SendData(clientfd , szbuf ,nlen );
}
