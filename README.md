# chatroom

获取与构建流程。

## 获取

```bash
git clone git@github.com:Mouyu9230/chatroom.git
cd chatroom
```

## 依赖

- CMake ≥ 3.16
- C++17 编译器
- protobuf(协议代码已预生成并入库, 仅需链接库, 无需 protobuf-compiler)
- mysqlclient
- hiredis
- OpenSSL

Debian / Ubuntu:

```bash
sudo apt install cmake g++ libprotobuf-dev libmysqlclient-dev libhiredis-dev libssl-dev pkg-config
```

## 构建

```bash
mkdir build
cd build
cmake ..
cmake --build . -j"$(nproc)"
```

构建产物为 `build/` 下的 `server` / `client` / `db_smoke` 三个可执行文件。
