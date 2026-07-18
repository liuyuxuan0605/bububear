// stress_test_client.cpp
// ----------------------------------------------------------------------------
// MediaServer 最大连接数 / 并发连接压力测试工具
//
// 设计思路：
//   压测端如果"一个连接开一个线程"，几千个连接就会先把压测机自己的线程调度
//   拖垮，测出来的瓶颈是压测机而不是被测的 MediaServer。所以这个工具跟
//   MediaServer 自己（block_epoll_net.cpp）一样，用【单线程 + epoll】去驱动
//   海量非阻塞 connect()，一个线程就能同时跟踪几万个连接的状态。
//
//   连接建立过程分两步在 epoll 里跟踪：
//     1) connect() 非阻塞发起后立刻返回 EINPROGRESS，把 fd 挂到 epoll 上等
//        EPOLLOUT 事件（可写事件到来 = 三次握手完成或者失败）。
//     2) EPOLLOUT 到来后用 getsockopt(SO_ERROR) 判断到底是"连上了"还是
//        "连接失败"（不能只看 EPOLLOUT 触发就当成功，失败也会触发一次）。
//     3) 连上之后把 epoll 关注事件切成 EPOLLIN，用来感知服务端主动断开
//        （对端 close/RST 会体现为可读但 recv()==0 或者出错）。
//
// 编译（在能跑 MediaServer 的 Linux 机器上）：
//   g++ -std=c++11 -O2 -o stress_test_client stress_test_client.cpp
//
// 用法示例：
//   压测本机服务端，目标 20000 个并发连接，按每秒 1000 个的速度爬升：
//     ./stress_test_client --ip 127.0.0.1 --port 8001 --target 20000 --rate 1000
//
//   额外带上真实的登录协议包（会真正走到 CLogic::LoginRq，包含一次 MySQL
//   查询），更接近业务连接的压测，而不只是测 TCP 层的连接数上限：
//     ./stress_test_client --ip 192.168.129.135 --port 8001 --target 5000 --rate 300 --login
//
//   达到目标后保持连接 60 秒再自动退出（默认是 0，即一直保持到 Ctrl+C）：
//     ./stress_test_client --ip 127.0.0.1 --port 8001 --target 20000 --hold 60
//
// 压测前必须确认的几个"隐藏上限"（不调这些，压测结果测的是这些限制，不是
// MediaServer 本身的能力）：
//
//   1) 压测机自身的 fd 上限：本工具启动时会自动尝试把自己的 RLIMIT_NOFILE
//      提到 hard limit，但如果 hard limit 本身很低（常见发行版默认
//      1024/4096），要先用 `ulimit -Hn` 查，不够的话在 /etc/security/limits.conf
//      里加大，或者本次会话内 `ulimit -n 65535`（需要 root 或 hard limit 允许）。
//
//   2) 压测机的临时端口范围：一台机器对同一个 目的IP:端口 发起 TCP 连接，
//      最多能用的连接数受限于本地临时端口数量（一般几万个，具体看
//      /proc/sys/net/ipv4/ip_local_port_range），如果要测超过这个数量级
//      的并发（比如 10 万+），单台压测机是测不出来的，需要多台压测机
//      或者给压测机绑定多个源 IP。
//
//   3) 服务端自身的 fd 上限：MediaServer 进程也受 ulimit -n 限制，这是
//      Block_Epoll_Net 能同时维护多少个 myevent_s 的硬上限，压测前要在
//      服务端这边也确认/调大。
//
//   4) 服务端 listen() 的 backlog：block_epoll_net.cpp 里 `listen(m_listenfd,128)`，
//      backlog=128 是"已完成三次握手、等待 accept() 取走"的队列长度。如果
//      --rate 设得很高（短时间内连接请求暴涨），超过 backlog 的连接会被
//      内核直接拒绝或丢弃，表现为大量 ECONNREFUSED——这本身也是一个有意义
//      的压测结论（"当前 backlog=128 扛不住每秒 N 个新连接的爆发"），但如果
//      你想测的是"能维持的最大连接数"而不是"能承受的连接爆发速度"，建议把
//      --rate 调低一些，让连接平缓爬升，避免被 backlog 这个独立瓶颈干扰结果。
//
//   5) epoll 的 MAX_EVENTS(4096) 不是连接数上限：block_epoll_net.h 里
//      `#define MAX_EVENTS 4096` 只是每次 epoll_wait 一口气最多取回多少个
//      就绪事件，不是 epoll 能监听的 fd 总数上限（epoll 本身没有这个硬编码
//      上限，只受 fd 数量限制）。压测时不用担心这个宏。
// ----------------------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <chrono>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/resource.h>

