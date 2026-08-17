#include <cstddef>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "../protocol/protocol.hpp"
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

static volatile bool g_running = true;

extern "C" void handle_signal(int sig) {
    (void)sig;
    g_running = false;
}

// 在线用户表: user_id -> fd。仅主线程访问, 无需加锁。
// 登录时绑定、登出/断开时清除; 用于把 ChatNotify 等推送路由到接收方连接。
static std::unordered_map<uint32_t, int> g_user_to_fd;

// 连接即将关闭时, 若它绑定了用户则清除在线表项。
static void clear_user_binding(int fd) {
    Connection* conn = connection_get(fd);
    if (conn != nullptr && conn->user_id() != 0) {
        g_user_to_fd.erase(conn->user_id());
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
 

     }//主循环结束

    fprintf(stdout, "\n[shutdown] stopping thread pool...\n");
    pool.stop();

    fprintf(stdout, "[shutdown] closing all connections...\n");
    for (int i = 0; i < MAX_CONN; ++i) {
        connection_remove(i);
    }

    fprintf(stdout, "[shutdown] server stopped gracefully.\n");
    return 0;



}