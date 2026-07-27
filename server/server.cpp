#include <signal.h>
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

        




     }













}