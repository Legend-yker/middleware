# --- Hesai SDK (vendor third-party) ---
add_subdirectory(sdk_core)

# --- Pandar128 driver ---
target_sources(hal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/pandar128_driver.cpp
)

target_link_libraries(hal PUBLIC hesai_sdk)
