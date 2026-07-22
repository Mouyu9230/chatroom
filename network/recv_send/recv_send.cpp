#include "recv_send.hpp"
#include "../connection/connection.hpp"
#include "../protocol/protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

int recv_send::Recv(Connection& conn) {
    char* buf     = conn.recv_buffer();
    int   buf_len = conn.recv_length();
    int   cap     = RECV_BUFFER_SIZE;

    if (buf_len >= cap) {
        //已满，无法继续读取
        return -1;
    }

    ssize_t n = read(conn.fd(), buf + buf_len, cap - buf_len);
    if(n > 0){
        conn.set_recv_length(buf_len + n);
    }else if(n == 0){
        // 连接关闭 
        return 0;
    }else{
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;   // 非阻塞下无数据可读，不算错误
        }
        return -1;
    }

    return n;
}

int recv_send::Send(Connection& conn) {
    char* buf     = conn.send_buffer();
    int   buf_len = conn.send_length();

    if (buf_len <= 0) {
        return 0;   // 无数据可发
    }

    ssize_t n = write(conn.fd(), buf, buf_len);
    if (n > 0) {
        // 已发送 n 字节，将剩余数据移到缓冲区头部
        int remaining = buf_len - n;
        if (remaining > 0 && n > 0) {
            memmove(buf, buf + n, remaining);
        }
        conn.set_send_length(remaining);
    } else if (n == 0) {
        // 连接关闭 
        return 0;
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;   // 非阻塞下发送缓冲区满，稍后重试
        }
        return -1;
    }

    return n;
}
bool HasCompletePacket(Connection& conn){
    int buf_len=conn.recv_length();
    
    if(buf_len<=0||buf_len<=sizeof(protocol::packet_header)){
        return false;
    }


    
}
