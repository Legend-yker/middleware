# CMake generated Testfile for 
# Source directory: /home/dji/car1609/middleware/tests
# Build directory: /home/dji/car1609/middleware/build-x86-new/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(hal_sensor_smoke "/home/dji/car1609/middleware/build-x86-new/bin/tests/hal_sensor_smoke")
set_tests_properties(hal_sensor_smoke PROPERTIES  _BACKTRACE_TRIPLES "/home/dji/car1609/middleware/tests/CMakeLists.txt;8;add_test;/home/dji/car1609/middleware/tests/CMakeLists.txt;0;")
add_test(dcms_smoke "/home/dji/car1609/middleware/build-x86-new/bin/tests/dcms_smoke")
set_tests_properties(dcms_smoke PROPERTIES  _BACKTRACE_TRIPLES "/home/dji/car1609/middleware/tests/CMakeLists.txt;13;add_test;/home/dji/car1609/middleware/tests/CMakeLists.txt;0;")
add_test(dcms_rpc_smoke "/home/dji/car1609/middleware/build-x86-new/bin/tests/dcms_rpc_smoke")
set_tests_properties(dcms_rpc_smoke PROPERTIES  _BACKTRACE_TRIPLES "/home/dji/car1609/middleware/tests/CMakeLists.txt;18;add_test;/home/dji/car1609/middleware/tests/CMakeLists.txt;0;")
add_test(hal_dcms_integration "/home/dji/car1609/middleware/build-x86-new/bin/tests/hal_dcms_integration")
set_tests_properties(hal_dcms_integration PROPERTIES  _BACKTRACE_TRIPLES "/home/dji/car1609/middleware/tests/CMakeLists.txt;24;add_test;/home/dji/car1609/middleware/tests/CMakeLists.txt;0;")
