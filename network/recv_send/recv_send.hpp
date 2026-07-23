#pragma once

#include <cstddef>

class Connection;

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