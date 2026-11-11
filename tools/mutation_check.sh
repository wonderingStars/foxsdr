#!/bin/bash
# tools/mutation_check.sh - one BREAK-IT check: apply a one-line mutation to a source file, build the named test targets, run the named tests and say whether they went
# RED, then put the file back with git. Used by the engine features lane (docs/multi-radio-records/engine-features-a.md) for the guarantees it added.
#
#   mutation_check.sh <id> <file relative to the repo> <sed expression> "<targets>" "<test[:argument] ...>"
#
# The file must be committed (the restore is `git checkout -- <file>`). The build folder is D:/foxsdr-build/mr-engine1 (edit B below for another).
id="$1"; file="$2"; expr="$3"; targets="$4"; tests="$5"
R=C:/Users/steve/foxsdr-wt/mr-engine1
B=D:/foxsdr-build/mr-engine1
out=$B/lane/mut-$id.log
cd "$R" || exit 2
git diff --quiet -- "$file" || { echo "$id: $file has uncommitted changes; refusing" > "$out"; exit 2; }
sed -i "$expr" "$file"
if git diff --quiet -- "$file"; then echo "$id: the sed expression changed nothing" > "$out"; exit 2; fi
{
  echo "== mutation $id on $file"
  git diff -U0 -- "$file" | grep -E '^[+-][^+-]' | cut -c1-200
  cmake --build $B --config Release --target $targets --parallel 6 > $B/lane/mut-$id-build.log 2>&1
  echo "build exit=$?"
  for t in $tests; do
    name=${t%%:*}; arg=""; [ "$t" != "$name" ] && arg=${t#*:}
    timeout 280 $B/tests/Release/$name.exe $arg 2>&1 | grep -E "FAIL|checks" | head -4 | cut -c1-200
    echo "  $name${arg:+ $arg}: exit ${PIPESTATUS[0]}"
  done
} > "$out" 2>&1
git checkout -- "$file"
echo "restored" >> "$out"
