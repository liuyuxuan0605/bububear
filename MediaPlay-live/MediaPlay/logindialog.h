#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>

namespace Ui {
class LoginDialog;
}

struct Hobby{
    char food    ;
    char funny   ;
    char ennegy  ;
    char dance   ;
    char music   ;
    char video   ;
    char outside ;
    char edu     ;
};

class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QWidget *parent = 0);
    ~LoginDialog();



signals:
    void SIG_registerCommit(QString name,QString password,Hobby hy);
    void SIG_loginCommit(QString name,QString password);

private slots:

    void on_pb_register_clear_clicked();
    void slot_login_clear();
    void slot_register_clear();
    void on_pb_register_clicked();
    void on_pb_login_clicked();

    void on_cb_all_clicked();

    void on_cb_allnot_clicked();

private:
    Ui::LoginDialog *ui;
};

#endif // LOGINDIALOG_H
