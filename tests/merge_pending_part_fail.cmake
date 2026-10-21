# The body of a merge_pending_part_<name> placeholder (tests/merge_pending.cmake): fails, and says what is owed.
message(FATAL_ERROR
    "FIXME(2026-10-21, the 0.99.84 merge): part '${PART}' of a converted test belongs to the window feature '${FEATURE}', which is not yet "
    "re-expressed (docs/engine-merge-0.99.84.md, ledger id ${FEATURE}): ${TEXT}")
