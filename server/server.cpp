#include <cstddef>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "../protocol/protocol.hpp"
#include "../protocol/user/user.pb.h"
#include "../handler/handler.hpp"
#include "../network/socket/socket.hpp"
#include "../network/epoll/epoll.hpp"
#include "../network/connection/connection.hpp"
#include "../network/recv_send/recv_send.hpp"
#include "../network/thread_pool/thread_pool.hpp"
#include "../database/db_config.hpp"
#include "../database/db_pool.hpp"
#include "../database/db_init.hpp"
#include "../database/redis.hpp"
#include "db_admin.hpp"

// 运行标志。主循环 + dbadmin 控制台线程都会读它(停机时控制台线程退出)。
volatile bool g_running = true;

extern "C" void handle_signal(int sig) {
    (void)sig;
    g_running = false;
}

// 在线用户表: user_id -> fd。仅主线程访问, 无需加锁。
// 登录时绑定、登出/断开时清除; 用于把 ChatNotify 等推送路由到接收方连接。
static std::unordered_map<uint32_t, int> g_user_to_fd;

// 心跳超时阈值(毫秒): 超过该时长未收到任何包的已登录连接视为登出。
constexpr uint64_t HEARTBEAT_TIMEOUT_MS = 15000;

// 已投递"合成登出"任务、待其结果返回后关闭的连接。仅主线程访问。
// 存指针: 连接若被其它路径提前关闭, 其 fd 复用时不会误关新连接。
static std::unordered_set<Connection*> g_pending_close;

// 被"顶号"(同账号在别处登录)踢出的旧连接: 已把"被顶号"通知追加到其发送缓冲,
// 待 EPOLLOUT 排空后关闭(确保旧客户端先收到提示再断开)。仅主线程访问。
static std::unordered_set<Connection*> g_kick_close;

// 全局 TLS 上下文: 进程内共享, 加载同一份服务端证书。仅主线程创建。
static SSL_CTX* g_ssl_ctx = nullptr;

// 依次查找证书文件, 返回第一个可读的绝对/相对路径, 找不到返回空串。
// 候选顺序: 显式参数 > 当前目录 > 可执行文件所在目录 > 其上一级目录。
// (gen_cert.sh 把证书生成在仓库根; 而 server 二进制在 build/ 下, 从任意 cwd 启动都应能找到)
static std::string find_file(const char* explicit_path, const char* base_name) {
    std::string exe_dir;
    {
        char buf[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            char* slash = strrchr(buf, '/');
            if (slash != nullptr) {
                *slash = '\0';
                exe_dir = buf;
            }
        }
    }

    std::vector<std::string> candidates;
    if (explicit_path != nullptr && explicit_path[0] != '\0') {
        candidates.push_back(explicit_path);
    }
    candidates.push_back(base_name);                      // 当前工作目录
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir + "/" + base_name);            // 可执行文件目录
        candidates.push_back(exe_dir + "/../" + base_name);         // 上一级(仓库根)
    }
    for (const auto& p : candidates) {
        if (access(p.c_str(), R_OK) == 0) {
            return p;
        }
    }
    return std::string();
}

