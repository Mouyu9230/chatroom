#ifndef server.h
#define server.h


#include <csignal>
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>
#include <sstream>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <dirent.h>
#include <fcntl.h>

#define SERVER_PORT 2100
#define BUF_SIZE 4096
#define CLIENT_NAME "1"
#define CLIENT_PASS "1"

// FTP session状态
typedef struct
{
    int ctrl_fd;//控制连接柄
    int data_listen_fd;//监听柄
    int data_fd;//数据连接柄
    int pasv_port;//开放端口
    bool is_right_user;
    bool is_login;
    char cwd[512];//server当前目录路径
    long rest_offset;//断点续传偏移量(REST命令设置,下次RETR/STOR生效)

}ftp_session;


using namespace std;

//工具函数
int create_server_socket(int port);
void send_response(int fd, const string& msg);

//FTP功能模块声明
void client_handler(int client_fd);
int handle_command(ftp_session* sess, const string& cmd);

void handle_user(ftp_session* sess, const string& arg);
void handle_pass(ftp_session* sess, const string& arg);
void handle_quit(ftp_session* sess);

void handle_pasv(ftp_session* sess);
void handle_list(ftp_session* sess);
void handle_retr(ftp_session* sess, const string& file);
void handle_stor(ftp_session* sess, const string& file);
void handle_rest(ftp_session* sess, const string& arg);
void handle_size(ftp_session* sess, const string& file);
void sigint_handler(int x);





#endif