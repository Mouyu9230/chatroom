#pragma once

#include <sys/epoll.h>

#include <cstdint>

namespace network {



class Epoll {
public:


    explicit Epoll(int max_events = 1024);
    //explicit 禁止构造函数被隐式转换
    //如果一个构造函数只接受一个参数（或者除默认参数外只接受一个），编译器会把它视为如何从参数类型转换到当前类类型的说明。
    //这导致在出现int而期望类型为Epoll时编译器会new一个构造函数参数为那个int的Epoll对象替代之


    ~Epoll();


    Epoll(const Epoll&) = delete;
    Epoll& operator=(const Epoll&) = delete;




    int add(int fd, uint32_t events);


    int mod(int fd, uint32_t events);

    int del(int fd);

    int wait(int timeout_ms = -1);



    int fd() const { return epoll_fd_; }

 
    const struct epoll_event* events() const { return events_; }

private:

    int epoll_fd_;
    int max_events_;
    struct epoll_event* events_;

};

} // namespace network
