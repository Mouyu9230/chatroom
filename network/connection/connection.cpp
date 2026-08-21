#include "connection.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>




Connection::Connection(int fd)
    : fd_(fd)
    , ssl_(nullptr)
    , tls_state_(TlsState::HANDSHAKE)
    , userid_(0)
    , last_active_(0)
    , recv_buf_()
    , recv_len_(0)
    , send_buf_()
    , send_len_(0)
{
    // 初始缓冲大小; 超出时由 recv_send 按需自动增长(见 ensure_*_capacity)
    recv_buf_.resize(RECV_BUFFER_SIZE);
    send_buf_.resize(SEND_BUFFER_SIZE);
}

Connection::~Connection() {
    // RAII: 统一在这里释放资源。ssl_ 先 free, 再关 fd; 各自置空/-1 防重复释放。
    if (ssl_ != nullptr) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}



static Connection* conn_pool[MAX_CONN];

void connection_init() {
    memset(conn_pool, 0, sizeof(conn_pool));
}

int connection_add(int fd) {
    if (fd < 0 || fd >= MAX_CONN) {
        fprintf(stderr, "connection_add: fd %d out of range [0, %d)\n", fd, MAX_CONN);
        return -1;
    }

    if (conn_pool[fd] != NULL) {
        fprintf(stderr, "connection_add: fd %d already exists\n", fd);
        return -1;
    }

    Connection* conn = new Connection(fd);
    if (conn == NULL) {
        perror("new Connection");
        return -1;
    }

    conn_pool[fd] = conn;
    return 0;
}

void connection_remove(int fd) {
    if (fd < 0 || fd >= MAX_CONN) {
        return;
    }

    Connection* conn = conn_pool[fd];
    if (conn == NULL) {
        return;
    }

    // 资源(SSL + fd)统一由 Connection 析构释放, 此处只负责删除对象并清池
    delete conn;
    conn_pool[fd] = NULL;
}

Connection* connection_get(int fd) {

    if (fd < 0 || fd >= MAX_CONN) {
        return NULL;
    }
    return conn_pool[fd];
}
