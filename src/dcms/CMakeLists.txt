set(DCMS_IPC_BACKEND "UNIX_SOCKET" CACHE STRING "DCMS IPC backend: UNIX_SOCKET or QNX_PULSE")
set_property(CACHE DCMS_IPC_BACKEND PROPERTY STRINGS UNIX_SOCKET QNX_PULSE)

add_library(dcms STATIC
    src/common/dcms_types.cpp
    src/common/crc32.cpp
    src/shm/buddy_allocator.cpp
    src/shm/slab_allocator.cpp
    src/shm/shm_pool.cpp
    src/transport/common/ipc_backend.cpp
    src/utils/msg_queue.cpp
    src/manager/recv_thread.cpp
    src/api/dcms_api.cpp
    src/api/dcms.cpp
    src/api/publisher.cpp
)

target_include_directories(dcms
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(dcms PUBLIC cxx_std_17)

if(DCMS_IPC_BACKEND STREQUAL "QNX_PULSE")
    target_compile_definitions(dcms PUBLIC DCMS_IPC_QNX_PULSE=1)
    target_sources(dcms PRIVATE src/transport/qnx_pulse/qnx_pulse_backend.cpp)
else()
    target_compile_definitions(dcms PUBLIC DCMS_IPC_UNIX_SOCKET=1)
    target_sources(dcms PRIVATE src/transport/unix_socket/unix_socket_backend.cpp)
endif()

find_package(Threads REQUIRED)
target_link_libraries(dcms PUBLIC Threads::Threads)

add_executable(dcms_app src/app/main.cpp)
target_link_libraries(dcms_app PRIVATE dcms)
target_compile_features(dcms_app PRIVATE cxx_std_17)
