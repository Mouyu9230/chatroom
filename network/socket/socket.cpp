#include "socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// 创建 TCP Socket
int socket_create(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    return fd;
}

// 创建监听 Socket
int socket_create_server(const char *ip, int port)
{
    int fd = socket_create();
    if (fd < 0) return -1;

    // 设置地址复用，避免 TIME_WAIT 导致 bind 失败
    socket_set_reuseaddr(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (ip == NULL || ip[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
            perror("inet_pton");
            close(fd);
            return -1;
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

// 连接服务器
int socket_connect(int sockfd, const char *ip, int port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return -1;
    }

    return 0;
}

// 接收客户端连接
int socket_accept(int listenfd, struct sockaddr_in *client)
{
    socklen_t len = sizeof(struct sockaddr_in);

    int connfd = accept(listenfd, (struct sockaddr *)client, &len);
    if (connfd < 0) {
        perror("accept");
        return -1;
    }

    return connfd;
}

// 设置非阻塞
int socket_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 设置阻塞
int socket_set_block(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

// Socket 选项：地址复用
int socket_set_reuseaddr(int fd)
{
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

// Socket 选项：端口复用
int socket_set_reuseport(int fd)
{
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
}

// Socket 选项：TCP keep-alive
int socket_set_keepalive(int fd)
{
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
}

// Socket 选项：禁用 Nagle 算法
int socket_set_nodelay(int fd)
{
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

// 获取本地地址
int socket_get_localaddr(int fd, struct sockaddr_in *addr)
{
    socklen_t len = sizeof(struct sockaddr_in);
    if (getsockname(fd, (struct sockaddr *)addr, &len) < 0) {
        perror("getsockname");
        return -1;
    }
    return 0;
}

// 获取对端地址
int socket_get_peeraddr(int fd, struct sockaddr_in *addr)
{
    socklen_t len = sizeof(struct sockaddr_in);
    if (getpeername(fd, (struct sockaddr *)addr, &len) < 0) {
        perror("getpeername");
        return -1;
    }
    return 0;
}

// 从 sockaddr_in 获取 IP 字符串
const char *socket_get_ip(const struct sockaddr_in *addr)
{
    return inet_ntoa(addr->sin_addr);
}

// 从 sockaddr_in 获取端口号（主机字节序）
int socket_get_port(const struct sockaddr_in *addr)
{
    return ntohs(addr->sin_port);
}

// 关闭 Socket
void socket_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}
