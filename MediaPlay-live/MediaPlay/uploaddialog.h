#ifndef UPLOADDIALOG_H
#define UPLOADDIALOG_H

#include <QDialog>
#include<QImage>
#include"logindialog.h"
#include"recorderdialog.h"

namespace Ui {
class UploadDialog;
}

class UploadDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UploadDialog(QWidget *parent = nullptr);
    ~UploadDialog();

    void clear();

    QString SaveVideoJpg(QString FilePath);
signals:
    void SIG_GetOnePicture(QImage image);
    void SIG_UploadFile(QString filePath,QString imgPath,Hobby by);
public slots:
    void on_pb_view_clicked();

    void on_pb_beginupload_clicked();
    void slot_updateProcess(qint64 cur,qint64 max);
    void on_pb_page2_clicked();
    void on_pb_page1_clicked();


private:
    Ui::UploadDialog *ui;

    QString m_filePath;
    QString m_imgPath;

    RecorderDialog *m_recorderDialog;

};

#endif // UPLOADDIALOG_H
