# git-commit.cmake - write the commit THIS build was made from into a header.
#
# WHY THIS IS A SCRIPT AND NOT THREE LINES OF execute_process IN CMakeLists.
# It used to be exactly that, and it was silently wrong. CMake re-runs its
# configure step when CMakeLists.txt changes, NOT when HEAD moves, so the SHA
# baked into the binary was whatever HEAD happened to be the last time the
# project was configured: every build after the next commit reported the
# PREVIOUS one. A version identifies a release, a commit identifies a build,
# and the whole reason a report carries the commit is so an engineer can check
# out the exact tree the offsets in it came from. A silently stale SHA is worse
# than "unknown", because it sends them to a tree that exists and is wrong.
#
# So this runs as a custom target on EVERY build. configure_file only rewrites
# the header when its contents actually change, so a build in which HEAD has
# not moved does not recompile the world.
#
# AND THE SHA ALONE IS STILL NOT THE TREE THE BUILD CAME FROM. Measured on
# this project: the binary reported 5ba13f6d0c86 while `git status` listed 32
# modified or untracked entries, including the whole diagnostics feature - so
# an engineer following docs/DIAGNOSTICS.md ("commit: names the exact tree to
# check out") would have checked out a tree that does NOT contain the code the
# offsets came from. That is the same failure as the stale SHA above, and it
# is worse than "unknown" for the same reason: it sends the reader somewhere
# that exists and is wrong. So a modified tree is marked, and the marker is
# part of the commit field itself rather than of the version string -
# tools/build-nightly.ps1 appends ".dirty" to the VERSION, but a hand-compiled
# installer (installer/README-installer.md explicitly contemplates one) never
# goes near that script and was covered by nothing.
#
# `git status --porcelain` and not `git diff --quiet`, deliberately: an
# UNTRACKED source file is exactly as absent from the checked-out tree as a
# modified one, and in the measurement above most of the feature was untracked.
# .gitignore still applies, so build outputs and symbols/ do not mark a tree
# dirty.
#
# Invoked as:
#   cmake -DGIT=<git> -DTREE=<source dir> -DOUT=<header path> -P git-commit.cmake
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

set(CASCADE_GIT_COMMIT "unknown")

if(GIT AND EXISTS "${GIT}")
    execute_process(COMMAND "${GIT}" rev-parse --short=12 HEAD
                    WORKING_DIRECTORY "${TREE}"
                    OUTPUT_VARIABLE _sha
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _rc)
    # "unknown" when the tree is not a git checkout (a source tarball), which is
    # honest rather than absent.
    if(_rc EQUAL 0 AND _sha)
        set(CASCADE_GIT_COMMIT "${_sha}")
        # A modified or untracked-source tree is NOT the commit it names. The
        # marker goes on the commit, because the commit is the line a reader is
        # told to check out.
        execute_process(COMMAND "${GIT}" status --porcelain
                        WORKING_DIRECTORY "${TREE}"
                        OUTPUT_VARIABLE _status
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE _src)
        if(_src EQUAL 0 AND NOT _status STREQUAL "")
            set(CASCADE_GIT_COMMIT "${_sha}-dirty")
        endif()
    endif()
endif()

file(WRITE "${OUT}.tmp"
"// GENERATED ON EVERY BUILD by cmake/git-commit.cmake - do not edit, do not commit.
#define CASCADE_GIT_COMMIT \"${CASCADE_GIT_COMMIT}\"
")
# Only replaces the real header when the SHA actually changed, so an unchanged
# HEAD does not force a rebuild of everything that includes it.
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
