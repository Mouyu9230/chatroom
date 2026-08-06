cmake_minimum_required(VERSION 3.16)
project(chatroom CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- 依赖 ----
# 协议层使用预生成的 protobuf 代码, 只需链接 protobuf 库
find_package(Protobuf REQUIRED)
# thread_pool 使用 std::thread
find_package(Threads REQUIRED)

# ---- 源码 ----
# 唯一的 protobuf 生成文件(chat.pb.cc / user.pb.cc 暂未被任何源码引用)
# 注意: 目录名含 (!) 括号, 路径必须加引号
set(PROTO_PB
    "protocol/(!)message/message.pb.cc"
)

# 网络层公共模块(server / client 共用)
set(NETWORK_SOURCES
    "network/connection/connection.cpp"
    "network/epoll/epoll.cpp"
    "network/recv_send/recv_send.cpp"
    "network/socket/socket.cpp"
    "network/thread_pool/thread_pool.cpp"
)

set(COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

# ---- server ----
add_executable(server
    server/server.cpp
    handler/handler.cpp
    ${NETWORK_SOURCES}
    ${PROTO_PB}
)
target_include_directories(server PRIVATE ${COMMON_INCLUDES})
target_link_libraries(server PRIVATE protobuf::libprotobuf Threads::Threads)

# ---- client ----
# 客户端仅使用 socket 与协议层, 不涉及线程池/业务处理
add_executable(client
    client/client.cpp
    network/socket/socket.cpp
    ${PROTO_PB}
)
target_include_directories(client PRIVATE ${COMMON_INCLUDES})
target_link_libraries(client PRIVATE protobuf::libprotobuf)
