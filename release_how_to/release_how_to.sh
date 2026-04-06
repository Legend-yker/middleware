#!/usr/bin/env bash
# 发布构建入口：调用同目录下的 build_x86 / build_arm
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    echo "Usage: $0 {x86|arm|help}"
    echo ""
    echo "  x86  — 本机构建，输出目录: ${DIR}/build-x86"
    echo "  arm  — ARM 交叉编译，输出目录: ${DIR}/build-arm"
    echo "         环境变量: ARM_TRIPLE (默认 aarch64-linux-gnu)"
    echo "                   BUILD_TYPE (默认 Release)"
    echo ""
    echo "可将额外 CMake 配置参数直接传给 build 脚本，例如："
    echo "  $0 x86 -- -DMIDDLEWARE_BUILD_TESTS=OFF"
}

case "${1:-help}" in
x86)
    shift
    exec "${DIR}/build_x86" "$@"
    ;;
arm)
    shift
    exec "${DIR}/build_arm" "$@"
    ;;
help | -h | --help)
    usage
    ;;
*)
    usage
    exit 1
    ;;
esac
