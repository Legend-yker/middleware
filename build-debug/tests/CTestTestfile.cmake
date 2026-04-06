# CMake generated Testfile for 
# Source directory: /home/dji/car1609/middleware/tests
# Build directory: /home/dji/car1609/middleware/build-debug/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(hal_sensor_smoke "/home/dji/car1609/middleware/build-debug/bin/hal_sensor_smoke")
set_tests_properties(hal_sensor_smoke PROPERTIES  _BACKTRACE_TRIPLES "/home/dji/car1609/middleware/tests/CMakeLists.txt;7;add_test;/home/dji/car1609/middleware/tests/CMakeLists.txt;0;")
add_test(dcms_smoke "/home/dji/car1609/middleware/build-debug/bin/dcms_smoke")
set_tests_properties(dcms_smoke PROPERTIES  _BACKTRACE_TRIPLES "/home/dji/car1609/middleware/tests/CMakeLists.txt;11;add_test;/home/dji/car1609/middleware/tests/CMakeLists.txt;0;")
