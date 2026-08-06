#include "client.hpp"

#include "../network/socket/socket.hpp"
#include "../protocol/(!)message/message.pb.h"
#include "../handler/handler.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ============================================================
//  客户端实现
//
//  用法: client [port] [ip]   (默认 127.0.0.1:2100)
//
//  基础版: 阻塞式 TCP 客户端, 连接后进入交互命令行,
//  用 login / chat / heartbeat 三个命令逐个触发
//  handler 中对应的三个处理函数并打印响应。
// ============================================================

namespace client {
namespace {

// 阻塞读满 n 字节, 成功返回 true
bool recv_exact(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
        } else if (r == 0) {
            return false;  // 对端关闭
        } else if (errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// 阻塞发满 n 字节, 成功返回 true (MSG_NOSIGNAL 避免 SIGPIPE 终止进程)
bool send_exact(int fd, const char* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (r > 0) {
            sent += static_cast<size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);  // 发送缓冲区暂满, 稍后重试
            continue;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<char> build_packet(uint8_t type, const google::protobuf::Message& body) {
    std::string body_str;
    body.SerializeToString(&body_str);

    std::vector<char> packet;
    packet.resize(sizeof(protocol::packet_header) + body_str.size());
 
    auto* hdr = reinterpret_cast<protocol::packet_header*>(packet.data());
    hdr->magic    = protocol::MAGIC_NUM;
    hdr->ver      = 1;
    hdr->type     = type;
    hdr->body_len = static_cast<uint32_t>(body_str.size());

    if (!body_str.empty()) {
        std::memcpy(packet.data() + sizeof(protocol::packet_header),
                    body_str.data(), body_str.size());
    }
    return packet;
}

bool send_packet(int fd, const std::vector<char>& packet) {
    return send_exact(fd, packet.data(), packet.size());
}

std::vector<char> read_packet(int fd) {
    protocol::packet_header hdr;
    if (!recv_exact(fd, reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        return {};
    }
    if (hdr.magic != protocol::MAGIC_NUM) {
        fprintf(stderr, "[client] bad magic: 0x%04x\n", hdr.magic);
        return {};
    }
    if (hdr.body_len > protocol::MAX_BODY_LEN) {
        fprintf(stderr, "[client] body too large: %u\n", hdr.body_len);
        return {};
    }

    std::vector<char> packet(sizeof(hdr) + hdr.body_len);
    std::memcpy(packet.data(), &hdr, sizeof(hdr));
    if (hdr.body_len > 0) {
        if (!recv_exact(fd, packet.data() + sizeof(hdr), hdr.body_len)) {
            return {};
        }
    }
    return packet;
}

bool request(int fd, uint8_t req_type, const google::protobuf::Message& req,
             uint8_t expect_type, std::vector<char>& resp_packet) {
    std::vector<char> packet = build_packet(req_type, req);
    if (!send_exact(fd, packet.data(), packet.size())) {
        fprintf(stderr, "[client] send failed\n");
        return false;
    }

    for (;;) {
        std::vector<char> pkt = read_packet(fd);
        if (pkt.empty()) {
            fprintf(stderr, "[client] read failed / connection closed\n");
            return false;
              }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type == expect_type) {
            resp_packet = std::move(pkt);
            return true;
        }
        // 其它类型(如未来的系统通知)不是本请求的响应, 打印后继续等
        fprintf(stdout, "[client] <skip> unexpected type=0x%02x body_len=%u\n",
                hdr->type, hdr->body_len);
    }
}

}  // namespace client




static void print_help() {
    fprintf(stdout,
            "commands:\n"
            "  login <username> <password>  send login, print LoginResponse\n"
            "  chat  <receiver_id> <text>   send chat, print ChatResponse\n"
            "  heartbeat                    send heartbeat, print HeartbeatResp\n"
            "  help                         show this help\n"
            "  quit / exit                  close connection and exit\n");
}













int main(int argc, char* argv[]) {
    const char* ip = "127.0.0.1";
    int port = 2100;
    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) ip = argv[2];

    network::Socket sock;
    if (sock.connect(ip, port) != 0) {
        fprintf(stderr, "[client] connect %s:%d failed\n", ip, port);
        return 1;
    }
    int fd = sock.fd();
    fprintf(stdout, "[client] connected to %s:%d (fd=%d)\n", ip, port, fd);
    print_help();

 
    char line[1024];
    while(1){

        fprintf(stdout, "\nclient> "); 
        if (fgets(line, sizeof(line), stdin) == nullptr) {
            break;  // EOF
        }
        line[strcspn(line, "\n")] = '\0';  // 去掉换行

        char cmd[64] = {0};
        if (sscanf(line, "%63s", cmd) != 1) {
            continue;
        }


//指令判断/处理-------


        if (strcmp(cmd, "help") == 0) {

            print_help();

        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {

            break;

        } else if (strcmp(cmd, "login") == 0) {

            char username[128] = {0}, password[128] = {0};
            if (sscanf(line, "%*s %127s %127s", username, password) != 2) {
                fprintf(stderr, "usage: login <username> <password>\n");
                continue;
            }
            protocol::LoginRequest req;
            req.set_username(username);
            req.set_password(password);

            std::vector<char> resp;
            if (client::request(fd, protocol::MSG_TYPE_LOGIN_REQ, req,
                                protocol::MSG_TYPE_LOGIN_RESP, resp)) {
                auto* hdr = reinterpret_cast<const protocol::packet_header*>(resp.data());
                protocol::LoginResponse rsp;
                if (rsp.ParseFromArray(resp.data() + sizeof(*hdr), hdr->body_len)) {
                    fprintf(stdout, "[login] err=%d userid=%u\n",
                            static_cast<int>(rsp.err()), rsp.userid());
                } else {
                    fprintf(stderr, "[login] bad response body\n");
                }
            }

        } else if (strcmp(cmd, "chat") == 0) {
            uint32_t receiver_id = 0;
            char content[4096] = {0};
            if (sscanf(line, "%*s %u %4095[^\n]", &receiver_id, content) < 1) {
                fprintf(stderr, "usage: chat <receiver_id> <text>\n");
                continue;
            }
            protocol::ChatRequest req;
            req.set_receiver_id(receiver_id);
            req.set_content(content);

            std::vector<char> resp;
            if (client::request(fd, protocol::MSG_TYPE_CHAT_REQ, req,
                                protocol::MSG_TYPE_CHAT_RESP, resp)) {
                auto* hdr = reinterpret_cast<const protocol::packet_header*>(resp.data());
                protocol::ChatResponse rsp;
                if (rsp.ParseFromArray(resp.data() + sizeof(*hdr), hdr->body_len)) {
                    fprintf(stdout, "[chat] err=%d msg_id=%u\n",
                            static_cast<int>(rsp.err()), rsp.msg_id());
                } else {
                    fprintf(stderr, "[chat] bad response body\n");
                }
            }

        } else if (strcmp(cmd, "heartbeat") == 0) {
            protocol::HeartbeatReq req;
            std::vector<char> resp;
            if (client::request(fd, protocol::MSG_TYPE_HEARTBEAT_REQ, req,
                                protocol::MSG_TYPE_HEARTBEAT_RESP, resp)) {
                fprintf(stdout, "[heartbeat] ok\n");
            }
             
        } else {
            fprintf(stderr, "unknown command: %s (try 'help')\n", cmd);
        }


//指令处理模块结束


    }//主循环结束

    ::close(fd);
    fprintf(stdout, "\n[client] closed.\n");
    return 0;
}
