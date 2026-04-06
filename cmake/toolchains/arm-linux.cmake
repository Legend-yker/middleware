# ARM 交叉编译 Linux 目标（gcc 工具链前缀可配置）
# 用法示例：
#   cmake -S .. -B ../release_how_to/build-arm \
#     -DCMAKE_TOOLCHAIN_FILE=$(pwd)/cmake/toolchains/arm-linux.cmake \
#     -DARM_TRIPLE=aarch64-linux-gnu
#
# 也可通过环境变量覆盖：ARM_TRIPLE=arm-linux-gnueabihf

if(DEFINED ENV{ARM_TRIPLE} AND NOT ARM_TRIPLE)
    set(ARM_TRIPLE "$ENV{ARM_TRIPLE}")
endif()
if(NOT ARM_TRIPLE)
    set(ARM_TRIPLE "aarch64-linux-gnu")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ARM)

set(CMAKE_C_COMPILER   "${ARM_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${ARM_TRIPLE}-g++")

if(DEFINED ARM_SYSROOT AND ARM_SYSROOT)
    set(CMAKE_SYSROOT "${ARM_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${ARM_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endif()
