# The body of a merge_pending_<test> placeholder (tests/merge_pending.cmake): fails, and says what is owed.
message(FATAL_ERROR
    "FIXME(2026-10-21, the 0.99.84 merge): ${TEST} is master's test of the window feature '${FEATURE}', which is not yet "
    "re-expressed onto the engine's table (docs/engine-merge-0.99.84.md, ledger id ${FEATURE}). The test is kept in tests/${TEST}.cpp "
    "exactly as master wrote it, out of the build. Re-express the feature, convert the test (AppWindowTestAccess over rawEngine(), "
    "tests/pass_seam.hpp), and remove it from tests/merge_pending.cmake.")
