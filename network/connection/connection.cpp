#include "connection.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>




Connection::Connection(int fd)
    : fd_(fd)
    , userid_(0)
    , last_active_(0)
    , recv_buf_()
    , recv_len_(0)
    , send_buf_()
    , send_len_(0)
{
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

    // 清理连接持有的fd
    if (conn->fd() >= 0) {
        close(conn->fd());
    }

    delete conn;
    conn_pool[fd] = NULL;
}

Connection* connection_get(int fd) {

    if (fd < 0 || fd >= MAX_CONN) {
        return NULL;
    }
    return conn_pool[fd];
}
