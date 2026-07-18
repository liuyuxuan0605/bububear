#pragma once


// 包含必要的头文件
#include <QOpenGLWidget>// 包含Qt中用于创建OpenGL小部件的头文件
#include <QOpenGLFunctions>// 包含Qt中用于处理OpenGL函数的头文件
#include <QImage>// 包含Qt中用于处理图像的头文件

// 自定义的OpenGL窗口小部件
class MyOpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT// 声明Qt元对象系统，允许使用信号和槽机制

public:
    // 构造函数
    MyOpenGLWidget(QWidget *parent = nullptr) : QOpenGLWidget(parent) {}
    // 用于设置图像
    void slot_setImage(QImage img );

protected:
    // OpenGL初始化函数，用于初始化OpenGL状态和对象
    void initializeGL() override;
    // OpenGL绘制函数，用于渲染OpenGL场景
    void paintGL() override;
    // OpenGL大小变化函数，用于处理窗口大小改变事件
    void resizeGL(int w, int h) override;
    // 获取图像缩放后的大小
    QSize getImageScaledSize( QSize size);
private:
    QImage m_image;// 存储图像数据的成员变量
    GLuint m_texture;// 存储OpenGL纹理对象标识符的成员变量
};


