#pragma once

#include <netinet/in.h>

namespace network {


class Socket {
public:

    Socket();

    /// 包装一个已有的文件描述符（接管所有权）
    explicit Socket(int fd);


    ~Socket();


    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;





    int create_server(const char* ip, int port);


    int accept();




    int connect(const char* ip, int port);

    int set_nonblock();
    int set_reuseaddr();
    int set_reuseport();
    int set_keepalive();
    int set_nodelay();



    int get_localaddr(struct sockaddr_in* addr);
    int get_peeraddr(struct sockaddr_in* addr);



    int fd() const { return fd_; }
    void close_socket();

private:
    int fd_;
};

} // namespace net
