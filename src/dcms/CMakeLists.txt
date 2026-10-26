# IPC 后端选项（对齐公司生产版：Linux 默认 POSIX 信号量，QNX 用 pulse）
# LINUX_SEM：sem_post / sem_timedwait，延迟更低、无内核 fd 开销
# LINUX    ：eventfd（保留，方便调试对比）
# QNX_PULSE：QNX 微内核原生 pulse（存根）
set(DCMS_IPC_BACKEND "LINUX_SEM" CACHE STRING "DCMS IPC backend: LINUX_SEM / LINUX / QNX_PULSE")
set_property(CACHE DCMS_IPC_BACKEND PROPERTY STRINGS LINUX LINUX_SEM QNX_PULSE)

set(DCMS_BACKEND_SOURCES "")
if(DCMS_IPC_BACKEND STREQUAL "LINUX_SEM")
    list(APPEND DCMS_BACKEND_SOURCES
        src/transport/linux/linux_sem_backend.cpp
    )
    message(STATUS "DCMS: using Linux POSIX semaphore backend")
elseif(DCMS_IPC_BACKEND STREQUAL "QNX_PULSE")
    list(APPEND DCMS_BACKEND_SOURCES
        src/transport/qnx_pulse/qnx_pulse_backend.cpp
    )
    message(WARNING "DCMS: QNX_PULSE backend is a stub and not functional on Linux")
else()
    message(STATUS "DCMS: using default Linux eventfd backend")
endif()

add_library(dcms STATIC
    src/common/dcms_types.cpp
    src/common/crc32.cpp
    src/common/e2e.cpp
    src/common/whitelist.cpp
    src/shm/buddy_allocator.cpp
    src/shm/slab_allocator.cpp
    src/shm/shm_pool.cpp
    src/shm/ring_buffer.cpp
    src/shm/msg_lifecycle.cpp
    src/utils/msg_queue.cpp
    src/utils/dcms_thread.cpp
    src/api/dcms.cpp
    src/api/publisher.cpp
    src/api/node_handle.cpp
    src/daemon/daemon.cpp
    ${DCMS_BACKEND_SOURCES}
)

target_include_directories(dcms
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(dcms PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(dcms PUBLIC Threads::Threads rt dlog)

if(DCMS_IPC_BACKEND STREQUAL "LINUX_SEM")
    target_compile_definitions(dcms PUBLIC DCMS_BACKEND_LINUX_SEM)
elseif(DCMS_IPC_BACKEND STREQUAL "QNX_PULSE")
    target_compile_definitions(dcms PUBLIC DCMS_BACKEND_QNX_PULSE)
endif()

add_executable(dcms_daemon
    src/app/main.cpp
    src/daemon/daemon.cpp
)
target_link_libraries(dcms_daemon PRIVATE dcms dlog)
target_compile_features(dcms_daemon PRIVATE cxx_std_17)
middleware_set_module_output(dcms_daemon dcms)
