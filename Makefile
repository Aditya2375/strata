CC ?= cc
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Werror -Isrc
EXTRA ?=

all: build/strata

build/strata: src/main.c src/fs.c src/fsck.c src/fs.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(EXTRA) src/main.c src/fs.c src/fsck.c -o build/strata

build/test-unit: tests/unit/test-unit.c src/fs.c src/fsck.c src/fs.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(EXTRA) tests/unit/test-unit.c src/fs.c src/fsck.c -o build/test-unit

test: build/strata build/test-unit
	./tests/run-tests.sh

clean:
	rm -rf build

.PHONY: all test clean
