#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../protocol/protocol.hpp"
#include "../network/socket/socket.hpp"
#include "../network/epoll/epoll.hpp"
#include "../network/connection/connection.hpp"
#include "../network/recv_send/recv_send.hpp"
#include "../network/thread_pool/thread_pool.hpp"

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











}