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
#include "../network/socket/socket.hpp"
#include "../network/epoll/epoll.hpp"
#include "../network/connection/connection.hpp"
#include "../network/recv_send/recv_send.hpp"
#include "../network/thread_pool/thread_pool.hpp"

static volatile bool g_running = true;

int main(int argc,char* argv[]){

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
    if(listen_sock.create_server(ip,port)!=0){
        //fpr
        return 1;
    }

    network::Epoll epoll_main(1024);
    epoll_main.add(listen_sock.fd(),EPOLLIN);
    //fpr

     ThreadPool pool;
     pool.start([](Task task)->TaskResult{

        //根据tasktype分发任务 返回result

        return TaskResult{task.fd,std::move(task.data),false};
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

            if(ev_data_fd==listen_sock.fd()){//处理监听事件



            }




        }










     }













}