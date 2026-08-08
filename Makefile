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
# picked up. src/config.c lands in phase 3, src/event.c in phase 4, and
# both are linked immediately -- audio/decoder.c, audio/engine.c,
# queue.c, ui/*, library/* land in their own phases and get added here
# then.
# src/audio/ring.c (phase 1) moves in here in phase 5: audio/sink.c is
# its first real caller (pcm_ring/viz_ring), so ring.c can no longer sit
# out of the main link the way it did while only tools/ring_stress.c
# used it.
# src/audio/trace.c (phase 5b) is wired in immediately, same as ring.c
# and sink.c before it -- audio/sink.c's callbacks are its only callers.
# src/audio/decoder.c (phase 6) is the AudioSource over ffmpeg -- wired
# in immediately for the same reason: --decode (main.c) is its only
# caller in this phase, and an unlinked ffmpeg-facing file would go
# unanalysed by `make analyze` until someone remembered to move it.
SRCS = src/main.c src/config.c src/event.c src/audio/ring.c src/audio/sink.c \
       src/audio/trace.c src/audio/decoder.c

# Sources that exist and must be statically analysed, but are not yet
# linked into the kurono binary. Move each entry into SRCS when its
# phase wires it in. Empty as of phase 5 -- audio/ring.c moved into SRCS
# above; kept as a named list (not deleted) so a future orphaned file is
# visible here rather than silently unanalysed.
UNLINKED_SRCS =

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
# audio path specifically. -Wconversion does not reach vendor/miniaudio.h's
# own implementation inside audio/sink.c -- that block is bracketed by a
# #pragma GCC diagnostic push/ignored/pop around the #include, scoped to
# exactly the vendored code, not ours (see the top of sink.c).
# audio/trace.c (phase 5b) joins the same rule: it is the third SPSC ring
# instance and shares the same scrutiny as ring.c and sink.c.
build/default/audio/ring.o build/default/audio/sink.o build/default/audio/trace.o \
build/asan/audio/ring.o    build/asan/audio/sink.o    build/asan/audio/trace.o \
build/tsan/audio/ring.o    build/tsan/audio/sink.o    build/tsan/audio/trace.o: EXTRA_WARN = -Wvla -Wshadow -Wconversion

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
# tools/ring_stress.c against src/audio/ring.c directly, compiled here
# into build/tsan/audio/ring.o -- the same object path (and so the same
# EXTRA_WARN rule above) that linking kurono-tsan itself produces, now
# that ring.c is in SRCS (phase 5).
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
