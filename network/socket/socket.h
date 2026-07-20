#ifndef SOCKET_H
#define SOCKET_H

#include <netinet/in.h>

// 创建 TCP Socket
int socket_create(void);

// 创建监听 Socket（backlog 固定为 SOMAXCONN）
int socket_create_server(const char *ip, int port);

// 连接服务器
int socket_connect(int sockfd, const char *ip, int port);

// 接收客户端连接
int socket_accept(int listenfd, struct sockaddr_in *client);

// 设置阻塞/非阻塞
int socket_set_nonblock(int fd);
int socket_set_block(int fd);

// Socket 选项
int socket_set_reuseaddr(int fd);
int socket_set_reuseport(int fd);
int socket_set_keepalive(int fd);
int socket_set_nodelay(int fd);

// 地址信息
int socket_get_localaddr(int fd, struct sockaddr_in *addr);
int socket_get_peeraddr(int fd, struct sockaddr_in *addr);

const char *socket_get_ip(const struct sockaddr_in *addr);
int socket_get_port(const struct sockaddr_in *addr);


void socket_close(int fd);

#endif