// ---------------------------------------------------------------------------
// 和 MediaServer/include/packdef.h 保持一致的最小协议镜像
// 只用于 --login 模式下发一个真实的登录请求包，走到服务端 CLogic::LoginRq。
// 注意：这里手动镜像而不是直接 #include 服务端头文件，是为了让这个压测工具
// 保持单文件、不依赖工程其余部分就能独立编译。如果服务端协议改了字段，
// 这里要跟着同步改，否则 --login 模式发出去的包服务端会按错误的偏移量解析。
// ---------------------------------------------------------------------------
static const int32_t PACK_BASE      = 10000; // 对应 packdef.h 的 _DEF_PACK_BASE
static const int32_t PACK_LOGIN_RQ  = PACK_BASE + 2; // _DEF_PACK_LOGIN_RQ
static const int     NAME_FIELD_LEN = 40;    // 对应 packdef.h 的 _MAX_SIZE

struct LoginRqMirror {
    int32_t type;
    char    user[NAME_FIELD_LEN];
    char    password[NAME_FIELD_LEN];
};

// ---------------------------------------------------------------------------
// 连接状态
// ---------------------------------------------------------------------------
enum ConnPhase { PHASE_CONNECTING, PHASE_ESTABLISHED };

struct ConnInfo {
    ConnPhase phase;
    int       index; // 第几个发起的连接，用于 --login 模式拼用户名
};

struct Stats {
    long attempted    = 0; // 已发起 connect() 的次数
    long establishedNow = 0; // 当前仍然存活的连接数
    long establishedTotalPeak = 0; // 峰值存活连接数
    long connectFailed = 0; // connect 阶段失败（含立即失败和 EPOLLOUT 后发现出错）
    long peerClosed    = 0; // 建立后被对端关闭/重置
    long errEMFILE     = 0;
    long errRefused    = 0;
    long errTimedOut   = 0;
    long errResetOrOther = 0;
};

static volatile sig_atomic_t g_stop = 0;
static void OnSigInt(int) { g_stop = 1; }

static void RaiseFdLimit()
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        fprintf(stderr, "[warn] getrlimit failed: %s\n", strerror(errno));
        return;
    }
    rlim_t oldSoft = rl.rlim_cur;
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        fprintf(stderr, "[warn] setrlimit(RLIMIT_NOFILE) 提升失败: %s"
                        "（当前 soft limit 仍是 %lu，如果目标并发数比这个大，"
                        "会提前撞上 EMFILE，请用 root 权限或者在 "
                        "/etc/security/limits.conf 里调大 hard limit 后重试）\n",
                strerror(errno), (unsigned long)oldSoft);
        return;
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    printf("[info] 压测进程 fd 上限已从 %lu 提升到 soft=%lu (hard=%lu)\n",
           (unsigned long)oldSoft, (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);
}

static const char* ClassifyErrno(int err)
{
    switch (err) {
        case ECONNREFUSED: return "ECONNREFUSED(服务端未监听/backlog满被拒)";
        case ETIMEDOUT:     return "ETIMEDOUT(连接超时)";
        case EMFILE:        return "EMFILE(压测机自身fd用尽)";
        case ENFILE:        return "ENFILE(系统级fd用尽)";
        case ECONNRESET:    return "ECONNRESET(被RST)";
        case EADDRNOTAVAIL: return "EADDRNOTAVAIL(本地临时端口用尽)";
        default:             return "其他错误";
    }
}

static void PrintUsage(const char* prog)
{
    printf("用法: %s --ip <server_ip> [--port 8001] --target <连接数> "
           "[--rate 每秒新建连接数=500] [--hold 秒数=0,0表示一直保持到Ctrl+C] "
           "[--login]\n", prog);
}

