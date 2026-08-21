#pragma once

#include <cstddef>

class Connection;

// 返回约定:
//   >0 成功 n 字节; 0 EOF/无可发; -1 致命错误
//   -2 无数据可读(EAGAIN / TLS WANT_READ)
//   -3 需等待另一方向事件(TLS WANT_WRITE / WANT_READ), 调用方应重挂 EPOLLIN|EPOLLOUT
//
// 缓冲策略: Connection 的收发缓冲按需自动增长(见 connection.hpp),
//   - Recv 依据包头声明的 body_len 增长接收缓冲, 可整包接收大尺寸数据(≤ MAX_BODY_LEN);
//   - AppendSendBuffer 按需增长发送缓冲(≤ MAX_SEND_BUFFER_SIZE), 大响应由 EPOLLOUT 分多次排空。
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