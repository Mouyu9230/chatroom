#pragma once

#include <cstddef>

class Connection;

// 返回约定:
//   >0 成功 n 字节; 0 EOF/无可发; -1 致命错误
//   -2 无数据可读(EAGAIN / TLS WANT_READ)
//   -3 需等待另一方向事件(TLS WANT_WRITE / WANT_READ), 调用方应重挂 EPOLLIN|EPOLLOUT
class recv_send
{
public:


     int Recv(Connection& conn);


     int Send(Connection& conn);

    //是否存在完整数据包
     bool HasCompletePacket(Connection& conn);

    //取出一个完整数据包
     int FetchPacket(Connection& conn,
                           char* packet,
                           size_t& packet_len);

    //向发送缓冲区追加数据
     bool AppendSendBuffer(Connection& conn,const char* data,size_t len);

private:

};