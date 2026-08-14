# One executable per test_*.cpp, discovered by glob. Agents add test files
# only; this file itself never needs editing for a new test.
file(GLOB CASCADE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
foreach(src ${CASCADE_TESTS})
    get_filename_component(name "${src}" NAME_WE)
    add_executable(${name} "${src}")
    target_link_libraries(${name} PRIVATE cascade_lib)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# The app doubles as a smoke test: --frames N renders N frames then exits 0.
# (The pre-GUI stub accepts and ignores the flag under the same contract.)
add_test(NAME app_smoke COMMAND cascade --frames 3)
