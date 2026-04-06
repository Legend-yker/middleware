# HAL 模块第三方依赖（与仓库根区分，便于单独迁移/裁剪）

macro(hal_setup_nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
    if(NOT nlohmann_json_FOUND)
        if(POLICY CMP0135)
            cmake_policy(SET CMP0135 NEW)
        endif()
        include(FetchContent)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
            FetchContent_Declare(
                nlohmann_json
                URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            )
        else()
            FetchContent_Declare(
                nlohmann_json
                URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
            )
        endif()
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endmacro()
