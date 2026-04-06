# --- Airy lidar driver ---
# When vendor SDK is integrated, add sdk_core sources here:
# file(GLOB AIRY_SDK_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/sdk_core/*.cpp)

target_sources(hal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/airy_driver.cpp
    # ${AIRY_SDK_SRCS}
)

# If vendor SDK has extra include paths:
# target_include_directories(hal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk_core)
