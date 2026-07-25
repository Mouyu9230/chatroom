#include "socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <cstdio>

namespace network {



Socket::Socket()
    :fd_(socket(AF_INET, SOCK_STREAM, 0)){
        if(fd_< 0){
            perror("socket");
    }
}

Socket::Socket(int fd)
    :fd_(fd)
{
    //nope
}

Socket::~Socket() {
    close_socket();
}



int Socket::create_server(const char* ip, int port) {
    set_reuseaddr();

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (ip == NULL || ip[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
            perror("inet_pton");
            return -1;
        }
    }

    if (bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }

    if (listen(fd_, SOMAXCONN) < 0) {
        perror("listen");
        return -1;
    }

    return 0;
}

int Socket::accept() {
    struct sockaddr_in client;//虽然通过accept拿到了对端地址信息但没有被传出，该函数作用仅为获取连接fd,要获取就调用getpeeraddr
    socklen_t len = sizeof(client);

    int connfd = ::accept(fd_, (struct sockaddr*)&client, &len);
    if (connfd < 0) {
        perror("accept");
        return -1;
    }

    return connfd;
}


int Socket::connect(const char* ip, int port) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        return -1;
    }

    if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
    }

    return 0;
}


int Socket::set_nonblock() {
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
}

int Socket::set_reuseaddr() {
    int opt = 1;
    return setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

int Socket::set_reuseport() {
    int opt = 1;
    return setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
}

int Socket::set_keepalive() {
    int opt = 1;
    return setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
}

int Socket::set_nodelay() {
    int opt = 1;
    return setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}



int Socket::get_localaddr(struct sockaddr_in* addr){
    socklen_t len = sizeof(*addr);
    return getsockname(fd_, (struct sockaddr*)addr, &len);
}

int Socket::get_peeraddr(struct sockaddr_in* addr){
    socklen_t len = sizeof(*addr);
    return getpeername(fd_, (struct sockaddr*)addr, &len);
}



void Socket::close_socket(){
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}




}
