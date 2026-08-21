#ifndef client.h   
#define client.h  

#include <iostream>
#include <ostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 2100

using namespace std;

#define BUF_SIZE 4096

struct ftp_client
{
    int ctrl_fd;
    int data_fd;
    std::string server_ip;
    int server_port;
    bool is_login;
    long rest_offset;//断点续传偏移量
};

int connect_server(ftp_client* cli);

void send_cmd(ftp_client* cli, const std::string& cmd);
string recv_resp(ftp_client* cli);
int handle_command(ftp_client* cli, const string& cmd);

void handle_pasv(ftp_client* cli);
void handle_list(ftp_client* cli);
void handle_retr(ftp_client* cli, const std::string& file);
void handle_stor(ftp_client* cli, const std::string& file);
void handle_rest(ftp_client* cli, const std::string& arg);



#endif