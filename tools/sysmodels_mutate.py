#!/usr/bin/env python3
# Replaces EXACTLY ONE occurrence of a string in a file (the lane's break-it step). Exit 3 when the text is not found exactly once, so a mutation that did not apply can
# never be mistaken for a test that stayed green. A multi-line text is also tried with CRLF line endings (a file git checked out with them).
# usage: python -I tools/sysmodels_mutate.py <file> <from-text> <to-text>
import sys

LF = bytes([10])
CRLF = bytes([13, 10])

path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, "rb") as f:
    data = f.read()
old_b = old.encode("utf-8")
new_b = new.encode("utf-8")
n = data.count(old_b)
if n == 0 and LF in old_b:
    old_b = old_b.replace(LF, CRLF)
    new_b = new_b.replace(LF, CRLF)
    n = data.count(old_b)
if n != 1:
    print("MUTATION NOT APPLIED: %d occurrences of the text in %s" % (n, path))
    sys.exit(3)
with open(path, "wb") as f:
    f.write(data.replace(old_b, new_b))
print("mutated %s" % path)
