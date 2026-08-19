#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

const int RECV_BUFFER_SIZE = 8192;
const int SEND_BUFFER_SIZE = 8192;

/// 单个连接的数据与缓冲区
class Connection {
public:
    explicit Connection(int fd);
    ~Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;



    int fd() const { return fd_; }

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
