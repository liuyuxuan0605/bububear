#include "myopenglwidget.h"
#include<QDebug>

// 初始化 OpenGL
void MyOpenGLWidget::initializeGL()
{
    // 初始化 OpenGL 函数
    initializeOpenGLFunctions();
    // 设置背景颜色为黑色
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // 启用 2D 纹理
    glEnable(GL_TEXTURE_2D);
    // 生成纹理对象并绑定
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    // 设置纹理参数
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}


// 绘制 OpenGL 场景
void MyOpenGLWidget::paintGL()
{
    // 清除颜色缓冲区和深度缓冲区 -- 清理当前显示
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // 如果图像不为空
    if ( !m_image.isNull())
    {
        // 绑定纹理
        glBindTexture(GL_TEXTURE_2D, m_texture);
        // 将图像数据传输到纹理对象中
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_image.width(), m_image.height(), 0, GL_BGRA, GL_UNSIGNED_BYTE, m_image.bits());

        // 获取图像和窗口大小，并计算比例
        QSize imgSize = getImageScaledSize( m_image.size() );// 获取缩放后的图像大小
        QSize wndSize = this->size();// 获取窗口大小
        // 计算图像在窗口中的位置
        float left = - imgSize.width()  ;
        float right =   imgSize.width()  ;
        float up = imgSize.height() ;
        float bottom = -imgSize.height() ;
        // 将位置转换为相对于窗口大小的比例
        left = left*1.0 / wndSize.width();
        right = right*1.0 / wndSize.width();
        up = up*1.0 / wndSize.height();
        bottom = bottom*1.0/wndSize.height();

        // 绘制图像
        glBegin(GL_QUADS);//开始定义一个图元，这里指定绘制的图元类型为四边形（GL_QUADS）
        /// 指定第一个顶点的纹理坐标和顶点坐标。
        /// glTexCoord2f函数设置纹理坐标，(0, 1)表示纹理坐标的左下角（通常为(0, 0)到(1, 1)之间）。
        /// glVertex2d函数设置顶点坐标，(left, bottom)表示矩形的左下角顶点的坐标。
        /// glVertex2d函数设置的点为比例,
        /// 左下正常是(-1,-1), 不过图片居中调整后根据比例换算为( left, bottom )
        /// 以此类推 , 其他点类似
        glTexCoord2f(0, 1); glVertex2d( left, bottom );// 指定第一个顶点的纹理坐标和顶点坐标
        glTexCoord2f(1, 1); glVertex2d( right , bottom );
        glTexCoord2f(1, 0); glVertex2d( right , up );
        glTexCoord2f(0, 0); glVertex2d( left , up );
        /// 结束定义图元
        glEnd();
    }
}

// 窗口大小改变时调用
void MyOpenGLWidget::resizeGL(int w, int h)
{
    //按照固定缩放比 改变
    qDebug() << __func__;
    //设置视口大小
    glViewport(0, 0, w, h);
    // 输出窗口大小信息
    qDebug() << QSize(w,h);

}


// 设置图像，并触发重新绘制
void MyOpenGLWidget::slot_setImage(QImage img)
{
    m_image = img;
    this->repaint();
    return;
}

// 获取图像缩放后的大小
QSize MyOpenGLWidget::getImageScaledSize(QSize size)
{
    return size.scaled( this->size().width() , this->size().height()
                        , Qt::KeepAspectRatio );
}

