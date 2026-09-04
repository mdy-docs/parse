# mdyast — MDY document text to a hast tree, in C.
#
#   make            build lib and CLI
#   make test       the C checks (no node needed)
#   make compare    diff this against mdy-docs' JavaScript over a real corpus
#   make bench      how long each takes on the same input
#
# See README.md for what this is for and docs/ARCHITECTURE.md for how it works.

CC      ?= cc
AR      ?= ar
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -O2 -g -Iinclude -Isrc -Ithird_party/baru-re/include

SRCS := src/arena.c src/ast.c src/attrs.c src/unicode.c src/linkify.c src/emoji.c src/footnote.c src/inline.c src/block.c
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

# Where mdy-docs lives, for the comparison harness. Nothing in the library
# itself depends on it — this repo builds and tests standalone.
MDY_DOCS ?= $(HOME)/projects/mdy-wikipedia-web/third-party/mdy-docs
CORPUS   ?= $(HOME)/projects/mdy-wikipedia-web/site/corpus

all: build/mdyast

build/%.o: src/%.c include/mdyast.h src/internal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/libmdyast.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

build/mdyast: src/main.c build/libmdyast.a
	$(CC) $(CFLAGS) src/main.c build/libmdyast.a -o $@

build/linkify: test/linkify.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/linkify.c build/libmdyast.a -o $@

# Does the URL matching agree with linkify-it? Its rules are nobody's guess, so
# agreement is measured rather than assumed — see src/linkify.c.
.PHONY: check-links
check-links: build/linkify
	@node test/linkify.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(CORPUS)"

build/smoke: test/smoke.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/smoke.c build/libmdyast.a -o $@

.PHONY: all test compare bench clean
# build/mdyast too, though the checks do not use it: every probe reached for it
# by hand at some point, found yesterday's binary, and reported a bug that had
# already been fixed. Building it here costs a second and stops that.
test: build/smoke build/mdyast build/linkify
	@./build/smoke

# The check that actually matters. A 4,441-line parser is not ported by
# reading it; it is ported by producing the same tree for a real corpus,
# document by document, and diffing. This reports how far that has got.
compare: build/mdyast
	@node test/compare.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(CORPUS)"

bench: build/mdyast
	@node test/compare.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(CORPUS)" --bench

clean:
	rm -rf build
