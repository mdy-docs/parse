# mdyast — MDY document text to a hast tree, in C.
#
#   make            build lib and CLI
#   make test       the C checks (no node needed)
#   make compare    diff this against mdy-docs' JavaScript over a real corpus
#   make check-html diff the HTML writer against hast-util-to-html
#   make check-script diff the script compiler against compileScript
#   make check-yaml   read every YAML block the project holds, and compare
#   make corpus       assemble the borrowed markdown corpus
#   make check-markdown  the markdown front end against remark
#   make bench      how long each takes on the same input
#
# See README.md for what this is for and docs/ARCHITECTURE.md for how it works.

CC      ?= cc
AR      ?= ar
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -O2 -g -Iinclude -Isrc -Ithird_party/baru-re/include

# md4c — CommonMark + GFM in C, MIT, vendored at third_party/md4c. Not part of
# the library yet: the front end that turns its callbacks into a hast tree is
# the next piece of work, and build/md4cprobe is the check that comes first.
MD4C_INC := -Ithird_party/md4c/src
MD4C_SRCS := third_party/md4c/src/md4c.c third_party/md4c/src/entity.c

SRCS := src/arena.c src/ast.c src/attrs.c src/unicode.c src/linkify.c src/emoji.c src/footnote.c src/inline.c src/block.c src/html.c src/script.c src/yaml.c src/data.c src/markdown.c src/doc.c
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

# Where mdy-docs lives, for the comparison harness. Nothing in the library
# itself depends on it — this repo builds and tests standalone.
MDY_DOCS ?= $(HOME)/projects/mdy-wikipedia-web/third-party/mdy-docs
CORPUS   ?= $(HOME)/projects/mdy-wikipedia-web/site/corpus
# The writer wants the LAYOUTS as well as the documents: the doctype and the
# raw-text elements only appear there, and a doctype under sanitizing was
# wrong here for exactly as long as nothing pointed a check at one.
SITE     ?= $(HOME)/projects/mdy-wikipedia-web/site
THEME    ?= $(HOME)/projects/mdy-wikipedia-web/style-antiquity

all: build/mdyast

build/markdown.o: src/markdown.c include/mdymarkdown.h src/internal.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(MD4C_INC) -c $< -o $@

build/%.o: src/%.c include/mdyast.h include/mdyhtml.h include/mdyscript.h include/mdyyaml.h include/mdydata.h include/mdymarkdown.h include/mdydoc.h include/mdybuild.h src/internal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/md4c.o: third_party/md4c/src/md4c.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(MD4C_INC) -c $< -o $@

build/md4c-entity.o: third_party/md4c/src/entity.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(MD4C_INC) -c $< -o $@

build/libmdyast.a: $(OBJS) build/md4c.o build/md4c-entity.o
	$(AR) rcs $@ $(OBJS) build/md4c.o build/md4c-entity.o

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

# A source split into documents, and each into front matter and body.
build/doccat: test/doccat.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/doccat.c build/libmdyast.a -o $@

# Markdown in, the hast tree as JSON out — the other half of check-markdown.
build/mdcat: test/mdcat.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/mdcat.c build/libmdyast.a -o $@

# Does md4c read the borrowed corpus at all? The check before the front end.
build/md4cprobe: test/md4cprobe.c $(MD4C_SRCS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(MD4C_INC) test/md4cprobe.c $(MD4C_SRCS) -o $@

# ```data fences, on constructed cases — this project has none.
build/data: test/data.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/data.c build/libmdyast.a -o $@

# Data fences in, their YAML and the remaining body out.
build/datacat: test/datacat.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/datacat.c build/libmdyast.a -o $@

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
# A markdown corpus, borrowed — the CommonMark and GFM specs, md4c's own
# extension specs, and whatever real .md files are on this machine. Nothing is
# committed; see scripts/build-corpus.mjs for provenance and licences.
.PHONY: corpus check-markdown
corpus:
	@node scripts/build-corpus.mjs --roots "$(SITE)/.."

# The reference side alone until there is a C front end to point at with
# --tool: how much of the corpus remark reads, and what it costs.
check-markdown: build/mdcat
	@node test/compare-markdown.mjs --mdy-docs "$(MDY_DOCS)" --tool "$(if $(TOOL),$(TOOL),build/mdcat)"

.PHONY: check-yaml
check-yaml: build/yamlcat
	@node test/compare-yaml.mjs --mdy-docs "$(MDY_DOCS)" --corpus "$(SITE)" --corpus "$(THEME)"

# The script layer, on documents, with no engine anywhere.
build/script: test/script.c build/libmdyast.a
	@mkdir -p build
	$(CC) $(CFLAGS) test/script.c build/libmdyast.a -o $@

.PHONY: all test compare bench clean check-html check-script
# build/mdyast too, though the checks do not use it: every probe reached for it
# by hand at some point, found yesterday's binary, and reported a bug that had
# already been fixed. Building it here costs a second and stops that.
test: build/smoke build/html build/script build/yaml build/data build/mdyast build/linkify
	@./build/smoke
	@./build/html
	@./build/script
	@./build/yaml
	@./build/data

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
