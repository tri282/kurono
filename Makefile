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
# src/audio/engine.c (phase 7) is T2, wired in the same way -- --play
# (main.c) is its only caller in this phase.
# src/queue.c (phase 9) is pure and dependency-free (DESIGN.md III.2: it
# depends on nothing, not even audio/ or library/) and has no caller in
# the linked binary yet -- ui/queueview.c and audio/engine.c's queue
# policy land in later phases. It is wired into SRCS now anyway, same as
# every prior phase's file, so it is not silently unanalysed by
# `make analyze` between now and whichever phase first calls it.
# src/ui/theme.c and src/ui/keymap.c (phase 10) are the two tables the
# whole UI draws and dispatches through. Wired in immediately: main.c
# holds action_dispatch (the stub phases 11-17 fill in) and calls
# action_name, so keymap.o is already linked; theme.o has no caller until
# ui/tui.c in phase 11 and is listed here anyway, same as every prior
# phase's file, so it is not silently unanalysed in the meantime.
# src/ui/browser.c (phase 12) is the browser widget plus the M1
# filesystem BrowserSource. The widget and the source live in one file
# on purpose: the source is the smaller half, and phase 16 adds a
# library/query.c source beside it rather than replacing this one.
SRCS = src/main.c src/config.c src/event.c src/audio/ring.c src/audio/sink.c \
       src/audio/trace.c src/audio/decoder.c src/audio/engine.c src/queue.c \
       src/ui/theme.c src/ui/keymap.c src/ui/tui.c src/ui/browser.c

# Sources that exist and must be statically analysed, but are not yet
# linked into the kurono binary. Move each entry into SRCS when its
# phase wires it in. Empty as of phase 5 -- audio/ring.c moved into SRCS
# above; kept as a named list (not deleted) so a future orphaned file is
# visible here rather than silently unanalysed.
UNLINKED_SRCS =

OBJS_DEFAULT = $(SRCS:src/%.c=build/default/%.o)
OBJS_ASAN    = $(SRCS:src/%.c=build/asan/%.o)
OBJS_TSAN    = $(SRCS:src/%.c=build/tsan/%.o)

.PHONY: all release debug asan tsan analyze check check-layering check-flush \
        check-theme check-deps ring-stress queue-test keymap-test install \
        uninstall clean

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
# audio/engine.c (phase 7) joins too -- it is T2, on the audio path,
# and holds the pcm_ring producer side.
build/default/audio/ring.o build/default/audio/sink.o build/default/audio/trace.o build/default/audio/engine.o \
build/asan/audio/ring.o    build/asan/audio/sink.o    build/asan/audio/trace.o    build/asan/audio/engine.o \
build/tsan/audio/ring.o    build/tsan/audio/sink.o    build/tsan/audio/trace.o    build/tsan/audio/engine.o: EXTRA_WARN = -Wvla -Wshadow -Wconversion

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

# amendment-02-trace-ring.md decision 9 / DESIGN.md IV.8.b's generalising
# rule: ring_reset(pcm_ring) may only ever be called from inside
# pcm_flush_and_resync (src/audio/engine.c). Matches ring_reset( calls
# whose argument mentions pcm_ring specifically -- not every ring_reset(
# call (boundary_ring's own reset is unrestricted) and not ring.c's own
# function definition, `ring_reset(Ring * r)`, whose parameter name does
# not contain "pcm_ring" -- then counts that pattern against the count
# inside pcm_flush_and_resync's body (the lines from its column-0
# signature to its column-0 closing-brace comment) and fails if they
# differ. A third call site, or a call site outside the function, no
# longer builds. The same mechanical move check-layering makes for the
# dependency graph, applied to this invariant instead. Like
# check-layering, this is a grep and can be defeated by a macro or an
# alias -- catches the accidental case, not the adversarial one
# (amendment-02-trace-ring.md residual risks).
check-flush:
	@total=$$(grep -rnE 'ring_reset\([^)]*pcm_ring' src --include=*.c | wc -l | tr -d ' '); \
	 infn=$$(awk '/^pcm_flush_and_resync\(/{f=1} f && /ring_reset\([^)]*pcm_ring/{c++} /^} \/\* pcm_flush_and_resync \*\//{f=0} END{print c+0}' src/audio/engine.c); \
	 if [ "$$total" != "$$infn" ]; then \
	   echo "check-flush: ring_reset(...pcm_ring...) called outside pcm_flush_and_resync (src/audio/engine.c)"; \
	   echo "  $$total occurrence(s) in src/, $$infn inside the function:"; \
	   grep -rnE 'ring_reset\([^)]*pcm_ring' src --include=*.c; \
	   exit 1; \
	 fi; \
	 echo "check-flush ok ($$total ring_reset(...pcm_ring...) call(s), all inside pcm_flush_and_resync)"

# The mechanical form of CLAUDE.md's "no literal colours in draw code"
# rule (DESIGN.md VIII.5, phase-10-theme-keymap.md III): a 6-digit hex
# literal under src/ui/ may only appear in theme.c, which is the one file
# that owns the palette. Same class of check as check-layering: a grep,
# defeated by a macro or a decimal literal, catching the accidental case
# and not the adversarial one.
check-theme:
	@! grep -rn '0x[0-9a-fA-F]\{6\}' src/ui --include=*.c \
	    | grep -v '^src/ui/theme.c:' \
	  || { echo "literal colour outside src/ui/theme.c"; exit 1; }
	@echo "check-theme ok"

# Aggregate for the three grep-based invariants (DESIGN.md III.1, IV.8.b,
# VIII.5). Each also runs standalone; this is the one-word form for a
# phase's verify block.
check: check-layering check-flush check-theme

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

# Pure, dependency-free (DESIGN.md III.2, XII.2 testable tier): no
# PKGS, no check-deps, no sanitiser by default -- the ASan+UBSan variant
# in phase-9-queue.md III is invoked directly with gcc, not through this
# target. Links tools/queue_test.c against src/queue.c, compiled here
# into build/default/queue.o -- the same object path (and object) that
# linking the default `kurono` binary produces, now that queue.c is in
# SRCS (phase 9).
queue-test: tools/queue_test

tools/queue_test: build/default/tools/queue_test.o build/default/queue.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

build/default/tools/%.o: tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Pure, dependency-free (DESIGN.md XII.2 testable tier): no PKGS, no
# check-deps, no terminal. vendor/termbox2.h is included by ui/keymap.h
# and ui/theme.h for its declarations only -- TB_IMPL is defined nowhere
# in this link, so there is no termbox2 implementation to link against.
# Same object paths (and objects) the default kurono link produces.
keymap-test: tools/keymap_test

tools/keymap_test: build/default/tools/keymap_test.o build/default/ui/keymap.o \
                   build/default/ui/theme.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

install: release
	install -d $(BINDIR)
	install -m 755 kurono $(BINDIR)/kurono

uninstall:
	rm -f $(BINDIR)/kurono

clean:
	rm -rf build kurono kurono-asan kurono-tsan tools/ring_stress \
	       tools/queue_test tools/keymap_test
