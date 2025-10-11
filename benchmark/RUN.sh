#!/bin/bash
# 运行脚本 - 自动设置动态库路径

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 设置动态库搜索路径
export DYLD_LIBRARY_PATH="${BUILD_DIR}:${DYLD_LIBRARY_PATH}"

# 检查参数
if [ $# -eq 0 ]; then
    echo "用法: $0 [server|client] [参数...]"
    echo ""
    echo "示例:"
    echo "  $0 server              # 启动服务器（默认8070端口）"
    echo "  $0 server 9000 4096    # 启动服务器（9000端口，backlog 4096）"
    echo "  $0 client              # 启动客户端（默认配置）"
    echo "  $0 client 127.0.0.1 8070 100 1000 1024  # 自定义配置"
    exit 1
fi

MODE=$1
shift

case "$MODE" in
    server)
        echo "启动压测服务器..."
        exec "${SCRIPT_DIR}/stress_tcp_server" "$@"
        ;;
    client)
        echo "启动压测客户端..."
        exec "${SCRIPT_DIR}/stress_tcp_client" "$@"
        ;;
    *)
        echo "错误: 未知模式 '$MODE'"
        echo "请使用 'server' 或 'client'"
        exit 1
        ;;
esac

