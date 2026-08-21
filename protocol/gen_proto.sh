#!/usr/bin/env bash
# ============================================================
#  重新生成 user/chat/group/file 域的 protobuf 代码
#
#  背景: 项目使用"预生成 pb"策略(生成物入库, 构建不跑 protoc)。
#  user.proto / chat.proto / group.proto / file.proto 改动后, 需运行本脚本重新生成。
#
#  说明:
#    - chat.proto / group.proto / file.proto 里 `import "user.proto"`, 生成时
#      多个 .proto 须放在同一目录, 才能正确解析; 生成后各归其位。
#    - 生成出的 chat.pb.h / group.pb.h / file.pb.h 会 `#include "user.pb.h"`,
#      因此 CMake 需把 protocol/user 加入 include path。
#    - 依赖: protoc (proto3), 无需其它插件。
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cp user/user.proto chat/chat.proto group/group.proto file/file.proto "$tmp/"
( cd "$tmp" && protoc --cpp_out=. user.proto chat.proto group.proto file.proto )

cp "$tmp"/user.pb.h  "$tmp"/user.pb.cc  user/
cp "$tmp"/chat.pb.h  "$tmp"/chat.pb.cc  chat/
cp "$tmp"/group.pb.h "$tmp"/group.pb.cc group/
cp "$tmp"/file.pb.h  "$tmp"/file.pb.cc  file/

echo "[gen_proto] user/chat/group/file pb regenerated."
