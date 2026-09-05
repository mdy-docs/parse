# mdyast — MDY document text to a hast tree, in C.
#
#   make            build lib and CLI
#   make test       the C checks (no node needed)
#   make compare    diff this against mdy-docs' JavaScript over a real corpus
#   make check-html diff the HTML writer against hast-util-to-html
#   make check-script diff the script compiler against compileScript
#   make check-yaml   read every YAML block the project holds, and compare
#   make bench      how long each takes on the same input
#
# See README.md for what this is for and docs/ARCHITECTURE.md for how it works.

CC      ?= cc
AR      ?= ar
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -O2 -g -Iinclude -Isrc -Ithird_party/baru-re/include

SRCS := src/arena.c src/ast.c src/attrs.c src/unicode.c src/linkify.c src/emoji.c src/footnote.c src/inline.c src/block.c src/html.c src/script.c src/yaml.c
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

# Where mdy-docs lives, for the comparison harness. Nothing in the library
# itself depends on it — this repo builds and tests standalone.
MDY_DOCS ?= $(HOME)/projects/mdy-wikipedia-web/third-party/mdy-docs
CORPUS   ?= $(HOME)/projects/mdy-wikipedia-web/site/corpus
# The writer wants the LAYOUTS as well as the documents: the doctype and the
# raw-text elements only appear there, and a doctype under sanitizing was
# wrong here for exactly as long as nothing pointed a check at one.
SITE     ?= $(HOME)/projects/mdy-wikipedia-web/site

all: build/mdyast

build/%.o: src/%.c include/mdyast.h include/mdyhtml.h include/mdyscript.h include/mdyyaml.h src/internal.h
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

# The HTML writer, on trees built BY HAND — no parsing anywhere in it. That is
# the point of the separation: src/html.c takes a tree and returns a string,
# and this proves it without a document in sight.
build/html: test/html.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/html.c build/libmdyast.a -o $@

# YAML, on its own — the language, and what it refuses.
build/yaml: test/yaml.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/yaml.c build/libmdyast.a -o $@

# YAML in, the tree as JSON out — the comparison harness's other half.
build/yamlcat: test/yamlcat.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/yamlcat.c build/libmdyast.a -o $@

# And YAML: every front matter block, ```data fence and .yaml file the project
# holds, read here and by the `yaml` package, compared as JSON.
.PHONY: check-yaml
check-yaml: build/yamlcat
	@node test/compare-yaml.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(SITE)" --corpus "$(SITE)/.."

# The script layer, on documents, with no engine anywhere.
build/script: test/script.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/script.c build/libmdyast.a -o $@

.PHONY: all test compare bench clean check-html check-script
# build/mdyast too, though the checks do not use it: every probe reached for it
# by hand at some point, found yesterday's binary, and reported a bug that had
# already been fixed. Building it here costs a second and stops that.
test: build/smoke build/html build/script build/yaml build/mdyast build/linkify
	@./build/smoke
	@./build/html
	@./build/script
	@./build/yaml

# The check that actually matters. A 4,441-line parser is not ported by
# reading it; it is ported by producing the same tree for a real corpus,
# document by document, and diffing. This reports how far that has got.
compare: build/mdyast
	@node test/compare.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(CORPUS)"

# The same question for the writer: does `mdy_to_html` agree with
# `hast-util-to-html` over the corpus, byte for byte?
check-html: build/mdyast
	@node test/compare-html.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(SITE)"

# And the script layer: same document in, byte-identical JavaScript out. A
# difference here is different BEHAVIOUR, not a different tree.
check-script: build/mdyast
	@node test/compare-script.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(SITE)"

bench: build/mdyast
	@node test/compare.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(CORPUS)" --bench

clean:
	rm -rf build
