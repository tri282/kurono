CC = gcc
# _POSIX_C_SOURCE is required: strict -std=c11 does not declare strdup(),
# which silently truncates the returned pointer on 64-bit.
STD  = -std=c11 -D_POSIX_C_SOURCE=200809L
WARN = -Wall -Wextra
# TB_OPT_ATTR_W defaults to 16 in termbox2, which does not even define
# TB_OUTPUT_TRUECOLOR. Every target must carry this, including ones that
# do not yet compile a translation unit including termbox2.h -- see
# DESIGN.md VIII.1 and plan.md VI.5.
DEFS = -DTB_OPT_ATTR_W=32
CFLAGS = $(STD) $(WARN) $(DEFS) -g
INCLUDES = -Iinclude -Ivendor

PREFIX ?= $(HOME)/.local
BINDIR = $(PREFIX)/bin

PKGS = libavformat libavcodec libavutil libswresample sqlite3 taglib_c
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)
LDLIBS = $(PKG_LIBS) -lpthread -lm -ldl

# Explicit, not a wildcard: an orphaned file must be visible, not silently
# picked up. src/config.c lands in phase 3 and is linked immediately --
# event.c, audio/sink.c, audio/decoder.c, audio/engine.c, queue.c, ui/*,
# library/* land in their own phases and get added here then.
# src/audio/ring.c (phase 1) is not yet linked into the kurono binary --
# nothing wires it in before audio/engine.c (phase 7) -- so it is not in
# SRCS; it is built separately by the ring-stress target below.
SRCS = src/main.c src/config.c

# Sources that exist and must be statically analysed, but are not yet
# linked into the kurono binary. src/audio/ring.c is the phase-1 ring and
# is the single most analysis-worthy file in the tree, so `make analyze`
# must not skip it just because nothing links it before phase 7. Move
# each entry into SRCS when its phase wires it in.
UNLINKED_SRCS = src/audio/ring.c

OBJS_DEFAULT = $(SRCS:src/%.c=build/default/%.o)
OBJS_ASAN    = $(SRCS:src/%.c=build/asan/%.o)
OBJS_TSAN    = $(SRCS:src/%.c=build/tsan/%.o)

.PHONY: all release debug asan tsan analyze check-layering check-deps \
        ring-stress install uninstall clean

all: kurono

# release and debug produce the same kurono binary out of the same
# build/default tree as `all`, so both must start from a clean tree or a
# stale object built with different flags is silently reused. Recursing
# rather than declaring `clean kurono` as prerequisites: make does not
# order prerequisites, so under -j the clean can run concurrently with
# the compile and delete objects mid-build.
#
# optimized, assertions stripped
release:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory kurono CFLAGS="$(STD) $(WARN) $(DEFS) -O2 -DNDEBUG"

# extra diagnostics for development
debug:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory kurono CFLAGS="$(CFLAGS) -Og -fno-omit-frame-pointer"

# ASan/UBSan and TSan are mutually exclusive and must never share an
# object tree with each other or with the default build -- linking
# object files built against one sanitiser runtime into a binary built
# for another silently produces the wrong runtime. Separate build/<mode>
# directories are the one deliberate divergence from better-ls's in-tree
# %.o idiom (plan.md phase-2-makefile.md III.3).
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: kurono-asan

tsan: CFLAGS += -fsanitize=thread
tsan: kurono-tsan

# check-deps is order-only (|): it must run, and fail, before any link
# step, but it is .PHONY and therefore permanently out of date -- as a
# normal prerequisite it would force a relink on every make even with no
# source change.
kurono: $(OBJS_DEFAULT) | check-deps
	$(CC) $(CFLAGS) $(INCLUDES) $(PKG_CFLAGS) -o $@ $(OBJS_DEFAULT) $(LDLIBS)

kurono-asan: $(OBJS_ASAN) | check-deps
	$(CC) $(CFLAGS) $(INCLUDES) $(PKG_CFLAGS) -o $@ $(OBJS_ASAN) $(LDLIBS)

kurono-tsan: $(OBJS_TSAN) | check-deps
	$(CC) $(CFLAGS) $(INCLUDES) $(PKG_CFLAGS) -o $@ $(OBJS_TSAN) $(LDLIBS)

build/default/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_WARN) $(INCLUDES) $(PKG_CFLAGS) -c $< -o $@

build/asan/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_WARN) $(INCLUDES) $(PKG_CFLAGS) -c $< -o $@

build/tsan/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_WARN) $(INCLUDES) $(PKG_CFLAGS) -c $< -o $@

# DESIGN.md XII.5 / plan.md VI.1: -Wvla -Wshadow -Wconversion for the
# audio path specifically. Listed against every mode's object path for
# both ring.o and sink.o even though audio/sink.c does not exist until
# phase 5 -- a target-specific variable on a file that is never built is
# inert, and this way the rule does not need editing when sink.c lands.
build/default/audio/ring.o build/default/audio/sink.o \
build/asan/audio/ring.o    build/asan/audio/sink.o \
build/tsan/audio/ring.o    build/tsan/audio/sink.o: EXTRA_WARN = -Wvla -Wshadow -Wconversion

# pkg-config failure must name the missing MODULE, never a distro
# package -- module names are the stable cross-distro interface
# (DESIGN.md XIII). Order-only-in-spirit: invoked as a normal
# prerequisite of every linked binary so it runs, and fails, before the
# link step that would otherwise error on missing flags with a less
# useful message.
check-deps:
	@for p in $(PKGS); do \
	  pkg-config --exists $$p || { \
	    echo "missing pkg-config module: $$p"; \
	    echo "see README.md section I for the install command"; exit 1; }; \
	done

# The mechanical form of the acyclicity rule (DESIGN.md III.1): no file
# under src/audio/ or src/library/ may #include anything under
# include/ui/.
check-layering:
	@! grep -rn 'include/ui/' src/audio src/library 2>/dev/null \
	  || { echo "layering violation: audio/ or library/ includes ui/"; exit 1; }
	@echo "layering ok"

# -fanalyzer over every SRC, no output produced. DEFS/TB_OPT_ATTR_W=32
# carried even though no SRC yet includes termbox2.h -- see the DEFS
# comment above.
analyze:
	@for f in $(SRCS) $(UNLINKED_SRCS); do \
	  echo "analyzing $$f"; \
	  $(CC) $(STD) $(WARN) $(DEFS) $(INCLUDES) $(PKG_CFLAGS) -fanalyzer -fsyntax-only $$f || exit 1; \
	done

# Pure, dependency-free, TSan-checkable: no PKGS, no check-deps. Links
# tools/ring_stress.c against src/audio/ring.c directly -- ring.c is not
# part of SRCS/kurono's own link yet (see the SRCS comment above), so it
# is compiled here into its own build/tsan/audio/ring.o, which is the
# same object the EXTRA_WARN rule above already covers.
# -fsanitize=thread is set on tools/ring_stress itself, not on the
# ring-stress phony alias, so that `make tools/ring_stress` typed
# directly cannot build non-TSan objects into build/tsan/.
ring-stress: tools/ring_stress

tools/ring_stress: CFLAGS += -fsanitize=thread
tools/ring_stress: build/tsan/tools/ring_stress.o build/tsan/audio/ring.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lpthread

build/tsan/tools/%.o: tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: release
	install -d $(BINDIR)
	install -m 755 kurono $(BINDIR)/kurono

uninstall:
	rm -f $(BINDIR)/kurono

clean:
	rm -rf build kurono kurono-asan kurono-tsan tools/ring_stress
