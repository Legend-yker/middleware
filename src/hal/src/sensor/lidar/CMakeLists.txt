target_sources(hal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common/lidar_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/common/lidar_frame_manager.cpp
)

add_subdirectory(lidar_pandar128)
add_subdirectory(lidar_airy)
