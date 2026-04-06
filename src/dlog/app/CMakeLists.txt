set(DLOG_DEFAULT_HAL_CONFIG_FILE "${CMAKE_SOURCE_DIR}/src/hal/config/hal_online.json")

add_executable(dlog_lidar_subscriber_demo main.cpp)
target_link_libraries(dlog_lidar_subscriber_demo PRIVATE hal dcms)
target_compile_features(dlog_lidar_subscriber_demo PUBLIC cxx_std_17)
target_compile_definitions(dlog_lidar_subscriber_demo
    PRIVATE
        "DLOG_DEFAULT_HAL_CONFIG_PATH=\"${DLOG_DEFAULT_HAL_CONFIG_FILE}\""
)
