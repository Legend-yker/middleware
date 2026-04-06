add_executable(hal_sensor_smoke hal_sensor_smoke.cpp)
target_link_libraries(hal_sensor_smoke PRIVATE hal)
target_compile_definitions(hal_sensor_smoke
    PRIVATE
        HAL_TEST_CONFIG_FILE="${CMAKE_SOURCE_DIR}/src/hal/config/hal_online.json"
)
add_test(NAME hal_sensor_smoke COMMAND hal_sensor_smoke)

add_executable(dcms_smoke dcms_smoke.cpp)
target_link_libraries(dcms_smoke PRIVATE dcms)
add_test(NAME dcms_smoke COMMAND dcms_smoke)
