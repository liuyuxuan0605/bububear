#ifndef RECORDERDIALOG_H
#define RECORDERDIALOG_H

#include <QDialog>
#include"picturewidget.h"
#include"savevideofilethread.h"

QT_BEGIN_NAMESPACE
namespace Ui { class RecorderDialog; }
QT_END_NAMESPACE

class RecorderDialog : public QDialog //界面层，触发编码开始 / 停止、配置编码参数
{
    Q_OBJECT

public:
    RecorderDialog(QWidget *parent = nullptr);
    ~RecorderDialog();


private slots:
    void on_pb_start_clicked();

    void on_pb_stop_clicked();

    void on_pb_setUrl_clicked();

    void slot_setImage(QImage img);
private:
    Ui::RecorderDialog *ui;

    PictureWidget * m_picturewidget;
    SaveVideoFileThread * m_saveFileThread;

    QString m_saveUrl;
};
#endif // RECORDERDIALOG_H
