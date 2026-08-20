#include "recv_send.hpp"
#include "../connection/connection.hpp"
#include "../../protocol/protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include <openssl/err.h>

// 返回值约定(与既有语义兼容):
//   >0  读/写成功 n 字节; 0 EOF(读)或无可发数据(写); -1 致命错误
//   -2  无数据可读(EAGAIN / TLS WANT_READ)
//   -3  本次操作需要等待另一方向事件(TLS WANT_WRITE / WANT_READ), 需重挂事件
int recv_send::Recv(Connection& conn) {//读数据到接收缓冲区
    char* buf     = conn.recv_buffer();
    int   buf_len = conn.recv_length();
    int   cap     = RECV_BUFFER_SIZE;

    if (buf_len >= cap) {
        //已满，无法继续读取
        return -1;
    }

    SSL* ssl = conn.ssl();
    if (ssl == nullptr) {
        // 非 TLS 路径
        ssize_t n = read(conn.fd(), buf + buf_len, cap - buf_len);
        if (n > 0) {
            conn.set_recv_length(buf_len + n);
            return (int)n;
        }
        if (n == 0) {
            // 连接关闭 (EOF)
            return 0;
        }
        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 非阻塞下无数据可读: 正常, 不是错误也不该关闭 (与 EOF 的 0 区分)
            return -2;
        }
        return -1;
    }

    // TLS 路径: SSL_read 解密后的明文直接写入接收缓冲区, 上层 framing 逻辑不变
    int n = SSL_read(ssl, buf + buf_len, cap - buf_len);
    if (n > 0) {
        conn.set_recv_length(buf_len + n);
        return n;
    }
    switch (SSL_get_error(ssl, n)) {
        case SSL_ERROR_WANT_READ:
            return -2;                              // 暂无解密数据, 等待可读
        case SSL_ERROR_WANT_WRITE:
            return -3;                              // TLS 需向外刷数据(罕见), 等可写
        case SSL_ERROR_ZERO_RETURN:
            return 0;                               // close_notify / EOF
        case SSL_ERROR_SYSCALL:
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
            return -1;
        default:
            return -1;                              // 协议错误
    }
}

int recv_send::Send(Connection& conn) {//从发送缓冲区发送数据
    char* buf     = conn.send_buffer();
    int   buf_len = conn.send_length();

    if (buf_len <= 0) {
        return 0;   // 无数据可发
    }

    SSL* ssl = conn.ssl();
    if (ssl == nullptr) {
        // 非 TLS 路径
        ssize_t n = write(conn.fd(), buf, buf_len);
        if (n > 0) {
            // 已发送 n 字节，将剩余数据移到缓冲区头部
            int remaining = buf_len - (int)n;
            if (remaining > 0) {
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
        return (int)n;
    }

    // TLS 路径
    int n = SSL_write(ssl, buf, buf_len);
    if (n > 0) {
        // SSL_write 成功即明文已被加密写入底层 socket(未开 PARTIAL_WRITE 时 n==buf_len)
        int remaining = buf_len - n;
        if (remaining > 0) {
            memmove(buf, buf + n, remaining);
        }
        conn.set_send_length(remaining);
        return n;
    }
    switch (SSL_get_error(ssl, n)) {
        case SSL_ERROR_WANT_WRITE:
            // 明文仍保留在 SSL 内部待刷: 绝不能移动 send_buffer, 等下次 EPOLLOUT 重试
            return 0;
        case SSL_ERROR_WANT_READ:
            return -3;                              // TLS 需读数据(罕见), 等可读
        default:
            return -1;
    }
}
//校验数据包合法性
bool recv_send::HasCompletePacket(Connection& conn) {//！！未处理丢包（后续加序列号）残缺问题（ 魔数检验处返回错误）
    int buf_len=conn.recv_length();
    if (buf_len<(int)sizeof(protocol::packet_header)) {
        return false;
    }

    protocol::packet_header* hdr=reinterpret_cast<protocol::packet_header*>(conn.recv_buffer());

    // 校验魔数
    if (hdr->magic!=protocol::MAGIC_NUM) {
        return false;
    }

    if (hdr->body_len>protocol::MAX_BODY_LEN) {
        return false;
    }

    int total_len = sizeof(protocol::packet_header)+hdr->body_len;
    return buf_len >= total_len;
}

int recv_send::FetchPacket(Connection& conn, char* packet, size_t& packet_len) {//从缓冲区拿数据包用char* packet传出
    if(!HasCompletePacket(conn)){
        packet_len = 0;
        return -1;
    }

    protocol::packet_header* hdr = reinterpret_cast<protocol::packet_header*>(conn.recv_buffer());
    int total_len = sizeof(protocol::packet_header) + hdr->body_len;
    int buf_len = conn.recv_length();

    // 将完整数据包拷贝到 packet 中
    memcpy(packet, conn.recv_buffer(), total_len);
    packet_len = total_len;

    // 从接收缓冲区中移除已拷贝的数据
    {
        int remaining = buf_len - total_len;
        if (remaining > 0) {
            memmove(conn.recv_buffer(), conn.recv_buffer() + total_len, remaining);
        }
        conn.set_recv_length(remaining);
    }

    return 0;
}




bool recv_send::AppendSendBuffer(Connection& conn, const char* data, size_t len) {//数据写入发送缓冲区
    int buf_len = conn.send_length();
    if (buf_len + (int)len > SEND_BUFFER_SIZE) {
        return false;   // 缓冲区空间不足
    }

    memcpy(conn.send_buffer() + buf_len, data, len);
    conn.set_send_length(buf_len + len);
    return true;
}