int main(int argc, char** argv)
{
    std::string ip = "127.0.0.1";
    int port = 8001;          // 对应 TCPKernel.h 里的 _DEF_PORT 默认端口
    long target = 5000;
    long ratePerSec = 500;
    long holdSeconds = 0;
    bool withLogin = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "%s 需要一个参数\n", opt); exit(1); }
            return argv[++i];
        };
        if (a == "--ip") ip = next("--ip");
        else if (a == "--port") port = atoi(next("--port"));
        else if (a == "--target") target = atol(next("--target"));
        else if (a == "--rate") ratePerSec = atol(next("--rate"));
        else if (a == "--hold") holdSeconds = atol(next("--hold"));
        else if (a == "--login") withLogin = true;
        else if (a == "--help" || a == "-h") { PrintUsage(argv[0]); return 0; }
        else { fprintf(stderr, "未知参数: %s\n", a.c_str()); PrintUsage(argv[0]); return 1; }
    }

    if (target <= 0 || ratePerSec <= 0) {
        fprintf(stderr, "--target 和 --rate 必须为正数\n");
        return 1;
    }

    signal(SIGINT, OnSigInt);
    signal(SIGPIPE, SIG_IGN); // 对已断开的 fd send() 不要让进程被 SIGPIPE 杀掉
    RaiseFdLimit();

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) != 1) {
        fprintf(stderr, "非法 IP: %s\n", ip.c_str());
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return 1;
    }

    std::unordered_map<int, ConnInfo> conns;
    conns.reserve((size_t)target * 2);

    Stats stats;
    const int MAX_EVENTS_PER_WAIT = 2048;
    epoll_event events[MAX_EVENTS_PER_WAIT];

    auto startTime = std::chrono::steady_clock::now();
    auto lastPrint = startTime;
    auto reachedTargetAt = std::chrono::steady_clock::time_point(); // 未设置
    bool reachedTarget = false;

    printf("[info] 开始压测：目标 %ld 连接，爬升速率 %ld/秒，目标 %s:%d%s\n",
           target, ratePerSec, ip.c_str(), port, withLogin ? "（附带登录包）" : "（纯TCP连接，不发业务包）");

    // 令牌桶：每一轮循环按照经过的时间计算"这一刻理论上应该发起了多少个连接"，
    // 用差值控制本轮实际发起多少个新连接，避免瞬间打出一个大 burst 把服务端
    // listen backlog 打爆（除非本来就是想测这个）。
    while (!g_stop) {
        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - startTime).count();

        // ---- 按速率爬升发起新连接 ----
        if (stats.attempted < target) {
            long shouldHaveAttempted = (long)(elapsedSec * ratePerSec);
            if (shouldHaveAttempted > target) shouldHaveAttempted = target;
            long toCreate = shouldHaveAttempted - stats.attempted;
            // 单轮最多发起的量做个上限保护，避免定时器漂移导致一次性冲太猛
            if (toCreate > 1000) toCreate = 1000;

            for (long i = 0; i < toCreate && stats.attempted < target; ++i) {
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) {
                    stats.attempted++;
                    stats.connectFailed++;
                    if (errno == EMFILE) stats.errEMFILE++;
                    continue;
                }
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);

                int ret = connect(fd, (sockaddr*)&serverAddr, sizeof(serverAddr));
                stats.attempted++;

                if (ret == 0) {
                    // 极少见：本地/超快网络下立刻连上
                    ConnInfo info{PHASE_ESTABLISHED, (int)stats.attempted};
                    conns[fd] = info;
                    stats.establishedNow++;
                    if (stats.establishedNow > stats.establishedTotalPeak)
                        stats.establishedTotalPeak = stats.establishedNow;

                    if (withLogin) {
                        // 见下方统一发送逻辑，这里简单复用不重复实现，直接标记为
                        // 已连接，下一轮循环里对 EPOLLIN 的处理不会误判，登录包
                        // 在下面 EPOLLOUT 分支里统一发送；为了代码简洁，这里也走
                        // 一遍同样的发送函数：
                    }
                    epoll_event ev{};
                    ev.events = EPOLLIN;
                    ev.data.fd = fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
                } else if (errno == EINPROGRESS) {
                    ConnInfo info{PHASE_CONNECTING, (int)stats.attempted};
                    conns[fd] = info;
                    epoll_event ev{};
                    ev.events = EPOLLOUT;
                    ev.data.fd = fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
                } else {
                    stats.connectFailed++;
                    if (errno == ECONNREFUSED) stats.errRefused++;
                    else if (errno == ETIMEDOUT) stats.errTimedOut++;
                    else if (errno == EMFILE || errno == ENFILE) stats.errEMFILE++;
                    else if (errno == EADDRNOTAVAIL) stats.errResetOrOther++;
                    close(fd);
                }
            }
        }

        // ---- 处理 epoll 事件 ----
        int n = epoll_wait(epfd, events, MAX_EVENTS_PER_WAIT, 50 /*ms*/);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            auto it = conns.find(fd);
            if (it == conns.end()) continue;

            if (it->second.phase == PHASE_CONNECTING) {
                int soErr = 0;
                socklen_t len = sizeof(soErr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len);
                if (soErr == 0) {
                    // 三次握手真正完成
                    it->second.phase = PHASE_ESTABLISHED;
                    stats.establishedNow++;
                    if (stats.establishedNow > stats.establishedTotalPeak)
                        stats.establishedTotalPeak = stats.establishedNow;

                    if (withLogin) {
                        LoginRqMirror rq;
                        memset(&rq, 0, sizeof(rq));
                        rq.type = PACK_LOGIN_RQ;
                        snprintf(rq.user, sizeof(rq.user), "stress_%d", it->second.index);
                        // password 留空：服务端查不到这个用户名，直接返回
                        // user_not_exist，不会写库，几乎不给 MySQL 增加负担，
                        // 但完整走了 accept→epoll→线程池→协议分发→SQL查询
                        // 这一整条服务端处理路径，比纯 TCP 连接更接近真实业务压测。
                        char buf[4 + sizeof(rq)];
                        int32_t bodyLen = (int32_t)sizeof(rq);
                        memcpy(buf, &bodyLen, 4);
                        memcpy(buf + 4, &rq, sizeof(rq));
                        send(fd, buf, sizeof(buf), MSG_NOSIGNAL);
                    }

                    epoll_event ev{};
                    ev.events = EPOLLIN;
                    ev.data.fd = fd;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
                } else {
                    stats.connectFailed++;
                    if (soErr == ECONNREFUSED) stats.errRefused++;
                    else if (soErr == ETIMEDOUT) stats.errTimedOut++;
                    else stats.errResetOrOther++;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    conns.erase(it);
                }
            } else { // PHASE_ESTABLISHED，这里只用来感知服务端主动断开
                char tmp[256];
                ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    stats.peerClosed++;
                    stats.establishedNow--;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    conns.erase(it);
                }
                // r > 0：服务端回了业务数据（比如登录回复包），压测工具不关心
                // 内容，读走即可，避免这块数据一直堆在内核缓冲区。
            }
        }

        // ---- 每秒打印一次进度 ----
        if (std::chrono::duration<double>(now - lastPrint).count() >= 1.0) {
            printf("[进度] 耗时=%.0fs 已发起=%ld 存活连接=%ld(峰值%ld) "
                   "连接失败=%ld 被动断开=%ld\n",
                   elapsedSec, stats.attempted, stats.establishedNow,
                   stats.establishedTotalPeak, stats.connectFailed, stats.peerClosed);
            lastPrint = now;
        }

        // ---- 到达目标后的保持/退出逻辑 ----
        if (!reachedTarget && stats.attempted >= target) {
            reachedTarget = true;
            reachedTargetAt = now;
            printf("[info] 已发起全部 %ld 个连接尝试，当前存活 %ld 个。%s\n",
                   target, stats.establishedNow,
                   holdSeconds > 0 ? "开始计时保持阶段..." : "按 Ctrl+C 结束压测并查看汇总。");
        }
        if (reachedTarget && holdSeconds > 0) {
            double heldSec = std::chrono::duration<double>(now - reachedTargetAt).count();
            if (heldSec >= holdSeconds) break;
        }
    }

    // ---- 收尾：打印汇总，关闭所有仍然存活的连接 ----
    printf("\n================ 压测结束汇总 ================\n");
    printf("发起连接总次数        : %ld\n", stats.attempted);
    printf("成功建立过的连接数     : %ld (= 发起数 - connect失败数)\n",
           stats.attempted - stats.connectFailed);
    printf("结束时仍然存活的连接数 : %ld\n", stats.establishedNow);
    printf("整个过程的峰值并发连接 : %ld   <- 这个数字就是本次压测测出的"
           "\"最大并发连接数\"\n", stats.establishedTotalPeak);
    printf("connect阶段失败总数    : %ld\n", stats.connectFailed);
    printf("  其中 ECONNREFUSED   : %ld （服务端拒绝，常见于backlog被打满）\n", stats.errRefused);
    printf("  其中 ETIMEDOUT      : %ld （连接超时，常见于服务端处理不过来）\n", stats.errTimedOut);
    printf("  其中 EMFILE/ENFILE  : %ld （压测机自身fd用尽，不是服务端瓶颈！）\n", stats.errEMFILE);
    printf("  其中 其他错误       : %ld\n", stats.errResetOrOther);
    printf("建立后被服务端/网络断开: %ld\n", stats.peerClosed);
    printf("================================================\n");

    for (auto& kv : conns) {
        close(kv.first);
    }
    close(epfd);
    return 0;
}
