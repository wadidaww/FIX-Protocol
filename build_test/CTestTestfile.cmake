# CMake generated Testfile for 
# Source directory: /home/runner/work/FIX-Protocol/FIX-Protocol
# Build directory: /home/runner/work/FIX-Protocol/FIX-Protocol/build_test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
include("/home/runner/work/FIX-Protocol/FIX-Protocol/build_test/fix_tests[1]_include.cmake")
add_test([=[fix_unit_tests]=] "/home/runner/work/FIX-Protocol/FIX-Protocol/build_test/fix_tests")
set_tests_properties([=[fix_unit_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/runner/work/FIX-Protocol/FIX-Protocol/CMakeLists.txt;190;add_test;/home/runner/work/FIX-Protocol/FIX-Protocol/CMakeLists.txt;0;")
subdirs("_deps/googletest-build")
