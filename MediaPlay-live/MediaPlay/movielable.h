#ifndef MOVIELABLE_H
#define MOVIELABLE_H

#include <QWidget>
#include <QMovie>
#include <QEvent>

namespace Ui {
class movielable;
}

class movielable : public QWidget
{
    Q_OBJECT

public:
    explicit movielable(QWidget *parent = nullptr);
    ~movielable();

    QString rtmpUrl() const;
    QMovie *movie() const;
    int videoId() const;
    void setVideoId(int id);
    void clear(); // 清空卡片：停掉并回收动画、清显示、重置 videoId/rtmp

signals:
    void SIG_labelClicked();
public slots:
    void setMovie(QMovie *movie);
    void enterEvent(QEvent *event);
    void leaveEvent(QEvent *event);
    void setRtmpUrl(QString url);
    virtual bool eventFilter(QObject *watched,QEvent *event);
private:
    Ui::movielable *ui;
    QMovie *m_movie;
    QString m_rtmpUrl;
    int m_videoId;
};

#endif // MOVIELABLE_H
