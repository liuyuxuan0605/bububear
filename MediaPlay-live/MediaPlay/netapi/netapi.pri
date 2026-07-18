QT       += network
LIBS += -lpthread libwsock32 libws2_32
LIBS += -lpthread libMswsock libMswsock


INCLUDEPATH+=$$PWD/net/
INCLUDEPATH+=$$PWD/mediator/

HEADERS += \
    $$PWD/mediator/INetMediator.h \
    $$PWD/mediator/TcpClientMediator.h \
    $$PWD/mediator/UdpMediator.h \
    $$PWD/net/INet.h \
    $$PWD/net/TcpClient.h \
    $$PWD/net/UdpNet.h \
    $$PWD/net/packdef.h


SOURCES += \
    $$PWD/mediator/INetMediator.cpp \
    $$PWD/mediator/TcpClientMediator.cpp \
    $$PWD/mediator/UdpMediator.cpp \
    $$PWD/net/INet.cpp \
    $$PWD/net/TcpClient.cpp \
    $$PWD/net/UdpNet.cpp

