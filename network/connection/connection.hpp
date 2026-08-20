#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <openssl/ssl.h>   // SSL*

const int RECV_BUFFER_SIZE = 8192;
const int SEND_BUFFER_SIZE = 8192;

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
    int recv_length() const { return recv_len_; }
    void set_recv_length(int len) { recv_len_ = len; }

    char* send_buffer() { return send_buf_.data(); }
    int send_length() const { return send_len_; }
    void set_send_length(int len) { send_len_ = len; }

    // 上次收到数据的时间戳(毫秒), 用于服务端心跳超时判定。
    uint64_t last_active() const { return last_active_; }
    void set_last_active(uint64_t ms) { last_active_ = ms; }

private:
    int fd_;
    SSL* ssl_;                // TLS 会话对象(每个连接一个); 未启用 TLS 时为 nullptr
    TlsState tls_state_;      // TLS 握手状态
    int userid_;
    uint64_t last_active_;

    std::array<char, RECV_BUFFER_SIZE> recv_buf_;
    int recv_len_;

    std::array<char, SEND_BUFFER_SIZE> send_buf_;
    int send_len_;
};


#define MAX_CONN 1024


void connection_init();

int connection_add(int fd);

void connection_remove(int fd);

Connection* connection_get(int fd);
