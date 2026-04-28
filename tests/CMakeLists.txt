add_executable(lidar_frame_manager_test lidar_frame_manager_test.cpp)
target_link_libraries(lidar_frame_manager_test PRIVATE hal)
middleware_set_module_output(lidar_frame_manager_test tests)
add_test(NAME lidar_frame_manager_test COMMAND lidar_frame_manager_test)

add_executable(hal_sensor_smoke hal_sensor_smoke.cpp)
target_link_libraries(hal_sensor_smoke PRIVATE hal)
target_compile_definitions(hal_sensor_smoke
    PRIVATE
        HAL_TEST_CONFIG_FILE="${CMAKE_SOURCE_DIR}/src/hal/config/hal_online.json"
)
middleware_set_module_output(hal_sensor_smoke tests)
add_test(NAME hal_sensor_smoke COMMAND hal_sensor_smoke)

add_executable(dcms_smoke dcms_smoke.cpp)
target_link_libraries(dcms_smoke PRIVATE dcms dlog)
middleware_set_module_output(dcms_smoke tests)
add_test(NAME dcms_smoke COMMAND dcms_smoke)

add_executable(dcms_rpc_smoke dcms_rpc_smoke.cpp)
target_link_libraries(dcms_rpc_smoke PRIVATE dcms dlog)
middleware_set_module_output(dcms_rpc_smoke tests)
add_test(NAME dcms_rpc_smoke COMMAND dcms_rpc_smoke)

# 全模块集成测试：daemon 跑在线程里，所有模块日志写同一个 .log.gz
add_executable(hal_dcms_integration hal_dcms_integration.cpp)
target_link_libraries(hal_dcms_integration PRIVATE dcms dlog)
middleware_set_module_output(hal_dcms_integration tests)
add_test(NAME hal_dcms_integration COMMAND hal_dcms_integration)
