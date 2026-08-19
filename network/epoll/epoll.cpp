#include "epoll.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace network {



Epoll::Epoll(int max_events)
    : epoll_fd_(epoll_create1(EPOLL_CLOEXEC))
    , max_events_(max_events)
    , events_(NULL)
{
    if (epoll_fd_ < 0) {
        perror("epoll_create1");
        return;
    }

    // 分配就绪事件数组
    events_ = new struct epoll_event[max_events_];
}

Epoll::~Epoll() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }

    delete[] events_;
    events_ = NULL;
}



int Epoll::add(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl add");
        return -1;
    }

    return 0;
}

int Epoll::mod(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        perror("epoll_ctl mod");
        return -1;
    }

    return 0;
}

int Epoll::del(int fd) {

    struct epoll_event ev;
    ev.events = 0;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev) < 0) {
        perror("epoll_ctl del");
        return -1;
    }

    return 0;
}



int Epoll::wait(int timeout_ms) {
    int n = epoll_wait(epoll_fd_, events_, max_events_, timeout_ms);
    if (n < 0) {
        int e = errno;
        perror("epoll_wait");
        errno = e;   // perror 会改写 errno, 还原以便调用方判断 EINTR
        return -1;
    }

    return n;
}


} // namespace network
