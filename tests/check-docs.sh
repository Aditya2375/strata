#!/bin/sh
# the README states the unit-test check count; keep it honest
want=$(grep -o '[0-9,]* unit checks' README.md | head -1 | tr -d ',' | grep -o '^[0-9]*')
got=$(./build/test-unit | grep -o '^[0-9]* checks passed' | grep -o '^[0-9]*')
if [ "$want" != "$got" ]; then
  echo "README says $want unit checks, test-unit ran $got"
  exit 1
fi
# the README states the exact maximum file size; verify it from fs.h
want_bytes=$(grep -o '[0-9,]* B)' README.md | head -1 | tr -d ',(' | grep -o '^[0-9]*')
ndirect=$(grep -o 'define STRATA_NDIRECT *[0-9]*' src/fs.h | grep -o '[0-9]*')
bs=$(grep -o 'define STRATA_BS *[0-9]*' src/fs.h | grep -o '[0-9]*')
got_bytes=$(( (ndirect + bs / 4) * bs ))
if [ "$want_bytes" != "$got_bytes" ]; then
  echo "README says max file $want_bytes B, geometry gives $got_bytes B"
  exit 1
fi
echo "doc counts verified: $got unit checks, max file $got_bytes B"
