cmake_minimum_required(VERSION 3.16)
project(chatroom CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 导出编译数据库, 供 clangd / CLion 等 IDE 正确解析 include 路径
# (chat.pb.h 里 #include "user.pb.h" 依赖 -I protocol/user)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- 依赖 ----
# 协议层使用预生成的 protobuf 代码, 只需链接 protobuf 库
find_package(Protobuf REQUIRED)
# thread_pool 使用 std::thread
find_package(Threads REQUIRED)
# MySQL 客户端 (database/ 模块)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED IMPORTED_TARGET mysqlclient)

# ---- 源码 ----
# 预生成的 protobuf 文件(user/chat 域)
set(PROTO_PB
    "protocol/user/user.pb.cc"
    "protocol/chat/chat.pb.cc"
)

# 网络层公共模块(server / client 共用)
set(NETWORK_SOURCES
    "network/connection/connection.cpp"
    "network/epoll/epoll.cpp"
    "network/recv_send/recv_send.cpp"
    "network/socket/socket.cpp"
    "network/thread_pool/thread_pool.cpp"
)

# MySQL 封装模块(仅 server / db_smoke 使用)
set(DATABASE_SOURCES
    "database/db.cpp"
    "database/db_config.cpp"
    "database/db_pool.cpp"
    "database/db_init.cpp"
    "database/user_db.cpp"
    "database/chat_db.cpp"
)

set(COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol/user   # chat.pb.h 内 #include "user.pb.h"
    ${Protobuf_INCLUDE_DIRS}
)

# ---- server ----
add_executable(server
    server/server.cpp
    server/db_admin.cpp
    handler/handler.cpp
    ${NETWORK_SOURCES}
    ${DATABASE_SOURCES}
    ${PROTO_PB}
)
target_include_directories(server PRIVATE ${COMMON_INCLUDES})
target_link_libraries(server PRIVATE protobuf::libprotobuf Threads::Threads PkgConfig::MYSQL)

# ---- client ----
# 客户端仅使用 socket 与协议层, 不涉及线程池/业务处理/数据库
add_executable(client
    client/client.cpp
    network/socket/socket.cpp
    ${PROTO_PB}
)
target_include_directories(client PRIVATE ${COMMON_INCLUDES})
target_link_libraries(client PRIVATE protobuf::libprotobuf)

# ---- db_smoke ----
# MySQL 封装冒烟测试(独立可执行, 不依赖 handler)
add_executable(db_smoke
    database/db_smoke.cpp
    ${DATABASE_SOURCES}
    ${PROTO_PB}
)
target_include_directories(db_smoke PRIVATE ${COMMON_INCLUDES})
target_link_libraries(db_smoke PRIVATE protobuf::libprotobuf Threads::Threads PkgConfig::MYSQL)
