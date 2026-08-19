#include <cstddef>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

     while(g_running){

        {
            TaskResult result;
            while(pool.try_get_result(result)){//处理任务结果循环

                Connection* conn = connection_get(result.fd);
                if(result.need_close){
                    clear_user_binding(result.fd);
                    epoll_main.del(result.fd);
                    connection_remove(result.fd);
                    fprintf(stdout,"[close] fd=%d closed successfully as requested\n",result.fd);
                } else if(conn != nullptr){
                    // 登录成功的结果带有 user_id, 把连接绑定为该用户并登记在线表
                    if(result.user_id != 0){
                        conn->set_user_id(result.user_id);
                        g_user_to_fd[result.user_id] = result.fd;
                        fprintf(stdout, "[bind] fd=%d -> user=%u\n", result.fd, result.user_id);
                    }
                    // 登出: 解绑用户与在线表, 但不关闭连接
                    if(result.unbind_user && conn->user_id() != 0){
                        g_user_to_fd.erase(conn->user_id());
                        fprintf(stdout, "[unbind] fd=%d unbind user=%u\n",
                                result.fd, conn->user_id());
                        conn->set_user_id(0);
                    }
                    if(!result.data.empty()){
                        if(!rs_tool.AppendSendBuffer(*conn, result.data.data(), result.data.size())){
                            clear_user_binding(result.fd);
                            epoll_main.del(result.fd);
                            connection_remove(result.fd);
                        } else {
                            epoll_main.mod(result.fd,EPOLLIN|EPOLLOUT);
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

        }

        int ev_num=epoll_main.wait(100);

        if(ev_num<0){
            if(errno==EINTR)continue;//被信号打断，重试
            perror("[error] epoll_wait");
            break;
        }

        for(int i=0;i<ev_num;i++){

            const struct epoll_event& ev=epoll_main.events()[i];
            const int ev_data_fd=ev.data.fd;

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
                        }
                        epoll_main.add(connfd, EPOLLIN);
                        fprintf(stdout, "[accept] fd=%d\n", connfd);
                    }


                }
                continue;


            }
            //数据连接
            Connection* conn = connection_get(ev.data.fd);
            if (conn == nullptr) {
                continue;   // 竞态：已在另一个事件处理中被清理
            }

            // 异常 / 挂断
            if (ev.events & (EPOLLERR | EPOLLHUP)) {
                clear_user_binding(ev.data.fd);
                epoll_main.del(ev.data.fd);
                connection_remove(ev.data.fd);
                fprintf(stdout, "[close] fd=%d (EPOLLERR|EPOLLHUP)\n", ev_data_fd);
                continue;
            }

            // 可读
            if (ev.events & EPOLLIN) {
                int ret = rs_tool.Recv(*conn);
                if (ret == 0 || ret == -1) {
                    // ret == 0  → 对端关闭 (EOF)
                    // ret == -1 → 接收错误
                    clear_user_binding(ev_data_fd);
                    epoll_main.del(ev_data_fd);
                    connection_remove(ev_data_fd);
                    fprintf(stdout, "[close] fd=%d (recv=%d)\n", ev_data_fd, ret);
                    continue;
                }
                if (ret > 0) {
                    conn->set_last_active(now_ms());   // 收到包: 更新活性时间戳
                }
                // ret > 0  读到数据; ret == -2 为 EAGAIN(无数据可读), 忽略继续


                // 从接收缓冲区中取出所 有完整数据包
                while (rs_tool.HasCompletePacket(*conn)) {
                    // 读取包头以获取 body 长度
                    auto* hdr = reinterpret_cast<protocol::packet_header*>(conn->recv_buffer());
                    std::size_t total_len = sizeof(protocol::packet_header) + hdr->body_len;

                    Task task;
                    task.fd  = ev_data_fd;
                    task.user_id = conn->user_id();
                    task. data.resize(total_len);

                    std::size_t packet_len = 0; 
                    int fetch_ret = rs_tool.FetchPacket(*conn, task.data.data(), packet_len);
                    if (fetch_ret == 0 && packet_len > 0) {
                        pool.submit(std::move(task));
                    } else { 
                        break;//提取失败等待更多数据
                    }
                }
            }
            //可写
            if (ev.events & EPOLLOUT) {
                int ret = rs_tool.Send(*conn);
                if (ret < 0) {
                    clear_user_binding(ev_data_fd);
                    epoll_main.del(ev_data_fd);
                    connection_remove(ev_data_fd);
                    fprintf(stdout, "[close] fd=%d (send error)\n", ev_data_fd);
                    continue;
                }
    
                // 发送缓冲区已空 关闭写监听，避免 epoll 空转
                if (conn->send_length() == 0) {
                    epoll_main.mod(ev_data_fd, EPOLLIN);
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

    fprintf(stdout, "[shutdown] server stopped gracefully.\n");
    return 0;



}