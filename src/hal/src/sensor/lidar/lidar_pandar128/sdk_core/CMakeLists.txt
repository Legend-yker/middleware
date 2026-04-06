# Hesai LiDAR SDK 2.0 — integrated as static library
cmake_minimum_required(VERSION 3.14)

set(HESAI_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(HESAI_LIB  ${HESAI_ROOT}/libhesai)

# ── Options ──────────────────────────────────────────────────────────
option(HESAI_WITH_SSL   "Build with OpenSSL (PTC-S TLS support)" ON)
option(HESAI_WITH_GPU   "Build with CUDA GPU parser"             OFF)

# ── Source files ─────────────────────────────────────────────────────
# NOTE: Many .cc files (UdpParser/*, Container/*, Lidar/lidar.cc) are
# template implementations that get #include'd from their headers.
# They must NOT appear here as separate translation units.
set(HESAI_SOURCES
    # Logger
    ${HESAI_LIB}/Logger/src/logger.cc
    # Common / PlatUtils
    ${HESAI_LIB}/Common/src/plat_utils.cc
    ${HESAI_LIB}/Common/src/auto_tick_count.cc
    # Source (socket / pcap / serial / tcp)
    ${HESAI_LIB}/Source/src/source.cc
    ${HESAI_LIB}/Source/src/socket_source.cc
    ${HESAI_LIB}/Source/src/pcap_source.cc
    ${HESAI_LIB}/Source/src/serial_source.cc
    ${HESAI_LIB}/Source/src/pcap_saver.cc
    ${HESAI_LIB}/Source/src/tcp_source.cc
    # SerialClient
    ${HESAI_LIB}/SerialClient/src/serial_client.cc
    # PtcParser
    ${HESAI_LIB}/PtcParser/ptc_parser.cc
    ${HESAI_LIB}/PtcParser/src/general_ptc_parser.cc
    ${HESAI_LIB}/PtcParser/src/ptc_1_0_parser.cc
    ${HESAI_LIB}/PtcParser/src/ptc_2_0_parser.cc
    # PtcClient
    ${HESAI_LIB}/PtcClient/src/ptc_client.cc
    ${HESAI_LIB}/PtcClient/src/tcp_client.cc
    # Top-level driver entry
    ${HESAI_ROOT}/driver/hesai_lidar_sdk.cc
)

if(HESAI_WITH_SSL)
    list(APPEND HESAI_SOURCES ${HESAI_LIB}/PtcClient/src/tcp_ssl_client.cc)
endif()

# ── Static library ───────────────────────────────────────────────────
add_library(hesai_sdk STATIC ${HESAI_SOURCES})

set_target_properties(hesai_sdk PROPERTIES
    CXX_STANDARD          14
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(hesai_sdk PUBLIC
    ${HESAI_ROOT}/driver
    ${HESAI_LIB}
    ${HESAI_LIB}/Lidar
    ${HESAI_LIB}/Common/include
    ${HESAI_LIB}/Container/include
    ${HESAI_LIB}/Container/src
    ${HESAI_LIB}/Logger/include
    ${HESAI_LIB}/Source/include
    ${HESAI_LIB}/SerialClient/include
    ${HESAI_LIB}/UdpParser
    ${HESAI_LIB}/UdpParser/include
    ${HESAI_LIB}/UdpParser/src
    ${HESAI_LIB}/UdpProtocol
    ${HESAI_LIB}/PtcParser
    ${HESAI_LIB}/PtcParser/include
    ${HESAI_LIB}/PtcClient/include
)

# ── Dependencies ─────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(hesai_sdk PUBLIC Threads::Threads)

if(HESAI_WITH_SSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(hesai_sdk PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(hesai_sdk PUBLIC WITH_PTCS_USE=1)
endif()

# Suppress vendor SDK warnings (third-party code)
target_compile_options(hesai_sdk PRIVATE -w)
