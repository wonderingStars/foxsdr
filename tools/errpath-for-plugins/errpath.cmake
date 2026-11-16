# errpath.cmake - build and register the error-path gate (see README.md). include() it from the top-level CMakeLists.txt
# of the plugins repository, after enable_testing(). Needs only a C++20 compiler.
add_executable(errpath_check "${CMAKE_CURRENT_LIST_DIR}/errpath_check.cpp")
target_compile_features(errpath_check PRIVATE cxx_std_20)
target_include_directories(errpath_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
add_test(NAME errpath_check
         COMMAND errpath_check "${CMAKE_SOURCE_DIR}" --profile plugins --dir src
                 --allow "${CMAKE_CURRENT_LIST_DIR}/errpath_allow.tsv"
                 --reporters "${CMAKE_CURRENT_LIST_DIR}/errpath_reporters.txt"
                 --pin-file "${CMAKE_CURRENT_LIST_DIR}/errpath_pin.txt")
set_tests_properties(errpath_check PROPERTIES TIMEOUT 120)
