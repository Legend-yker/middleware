# =============================================================================
# dlog — 分布式模块日志库
# =============================================================================

# ---- 核心库 ----
add_library(dlog STATIC
    src/logger.cpp
)

target_include_directories(dlog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(dlog PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(dlog PUBLIC Threads::Threads)

# ---- dlog 可执行应用（lidar 订阅者 demo）----
add_subdirectory(app)
