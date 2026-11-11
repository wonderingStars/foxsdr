# Stand-alone probe: what does the Slint text engine do with right-to-left text?
# Not part of the FoxSDR build (nothing includes this directory). Configure it by hand:
#   cmake -S tools/access_rtl_probe -B <build dir> -DCMAKE_PREFIX_PATH=C:/Users/steve/slint-cpp-1.18.1
#   cmake --build <build dir> --config Release
# then copy slint_cpp.dll next to the exe and run it: it renders into memory with the
# software renderer (no OS window is created) and prints what it measured.
cmake_minimum_required(VERSION 3.21)
project(access_rtl_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Slint REQUIRED)
add_executable(access_rtl_probe main.cpp)
target_link_libraries(access_rtl_probe PRIVATE Slint::Slint)
slint_target_sources(access_rtl_probe probe.slint)
