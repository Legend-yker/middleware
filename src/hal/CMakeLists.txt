include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/hal_deps.cmake)
hal_setup_nlohmann_json()

add_library(hal STATIC
    src/utils/config.cpp
    src/utils/thread.cpp
    src/utils/hal_json.cpp
    src/factory/sensor_factory.cpp
    src/manager/config.cpp
    src/manager/sensor_manager.cpp
    src/sensor/common/sensor_base.cpp
    src/sensor/common/sensor_types.cpp
)

target_include_directories(hal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(hal PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(hal PUBLIC nlohmann_json::nlohmann_json Threads::Threads dcms dlog)

# Sensor sub-modules (each adds its own sources via target_sources)
add_subdirectory(src/sensor/camera)
add_subdirectory(src/sensor/imu)
add_subdirectory(src/sensor/gnss)
add_subdirectory(src/sensor/odo)
add_subdirectory(src/sensor/lidar)

set(HAL_DEFAULT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config/hal_online.json")
add_executable(hal_app src/app/main.cpp)
target_link_libraries(hal_app PRIVATE hal)
target_compile_features(hal_app PUBLIC cxx_std_17)
target_compile_definitions(hal_app
    PRIVATE
        "HAL_DEFAULT_CONFIG_PATH=\"${HAL_DEFAULT_CONFIG_FILE}\""
)
middleware_set_module_output(hal_app hal)
