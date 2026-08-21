#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <openssl/ssl.h>   // SSL*

// 接收/发送缓冲的初始大小(非上限): 会按需自动增长。
//   - 接收缓冲: 依据包头声明的 body_len 增长(上限 = MAX_BODY_LEN + 包头, 见 recv_send.cpp)。
//   - 发送缓冲: 依据待发数据增长(上限 = MAX_SEND_BUFFER_SIZE, 防积压 OOM)。
const int RECV_BUFFER_SIZE = 8192;
const int SEND_BUFFER_SIZE = 8192;
const size_t MAX_SEND_BUFFER_SIZE = 64u * 1024u * 1024u;   // 发送缓冲上限 64MB

/// TLS 连接状态: 非阻塞握手下, 连接必须先完成握手才能收发业务包
enum class TlsState : uint8_t {
    HANDSHAKE = 0,   // accept 后、握手完成前
    ACTIVE    = 1,   // 握手完成, 可正常收发
};

/// 单个连接的数据与缓冲区
class Connection {
public:
    explicit Connection(int fd);
    // RAII: 析构负责 SSL_free + close(fd), 并置 -1 防 double-close。
    // 释放/关闭不再由调用方手动做, 统一交给析构(见 connection_remove)。
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;



    int fd() const { return fd_; }

    SSL* ssl() const { return ssl_; }
    void set_ssl(SSL* ssl) { ssl_ = ssl; }

    TlsState tls_state() const { return tls_state_; }
    void set_tls_state(TlsState s) { tls_state_ = s; }

    int user_id() const { return userid_; }
    void set_user_id(int uid) { userid_ = uid; }

    char* recv_buffer() { return recv_buf_.data(); }
    const char* recv_buffer() const { return recv_buf_.data(); }
    int recv_length() const { return recv_len_; }
    void set_recv_length(int len) { recv_len_ = len; }

    // 接收缓冲当前可用容量(字节); 不足时调用 ensure_recv_capacity 增长
    size_t recv_capacity() const { return recv_buf_.size(); }
    void ensure_recv_capacity(size_t n) {
        if (recv_buf_.size() < n) {
            size_t new_size = recv_buf_.size();
            if (new_size == 0) new_size = RECV_BUFFER_SIZE;
            while (new_size < n) new_size *= 2;
            recv_buf_.resize(new_size);
        }
    }

    char* send_buffer() { return send_buf_.data(); }
    int send_length() const { return send_len_; }
    void set_send_length(int len) { send_len_ = len; }

    // 发送缓冲当前可用容量(字节); 增长到至少 n, 超过上限返回 false(调用方应关闭连接)
    size_t send_capacity() const { return send_buf_.size(); }
    bool ensure_send_capacity(size_t n) {
        if (send_buf_.size() < n) {
            if (n > MAX_SEND_BUFFER_SIZE) return false;
            size_t new_size = send_buf_.size();
            if (new_size == 0) new_size = SEND_BUFFER_SIZE;
            while (new_size < n) new_size *= 2;
            send_buf_.resize(new_size);
        }
        return true;
    }

    // 上次收到数据的时间戳(毫秒), 用于服务端心跳超时判定。
    uint64_t last_active() const { return last_active_; }
    void set_last_active(uint64_t ms) { last_active_ = ms; }

private:
    int fd_;
    SSL* ssl_;                // TLS 会话对象(每个连接一个); 未启用 TLS 时为 nullptr
    TlsState tls_state_;      // TLS 握手状态
    int userid_;
    uint64_t last_active_;

    std::vector<char> recv_buf_;   // 按需自动增长
    int recv_len_;

    std::vector<char> send_buf_;   // 按需自动增长
    int send_len_;
};


#define MAX_CONN 1024


void connection_init();

int connection_add(int fd);

void connection_remove(int fd);

Connection* connection_get(int fd);
