# NoOutsideReports.cmake - THE CTEST HALF OF "NO TEST SENDS A REPORT OFF THIS COMPUTER" (send-on lane; src/core/report_send_guard.hpp).
# Every test of the directory that calls this gets FOXSDR_NO_OUTSIDE_REPORTS=1 appended to its environment: the real transports then refuse any destination
# that is not 127.0.0.1, localhost or ::1, so a test that starts a stand-in server still works and one that forgets to redirect the telemetry address
# cannot reach telemetry.foxsdr.com. Call it as the LAST line of a CMakeLists.txt that defines tests (properties are appended, never replaced).
macro(cascade_forbid_outside_reports)
    get_property(_cfor_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    foreach(_cfor_t IN LISTS _cfor_tests)
        set_property(TEST ${_cfor_t} APPEND PROPERTY ENVIRONMENT "FOXSDR_NO_OUTSIDE_REPORTS=1")
    endforeach()
endmacro()
