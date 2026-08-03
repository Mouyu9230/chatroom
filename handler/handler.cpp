#include "handler.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../protocol/protocol.hpp"
#include "../protocol/(!)message/message.pb.h"

namespace handler {
namespace {

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



TaskResult on_heartbeat(const Task& task) {
    protocol::HeartbeatResp body;
    return TaskResult{task.fd, build_packet(protocol::MSG_TYPE_HEARTBEAT_RESP, body), false};
}


TaskResult on_login(const Task& task, const char* body, size_t body_len) {

    protocol::LoginRequest req;

    if (!req.ParseFromArray(body, static_cast<int>(body_len))) {

        //失败断开连接
        return TaskResult{task.fd, {}, true};

    }

    fprintf(stdout, "[handler] login: user=%s\n", req.username().c_str());


//demo---
    protocol::LoginResponse resp;
    resp.set_err(protocol::ERR_SUCCESS);
    resp.set_userid(10086);
//-------


    return TaskResult{task.fd, build_packet(protocol::MSG_TYPE_LOGIN_RESP, resp), false};
}


TaskResult on_chat(const Task& task, const char* body, size_t body_len) {
    protocol::ChatRequest req;

    if (!req.ParseFromArray(body, static_cast<int>(body_len))) {
        return TaskResult{task.fd, {}, true};
    }

    fprintf(stdout, "[handler] chat: user=%u -> %u: %s\n",
            task.user_id, req.receiver_id(), req.content().c_str());

//demo--
    protocol::ChatResponse resp;
    resp.set_err(protocol::ERR_SUCCESS);
    resp.set_msg_id(1); 
//------

    return TaskResult{task.fd, build_packet(protocol::MSG_TYPE_CHAT_RESP, resp), false};
}

}   


// ------------------------------------------------------------
TaskResult handle_task(const Task& task) {

    if (task.data.size() < sizeof(protocol::packet_header)) {
        fprintf(stderr, "[handler] packet too small: %zu\n", task.data.size());
        return TaskResult{task.fd, {}, true};
    }

    const auto* hdr = reinterpret_cast<const protocol::packet_header*>(task.data.data());

    
    if (hdr->magic != protocol::MAGIC_NUM) {
        fprintf(stderr, "[handler] bad magic: 0x%04x\n", hdr->magic);
        return TaskResult{task.fd, {}, true};
    }

    const char* body     = task.data.data() + sizeof(protocol::packet_header);
    const size_t body_len = task.data.size() - sizeof(protocol::packet_header);


    switch (hdr->type) {
        case protocol::MSG_TYPE_HEARTBEAT_REQ:
            return on_heartbeat(task);

        case protocol::MSG_TYPE_LOGIN_REQ:
            return on_login(task, body, body_len);

        case protocol::MSG_TYPE_CHAT_REQ:
            return on_chat(task, body, body_len);



        default:
            fprintf(stderr, "[handler] unknown type: 0x%02x\n", hdr->type);
            return TaskResult{task.fd, {}, true};
    }
}

}  // namespace handler
