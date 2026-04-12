add_executable(pubsub_demo main.cpp)
target_compile_features(pubsub_demo PRIVATE cxx_std_17)

target_include_directories(pubsub_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/src/dcms/src
    ${CMAKE_SOURCE_DIR}/src/dlog/include
)

find_package(Threads REQUIRED)
target_link_libraries(pubsub_demo PRIVATE dcms dlog Threads::Threads)

middleware_set_module_output(pubsub_demo examples)
