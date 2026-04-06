add_executable(gdb_lab main.cpp)
target_compile_features(gdb_lab PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(gdb_lab PRIVATE Threads::Threads)