// 初始化服务端 TLS 上下文并加载证书/私钥(自动定位证书路径)
static int init_tls(const char* cert_arg, const char* key_arg) {
    std::string cert = find_file(cert_arg, "server.crt");
    std::string key  = find_file(key_arg, "server.key");
    if (cert.empty() || key.empty()) {
        fprintf(stderr, "[error] TLS cert/key not found (cert=%s key=%s).\n"
                        "  Tried: cwd, <exe_dir>, <exe_dir>/..\n"
                        "  Generate them first: ./gen_cert.sh   (produces server.crt/server.key)\n",
                cert_arg, key_arg);
        return -1;
    }

    g_ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (g_ssl_ctx == nullptr) {
        fprintf(stderr, "[error] SSL_CTX_new failed\n");
        return -1;
    }
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
    // 禁掉 TLS1.2 重协商: 避免 SSL_read 期间需要反向写、SSL_write 期间需要反向读的复杂路径
    SSL_CTX_set_options(g_ssl_ctx, SSL_OP_NO_RENEGOTIATION);
    if (SSL_CTX_use_certificate_chain_file(g_ssl_ctx, cert.c_str()) != 1) {
        fprintf(stderr, "[error] load certificate failed: %s\n", cert.c_str());
        ERR_print_errors_fp(stderr);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, key.c_str(), SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "[error] load private key failed: %s\n", key.c_str());
        ERR_print_errors_fp(stderr);
        return -1;
    }
    fprintf(stdout, "[init] tls ready (cert=%s key=%s)\n", cert.c_str(), key.c_str());
    return 0;
}

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 连接即将关闭时, 若它绑定了用户则清除在线表项;
// 若它是"心跳超时待关"连接, 一并从待关集合移除(避免悬挂指针残留)。
static void clear_user_binding(int fd) {
    Connection* conn = connection_get(fd);
    if (conn != nullptr && conn->user_id() != 0) {
        g_user_to_fd.erase(conn->user_id());
    }
    if (conn != nullptr) {
        g_pending_close.erase(conn);
        g_kick_close.erase(conn);
    }
}

