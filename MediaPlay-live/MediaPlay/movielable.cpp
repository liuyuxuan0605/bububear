#include "movielable.h"
#include "ui_movielable.h"
#include<QDebug>

movielable::movielable(QWidget *parent) :
    QWidget(parent),m_movie(NULL),
    ui(new Ui::movielable),
    m_videoId(-1)
{
    ui->setupUi(this);

    //安装事件监听器，让lb_movie识别事件
    ui->lb_movie->installEventFilter(this);
}

movielable::~movielable()
{
    delete ui;
}

QString movielable::rtmpUrl() const
{
    return m_rtmpUrl;
}

QMovie *movielable::movie() const
{
    return m_movie;
}
//设置动画
void movielable::setMovie(QMovie *movie)
{
    m_movie=movie;
    ui->lb_movie->setMovie(movie);
    movie->start();
    movie->stop();
}
//鼠标移入
void movielable::enterEvent(QEvent *event)
{
    if(m_movie){
        m_movie->start();
    }
}
//鼠标移出
void movielable::leaveEvent(QEvent *event)
{
    if(m_movie){
        m_movie->stop();
    }
}
//设置控件播放url
void movielable::setRtmpUrl(QString url)
{
    m_rtmpUrl=url;
}
int movielable::videoId() const
{
    return m_videoId;
}
void movielable::setVideoId(int id)
{
    m_videoId=id;
}
//清空卡片（刷新推荐列表时调用）：旧一批的动画/URL/videoId 全部重置，
//避免新一批不足10个时看过的视频残留显示
void movielable::clear()
{
    if(m_movie){
        m_movie->stop();
        delete m_movie;
        m_movie=NULL;
    }
    ui->lb_movie->clear();
    m_rtmpUrl.clear();
    m_videoId=-1;
}
//事件过滤处理
bool movielable::eventFilter(QObject *watched, QEvent *event)
{
    //动画点击
    if(watched==ui->lb_movie&&event->type()==QEvent::MouseButtonPress)
    {
        qDebug()<<"mouse Press";
        Q_EMIT SIG_labelClicked();
        return true;
    }
    // 其余事件走默认处理。此前没有这个 return，函数结尾无返回值是未定义行为，
    // 返回到的随机值若为 true 会吞掉 hover 等事件
    return QWidget::eventFilter(watched, event);
}
