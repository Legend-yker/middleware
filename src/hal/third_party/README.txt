HAL 模块本地第三方依赖目录（可选）。
当前 nlohmann/json 由 cmake/hal_deps.cmake 中 find_package 或 FetchContent 拉取；
若需离线构建，可将 json 单头文件或源码包置此目录并在 hal_deps.cmake 中改为 add_subdirectory 引用。
