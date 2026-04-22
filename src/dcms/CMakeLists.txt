add_library(dcms STATIC
    src/common/dcms_types.cpp
    src/common/crc32.cpp
    src/shm/buddy_allocator.cpp
    src/shm/slab_allocator.cpp
    src/shm/shm_pool.cpp
    src/shm/ring_buffer.cpp
    src/utils/msg_queue.cpp
    src/utils/dcms_thread.cpp
    src/api/dcms.cpp
    src/api/publisher.cpp
    src/api/node_handle.cpp
    src/daemon/daemon.cpp      # 集成测试可在同进程实例化 Daemon
)

target_include_directories(dcms
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(dcms PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(dcms PUBLIC Threads::Threads rt dlog)

# QNX Pulse backend: kept but not compiled on Linux
set(DCMS_IPC_BACKEND "LINUX" CACHE STRING "DCMS IPC backend (LINUX or QNX_PULSE)")
if(DCMS_IPC_BACKEND STREQUAL "QNX_PULSE")
    message(WARNING "QNX_PULSE backend not yet ported to the new Daemon architecture")
endif()

add_executable(dcms_daemon
    src/app/main.cpp
    src/daemon/daemon.cpp
)
target_link_libraries(dcms_daemon PRIVATE dcms dlog)
target_compile_features(dcms_daemon PRIVATE cxx_std_17)
middleware_set_module_output(dcms_daemon dcms)
