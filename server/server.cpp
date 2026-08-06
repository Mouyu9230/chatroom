#include <cstddef>
#include <signal.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "../protocol/protocol.hpp"
#include "../handler/handler.hpp"
#include "../network/socket/socket.hpp"
#include "../network/epoll/epoll.hpp"
#include "../network/connection/connection.hpp"
#include "../network/recv_send/recv_send.hpp"
#include "../network/thread_pool/thread_pool.hpp"

static volatile bool g_running = true;

extern "C" void handle_signal(int sig) {
    (void)sig;
    g_running = false;
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
    //fpr
    network::Socket listen_sock;

    if(listen_sock.create_server(ip,port)!=0){
        //fpr
        return 1; 
    }
    listen_sock.set_nonblock();


    network::Epoll epoll_main(1024);
    epoll_main.add(listen_sock.fd(),EPOLLIN);
    //fpr

     ThreadPool pool;
     pool.start([](Task task)->TaskResult{

        return handler::handle_task(task);
     });
     //fpr

     recv_send rs_tool;

     while(g_running){

        {
            TaskResult result;
            while(pool.try_get_result(result)){//处理任务结果循环

                if(result.need_close){

                    epoll_main.del(result.fd);
                    connection_remove(result.fd);
                    //fpr 移除完成  
                    continue;

                }
                Connection* conn=connection_get(result.fd);
                if(conn==NULL){
                    //fpr连接关闭
                    continue;
                }
                if(result.data.empty()){
                    //fpr
                }else{
                    if(!rs_tool.AppendSendBuffer(*conn, result.data.data(), result.data.size())){
                        //fpr
                    epoll_main.del(result.fd);
                    connection_remove(result.fd);
                    continue; 
                    }
                    epoll_main.mod(result.fd,EPOLLIN|EPOLLOUT);
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
                epoll_main.del(ev.data.fd);
                connection_remove(ev.data.fd);
                fprintf(stdout, "[close] fd=%d (EPOLLERR|EPOLLHUP)\n", ev_data_fd);
                continue;
            }

            // 可读
            if (ev.events & EPOLLIN) {
                int ret = rs_tool.Recv(*conn); 
                if (ret <= 0) {
                    // ret == 0  → 对端关闭 (EOF)
                    // ret <  0  → 接收错误
                    epoll_main.del(ev_data_fd);
                    connection_remove(ev_data_fd);
                    fprintf(stdout, "[close] fd=%d (recv=%d)\n", ev_data_fd, ret);
                    continue;
                }

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