int main(int argc,char* argv[]){

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);// 返回 EPIPE 而不是杀进程

    const char* ip="0.0.0.0";
    int port=2100;
    if(argc>1)port=std::atoi(argv[1]);
    if(argc>2)ip=argv[2];
    const char* cert_file = "server.crt";
    const char* key_file  = "server.key";
    if(argc>3)cert_file=argv[3];
    if(argc>4)key_file=argv[4];

    connection_init();
    fprintf(stdout,"[init] connection pool init successfully\n");

    // ---- 初始化 MySQL 连接池 + 建表 ----
    DbConfig db_cfg;
    db_cfg.load_from_env();
    if (!db_pool().init(db_cfg)) {
        fprintf(stderr, "[error] MySQL init failed (%s:%u db=%s).\n"
                        "  Did you create the database & account? e.g.\n"
                        "  sudo mysql -uroot -e \"CREATE DATABASE IF NOT EXISTS chatroom "
                        "CHARACTER SET utf8mb4; CREATE USER IF NOT EXISTS 'chatroom'@'127.0.0.1' "
                        "IDENTIFIED BY 'chatroom'; GRANT ALL ON chatroom.* TO 'chatroom'@'127.0.0.1';\"\n"
                        "  Or set CHATROOM_DB_* env vars.\n",
                db_cfg.host.c_str(), db_cfg.port, db_cfg.database.c_str());
        return 1;
    }
    {
        DbGuard g(db_pool());
        if (!g || !init_db(*g)) {
            fprintf(stderr, "[error] init_db failed: %s\n", g ? g->error().c_str() : "no connection");
            return 1;
        }
    }
    fprintf(stdout, "[init] mysql pool ready (host=%s:%u db=%s pool=%zu)\n",
            db_cfg.host.c_str(), db_cfg.port, db_cfg.database.c_str(), db_cfg.pool_size);

    // ---- 初始化 Redis 缓存池(fail-open) ----
    // Redis 只是热读缓存, 初始化失败仅告警不退出: 之后 cache 全部回退 MySQL,
    // 功能不受影响(见 database/redis.hpp 的降级约定)。
    RedisConfig redis_cfg;
    redis_cfg.load_from_env();
    if (!redis_pool().init(redis_cfg)) {
        fprintf(stderr, "[warn] Redis cache unavailable (%s:%u pool=%zu), "
                        "cache-aside disabled, falling back to MySQL.\n"
                        "  Start redis-server, or set CHATROOM_REDIS_* env vars to enable.\n",
                redis_cfg.host.c_str(), redis_cfg.port, redis_cfg.pool_size);
    } else {
        fprintf(stdout, "[init] redis cache ready (host=%s:%u pool=%zu)\n",
                redis_cfg.host.c_str(), redis_cfg.port, redis_cfg.pool_size);
    }

    // ---- 初始化 TLS(全连接加密) ----
    // init_tls 内部会自动定位证书(见 find_file), 成功时打印实际加载路径
    if (init_tls(cert_file, key_file) != 0) {
        fprintf(stderr, "[error] TLS init failed (cert=%s key=%s).\n"
                        "  Generate a self-signed cert first: ./gen_cert.sh\n",
                cert_file, key_file);
        return 1;
    }

    network::Socket listen_sock;

    if(listen_sock.create_server(ip,port)!=0){
    fprintf(stdout,"[error] failed to create server\n");
        return 1; 
    }
    listen_sock.set_nonblock();


    network::Epoll epoll_main(1024);
    epoll_main.add(listen_sock.fd(),EPOLLIN);
    fprintf(stdout,"[init] epoll created successfully fd=%d\n",listen_sock.fd());

     ThreadPool pool;
     pool.start([](Task task)->TaskResult{

        return handler::handle_task(task);
     });
     
     fprintf(stdout,"[init] thread pool started\n");

     // 运行期数据库查看控制台: stdin 输入命令实时查询底层数据(只读)。
     dbadmin::start_console();

     recv_send rs_tool;

     // 线程池结果就绪通知: 结果入队即写 eventfd 唤醒主循环,
     // 避免每轮只能靠 epoll 超时(100ms)才取回结果 —— 大文件分片等高频
     // 请求/响应因此不再被 100ms 门控拖慢。
     const int result_evt_fd = pool.result_event_fd();
     if (result_evt_fd >= 0) {
         epoll_main.add(result_evt_fd, EPOLLIN);
     }

     // 处理线程池结果(响应发送/连接关闭/在线表路由推送)。eventfd 触发时与
     // 循环顶部各调用一次; try_get_result 非阻塞, 重复调用安全。
     auto drain_results = [&]() {
         TaskResult result;
         while (pool.try_get_result(result)) {  // 处理任务结果循环

             Connection* conn = connection_get(result.fd);
             if (result.need_close) {
                 clear_user_binding(result.fd);
                 epoll_main.del(result.fd);
                 connection_remove(result.fd);
                 fprintf(stdout, "[close] fd=%d closed successfully as requested\n", result.fd);
             } else if (conn != nullptr) {
                 // 登录成功的结果带有 user_id, 把连接绑定为该用户并登记在线表。
                 // 若该账号已在本服务端在线(顶号): 先踢掉旧连接(追加"被顶号"通知,
                 // 排空后关闭), 新连接取而代之。
                 if (result.user_id != 0) {
                     auto old_it = g_user_to_fd.find(result.user_id);
                     if (old_it != g_user_to_fd.end() && old_it->second != result.fd) {
                         const int old_fd = old_it->second;
                         Connection* old_conn = connection_get(old_fd);
                         g_user_to_fd.erase(old_it);       // 先摘掉旧映射
                         if (old_conn != nullptr) {
                             old_conn->set_user_id(0);     // 解绑, 不再接收推送
                             g_pending_close.erase(old_conn);
                             PendingPush kick = handler::make_system_notify(
                                 result.user_id, "你的账号已在别处登录, 此连接已被强制下线");
                             if (rs_tool.AppendSendBuffer(*old_conn, kick.data.data(),
                                                          kick.data.size())) {
                                 g_kick_close.insert(old_conn);   // 排空后关闭
                                 epoll_main.mod(old_fd, EPOLLIN | EPOLLOUT);
                             } else {
                                 epoll_main.del(old_fd);
                                 connection_remove(old_fd);
                             }
                             fprintf(stdout, "[kick] user=%u fd=%d replaced by fd=%d\n",
                                     result.user_id, old_fd, result.fd);
                         }
                     }
                     conn->set_user_id(result.user_id);
                     g_user_to_fd[result.user_id] = result.fd;
                     fprintf(stdout, "[bind] fd=%d -> user=%u\n", result.fd, result.user_id);
                 }
                 // 登出: 解绑用户与在线表, 但不关闭连接
                 if (result.unbind_user && conn->user_id() != 0) {
                     g_user_to_fd.erase(conn->user_id());
                     fprintf(stdout, "[unbind] fd=%d unbind user=%u\n",
                             result.fd, conn->user_id());
                     conn->set_user_id(0);
                 }
                 if (!result.data.empty()) {
                     if (!rs_tool.AppendSendBuffer(*conn, result.data.data(), result.data.size())) {
                         clear_user_binding(result.fd);
                         epoll_main.del(result.fd);
                         connection_remove(result.fd);
                     } else {
                         epoll_main.mod(result.fd, EPOLLIN | EPOLLOUT);
                     }
                 }
             }

             // 处理需推送给其它在线用户的包(如ChatNotify)
             for (const auto& push : result.pushes) {
                 auto it = g_user_to_fd.find(push.to_user_id);
                 if (it == g_user_to_fd.end()) continue;   // 不在线, 丢弃(消息已落库, 可拉历史)
                 Connection* pconn = connection_get(it->second);
                 if (pconn == nullptr) {
                     g_user_to_fd.erase(it);
                     continue;
                 }
                 if (rs_tool.AppendSendBuffer(*pconn, push.data.data(), push.data.size())) {
                     epoll_main.mod(it->second, EPOLLIN | EPOLLOUT);
                 } else {
                     clear_user_binding(it->second);
                     epoll_main.del(it->second);
                     connection_remove(it->second);
                 }
             }

             // 心跳超时触发的合成登出结果已处理: 关闭该连接。
             // 仅当连接仍存在且仍是投递任务时的那一个才关(防 fd 复用误关新连接)。
             Connection* rc = connection_get(result.fd);
             if (rc != nullptr && g_pending_close.erase(rc)) {
                 clear_user_binding(result.fd);
                 epoll_main.del(result.fd);
                 connection_remove(result.fd);
                 fprintf(stdout, "[close] fd=%d (heartbeat timeout logout)\n", result.fd);
             }
         }
     };

     while (g_running) {

        drain_results();

        int ev_num = epoll_main.wait(100);

        if(ev_num<0){
            if(errno==EINTR)continue;//被信号打断，重试
            perror("[error] epoll_wait");
            break;
        }

        for(int i=0;i<ev_num;i++){

            const struct epoll_event& ev=epoll_main.events()[i];
            const int ev_data_fd=ev.data.fd;

            // 线程池结果就绪: 排空 eventfd 计数器并立即处理结果(不必等 100ms 超时)
            if (result_evt_fd >= 0 && ev_data_fd == result_evt_fd) {
                uint64_t counter = 0;
                ssize_t rr = ::read(result_evt_fd, &counter, sizeof(counter));
                (void)rr;
                drain_results();
                continue;
            }

            if(ev_data_fd==listen_sock.fd()){//新连接
                if(ev.events&EPOLLIN){
                    while(true){
                        //避免 Socket::accept()对 EAGAIN 也调 perror
                        int connfd=::accept4(listen_sock.fd(), nullptr, nullptr,
                                               SOCK_NONBLOCK);
                        if(connfd<0){
                            if(errno==EAGAIN||errno==EINTR)
                                break;          // 已无新连接
                            perror("[error] accept");
                            break;
                        }
                        int optval = 1;
                        setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

                        if (connection_add(connfd) != 0) {
                            ::close(connfd);
                            fprintf(stderr, "[warn] connection_add failed for fd=%d\n", connfd);
                            continue;
                        }
                        if (Connection* cnew = connection_get(connfd)) {
                            cnew->set_last_active(now_ms());

                            // 创建 TLS 会话并进入握手态(握手在事件循环里非阻塞完成)
                            SSL* ssl = SSL_new(g_ssl_ctx);
                            if (ssl == nullptr) {
                                fprintf(stderr, "[warn] SSL_new failed for fd=%d\n", connfd);
                                connection_remove(connfd);
                                continue;
                            }
                            SSL_set_fd(ssl, connfd);
                            SSL_set_accept_state(ssl);
                            cnew->set_ssl(ssl);
                            cnew->set_tls_state(TlsState::HANDSHAKE);
                        }
                        epoll_main.add(connfd, EPOLLIN);
                        fprintf(stdout, "[accept] fd=%d (tls handshake pending)\n", connfd);
                    }


                }
                continue;


            }
            // 处理一个已激活(握手完成)连接的可读事件: 收包 → 提交线程池。
            // 抽成 lambda 以便"握手完成但 SSL 内部已缓冲明文"也能复用同一路径。
            // 返回 false 表示连接已被关闭(调用方应 continue, 不要再碰 conn)。
            auto process_readable = [&](int cfd) -> bool {
                Connection* c = connection_get(cfd);
                if (c == nullptr) return false;

                int ret = rs_tool.Recv(*c);
                if (ret == 0 || ret == -1) {
                    // ret == 0  → 对端关闭 (EOF / close_notify)
                    // ret == -1 → 接收错误
                    clear_user_binding(cfd);
                    epoll_main.del(cfd);
                    connection_remove(cfd);
                    fprintf(stdout, "[close] fd=%d (recv=%d)\n", cfd, ret);
                    return false;
                }
                if (ret > 0) {
                    c->set_last_active(now_ms());   // 收到包: 更新活性时间戳
                }
                if (ret == -3) {
                    // TLS 需向外刷数据(罕见): 挂上写事件, 让 SSL 内部输出落盘
                    epoll_main.mod(cfd, EPOLLIN | EPOLLOUT);
                    return true;
                }
                // ret > 0  读到数据; ret == -2 为 EAGAIN/WANT_READ(无数据可读), 忽略继续

                // 从接收缓冲区中取出所有完整数据包
                while (rs_tool.HasCompletePacket(*c)) {
                    // 读取包头以获取 body 长度
                    auto* hdr = reinterpret_cast<protocol::packet_header*>(c->recv_buffer());
                    std::size_t total_len = sizeof(protocol::packet_header) + hdr->body_len;

                    Task task;
                    task.fd  = cfd;
                    task.user_id = c->user_id();
                    task.data.resize(total_len);

                    std::size_t packet_len = 0;
                    int fetch_ret = rs_tool.FetchPacket(*c, task.data.data(), packet_len);
                    if (fetch_ret == 0 && packet_len > 0) {
                        pool.submit(std::move(task));
                    } else {
                        break;//提取失败等待更多数据
                    }
                }
                return true;
            };

            //数据连接
            Connection* conn = connection_get(ev.data.fd);
            if (conn == nullptr) {
                continue;   // 竞态：已在另一个事件处理中被清理
            }

            // 异常 / 挂断(握手阶段同样适用)
            if (ev.events & (EPOLLERR | EPOLLHUP)) {
                clear_user_binding(ev.data.fd);
                epoll_main.del(ev.data.fd);
                connection_remove(ev.data.fd);
                fprintf(stdout, "[close] fd=%d (EPOLLERR|EPOLLHUP)\n", ev_data_fd);
                continue;
            }

            // ---- TLS 握手阶段: 非阻塞, 在 EPOLLIN/EPOLLOUT 之间推进 ----
            if (conn->tls_state() == TlsState::HANDSHAKE) {
                int r = SSL_accept(conn->ssl());
                if (r == 1) {
                    conn->set_tls_state(TlsState::ACTIVE);
                    fprintf(stdout, "[tls] fd=%d handshake done (%s)\n",
                            ev_data_fd, SSL_get_version(conn->ssl()));
                    // 握手完成时可能已伴随应用数据(或被 SSL 内部缓冲): 有数据就一并处理
                    bool has_data = (ev.events & EPOLLIN) || SSL_pending(conn->ssl()) > 0;
                    if (has_data) {
                        process_readable(ev_data_fd);
                    }
                    continue;
                }
                int err = SSL_get_error(conn->ssl(), r);
                if (err == SSL_ERROR_WANT_READ) {
                    epoll_main.mod(ev_data_fd, EPOLLIN);
                    continue;
                }
                if (err == SSL_ERROR_WANT_WRITE) {
                    epoll_main.mod(ev_data_fd, EPOLLOUT);
                    continue;
                }
                // 握手失败(协议错误/超时): 直接关闭, 防恶意连接占用资源
                clear_user_binding(ev_data_fd);
                epoll_main.del(ev_data_fd);
                connection_remove(ev_data_fd);
                fprintf(stdout, "[close] fd=%d (tls handshake fail err=%d)\n", ev_data_fd, err);
                continue;
            }

            // 可读
            if (ev.events & EPOLLIN) {
                if (!process_readable(ev_data_fd)) {
                    continue;   // 连接已关闭
                }
            }
            //可写
            if (ev.events & EPOLLOUT) {
                int ret = rs_tool.Send(*conn);
                if (ret == -3) {
                    // TLS 需读数据(罕见): 不关闭, 重挂读写事件
                    epoll_main.mod(ev_data_fd, EPOLLIN | EPOLLOUT);
                    continue;
                }
                if (ret < 0) {
                    clear_user_binding(ev_data_fd);
                    epoll_main.del(ev_data_fd);
                    connection_remove(ev_data_fd);
                    fprintf(stdout, "[close] fd=%d (send error)\n", ev_data_fd);
                    continue;
                }

                // 发送缓冲区已空: 若是被顶号踢出的旧连接, "被顶号"通知已发出, 关闭它
                if (conn->send_length() == 0) {
                    if (g_kick_close.erase(conn)) {
                        clear_user_binding(ev_data_fd);
                        epoll_main.del(ev_data_fd);
                        connection_remove(ev_data_fd);
                        fprintf(stdout, "[kick] fd=%d closed (kicked user notified)\n", ev_data_fd);
                    } else {
                        epoll_main.mod(ev_data_fd, EPOLLIN);
                    }
                }
            }

        }//wait结果处理循环结束

        // 心跳超时检测: 每 ~1s 扫描已登录连接活性, 超时视为登出。
        static uint64_t next_idle_scan_ms = now_ms() + 1000;
        uint64_t now = now_ms();
        if (now >= next_idle_scan_ms) {
            next_idle_scan_ms = now + 1000;
            for (int i = 0; i < MAX_CONN; ++i) {
                Connection* c = connection_get(i);
                if (c == nullptr || c->user_id() == 0) continue;   // 只判已登录连接
                if (now - c->last_active() <= HEARTBEAT_TIMEOUT_MS) continue;

                fprintf(stdout, "[hb] user=%u fd=%d heartbeat timeout, treat as logout\n",
                        c->user_id(), c->fd());
                // 1) 立即停止推送路由到该连接
                g_user_to_fd.erase(c->user_id());
                // 2) 投递合成登出任务: 工作线程完成 DB 置离线 + 好友"已下线"通知
                protocol::user::UserPacket req;
                req.mutable_logout_req()->set_user_id(c->user_id());
                std::string body;
                req.SerializeToString(&body);
                Task t;
                t.fd = c->fd();
                t.user_id = c->user_id();
                t.data.resize(sizeof(protocol::packet_header) + body.size());
                auto* hdr = reinterpret_cast<protocol::packet_header*>(t.data.data());
                hdr->magic = protocol::MAGIC_NUM;
                hdr->ver = 1;
                hdr->type = protocol::DOMAIN_USER;
                hdr->body_len = static_cast<uint32_t>(body.size());
                std::memcpy(t.data.data() + sizeof(protocol::packet_header),
                            body.data(), body.size());
                pool.submit(std::move(t));
                // 3) 记录待关连接, 结果返回后再关闭
                g_pending_close.insert(c);
            }
        }

     }//主循环结束

    // 关闭 stdin: 解除 dbadmin 控制台线程在 getline 上的阻塞(输入到一半时也生效),
    // 否则进程退出时 glibc 清理 stdin 流会与 getline 的持锁死锁。
    close(STDIN_FILENO);

    fprintf(stdout, "\n[shutdown] stopping thread pool...\n");
    pool.stop();

    fprintf(stdout, "[shutdown] closing all connections...\n");
    for (int i = 0; i < MAX_CONN; ++i) {
        connection_remove(i);
    }

    if (g_ssl_ctx != nullptr) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = nullptr;
    }

    fprintf(stdout, "[shutdown] server stopped gracefully.\n");
    return 0;



}