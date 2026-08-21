cmake_minimum_required(VERSION 3.15)
project(ftp_project)
set(CMAKE_CXX_STANDARD 17)

include_directories(${PROJECT_SOURCE_DIR}/src/client)
include_directories(${PROJECT_SOURCE_DIR}/src/server)

add_executable(client
    src/client/client.cpp

)

add_executable(server
    src/server/server.cpp

)