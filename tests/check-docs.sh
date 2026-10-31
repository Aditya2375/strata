#!/bin/sh
# the README states the unit-test check count; keep it honest
want=$(grep -o '[0-9,]* unit checks' README.md | head -1 | tr -d ',' | grep -o '^[0-9]*')
got=$(./build/test-unit | grep -o '^[0-9]* checks passed' | grep -o '^[0-9]*')
if [ "$want" != "$got" ]; then
  echo "README says $want unit checks, test-unit ran $got"
  exit 1
fi
echo "doc counts verified: $got unit checks"
