CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -D_GNU_SOURCE -Iinclude
LDFLAGS = -lm

USE_SQLITE ?= 1

ifeq ($(USE_SQLITE),1)
CFLAGS  += -DUSE_SQLITE
LDFLAGS += -lsqlite3
endif

# Windows (MinGW-w64, e.g. MSYS2's MINGW64 shell): executables get .exe, and
# the POSIX regex API the regexp_* natives use comes from a separate library,
# linked as libgnurx (MSYS2's libsystre package provides it under that name).
ifeq ($(OS),Windows_NT)
EXE      = .exe
LDFLAGS += -lgnurx
endif

SRCDIR  = src
OBJDIR  = build
BINDIR  = bin

SOURCES     = $(wildcard $(SRCDIR)/*.c)

ifeq ($(USE_SQLITE),0)
SOURCES := $(filter-out $(SRCDIR)/sqlite_driver.c,$(SOURCES))
endif

OBJECTS     = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
LIB_OBJECTS = $(filter-out $(OBJDIR)/main.o,$(OBJECTS))

TARGET      = $(BINDIR)/auspex$(EXE)

.PHONY: all clean test examples-test fuzz fuzz-run fuzz-replay install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR) $(BINDIR):
	mkdir -p $@

test: $(TARGET)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_value tests/test_value.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_chunk tests/test_chunk.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_compiler tests/test_compiler.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_pager tests/test_pager.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_btree tests/test_btree.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_vm tests/test_vm.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_sql tests/test_sql.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_sql_engine tests/test_sql_engine.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_lexer tests/test_lexer.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_parser tests/test_parser.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_cli tests/test_cli.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_natives tests/test_natives.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_strings tests/test_strings.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_file_io tests/test_file_io.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_gc tests/test_gc.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_repl tests/test_repl.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_typecheck tests/test_typecheck.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_errors tests/test_errors.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase2 tests/test_phase2.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase3 tests/test_phase3.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase4 tests/test_phase4.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase5 tests/test_phase5.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase6 tests/test_phase6.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase7 tests/test_phase7.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase10 tests/test_phase10.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase11 tests/test_phase11.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase12 tests/test_phase12.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase13 tests/test_phase13.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_packages tests/test_packages.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_rename_compat tests/test_rename_compat.c $(LIB_OBJECTS) $(LDFLAGS)
ifeq ($(USE_SQLITE),1)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase1 tests/test_phase1.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase8 tests/test_phase8.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_phase9 tests/test_phase9.c $(LIB_OBJECTS) $(LDFLAGS)
	$(CC) $(CFLAGS) -Itests -o $(BINDIR)/test_sqlite tests/test_sqlite.c $(LIB_OBJECTS) $(LDFLAGS)
endif
	$(BINDIR)/test_value
	$(BINDIR)/test_chunk
	$(BINDIR)/test_compiler
	$(BINDIR)/test_pager
	$(BINDIR)/test_btree
	$(BINDIR)/test_vm
	$(BINDIR)/test_sql
	$(BINDIR)/test_sql_engine
	$(BINDIR)/test_lexer
	$(BINDIR)/test_parser
	$(BINDIR)/test_cli
	$(BINDIR)/test_natives
	$(BINDIR)/test_strings
	$(BINDIR)/test_file_io
	$(BINDIR)/test_gc
	$(BINDIR)/test_repl
	$(BINDIR)/test_typecheck
	$(BINDIR)/test_errors
	$(BINDIR)/test_phase2
	$(BINDIR)/test_phase3
	$(BINDIR)/test_phase4
	$(BINDIR)/test_phase5
	$(BINDIR)/test_phase6
	$(BINDIR)/test_phase7
	$(BINDIR)/test_phase10
	$(BINDIR)/test_phase11
	$(BINDIR)/test_phase12
	$(BINDIR)/test_phase13
	$(BINDIR)/test_packages
	$(BINDIR)/test_rename_compat
ifeq ($(USE_SQLITE),1)
	$(BINDIR)/test_phase1
	$(BINDIR)/test_phase8
	$(BINDIR)/test_phase9
	$(BINDIR)/test_sqlite
endif
	$(MAKE) --no-print-directory fuzz-replay
	$(MAKE) --no-print-directory examples-test

# Run every runnable example (examples/*.apx and examples/phases/*.apx) and
# require exit 0; see tests/run_examples.sh for
# the `// smoke:` directives an example can use (arguments, setup, skips).
examples-test: $(TARGET)
	tests/run_examples.sh $(TARGET)

# Fuzzing (tests/fuzz). `make fuzz` builds libFuzzer targets for the lexer,
# parser and conditional-compilation preprocessor with clang; `make fuzz-run`
# runs each for FUZZ_TIME seconds, writing new corpus entries and crash
# inputs under build/fuzz. `make fuzz-replay` runs the same targets over the
# seed corpus and saved regressions with $(CC), no libFuzzer needed.
FUZZ_TARGETS = lexer parser preprocessor
FUZZ_CC     ?= clang
FUZZ_TIME   ?= 60
FUZZ_FLAGS  ?=
# FUZZ_LEAKS=0 turns off leak reports, both per input and at process exit.
FUZZ_LEAKS  ?= 1
ifeq ($(FUZZ_LEAKS),0)
FUZZ_ENV     = ASAN_OPTIONS=detect_leaks=0
FUZZ_FLAGS  += -detect_leaks=0
endif
FUZZ_CFLAGS  = -g -O1 -std=c99 -D_GNU_SOURCE -Iinclude -Itests/fuzz \
               -fno-omit-frame-pointer -fsanitize=address,undefined
FUZZ_SOURCES = $(filter-out $(SRCDIR)/main.c $(SRCDIR)/sqlite_driver.c,$(wildcard $(SRCDIR)/*.c))

FUZZ_SEEDS_lexer        = examples tests/fixtures tests/fuzz/regressions/lexer
FUZZ_SEEDS_parser       = examples tests/fixtures tests/fuzz/corpus/parser tests/fuzz/regressions/parser
FUZZ_SEEDS_preprocessor = tests/fuzz/corpus/preprocessor tests/fuzz/regressions/preprocessor

fuzz: $(addprefix $(BINDIR)/fuzz_,$(FUZZ_TARGETS))

$(BINDIR)/fuzz_%: tests/fuzz/fuzz_%.c tests/fuzz/fuzz_common.h $(FUZZ_SOURCES) | $(BINDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -fsanitize=fuzzer -o $@ $< $(FUZZ_SOURCES) -lm

fuzz-run: fuzz
	$(foreach t,$(FUZZ_TARGETS),mkdir -p $(OBJDIR)/fuzz/corpus/$(t) $(OBJDIR)/fuzz/artifacts && \
	    $(FUZZ_ENV) $(BINDIR)/fuzz_$(t) -max_total_time=$(FUZZ_TIME) $(FUZZ_FLAGS) \
	        -artifact_prefix=$(OBJDIR)/fuzz/artifacts/$(t)- \
	        $(OBJDIR)/fuzz/corpus/$(t) $(FUZZ_SEEDS_$(t)) && ) true

fuzz-replay: $(addprefix $(BINDIR)/replay_,$(FUZZ_TARGETS))
	$(foreach t,$(FUZZ_TARGETS),$(BINDIR)/replay_$(t) $(FUZZ_SEEDS_$(t)) && ) true

$(BINDIR)/replay_%: tests/fuzz/fuzz_%.c tests/fuzz/fuzz_replay.c tests/fuzz/fuzz_common.h $(LIB_OBJECTS) | $(BINDIR)
	$(CC) $(CFLAGS) -Itests/fuzz -o $@ tests/fuzz/fuzz_replay.c $< $(LIB_OBJECTS) $(LDFLAGS)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

PREFIX     ?= /usr/local
DESTDIR    ?=
INSTALLBIN = $(DESTDIR)$(PREFIX)/bin
INSTALLMAN = $(DESTDIR)$(PREFIX)/share/man/man1

# `mypl` stays as a symlink for MyPL users until v0.4.0.
install: $(TARGET) auspex.1
	mkdir -p $(INSTALLBIN) $(INSTALLMAN)
	cp $(TARGET) $(INSTALLBIN)/auspex
	ln -sf auspex $(INSTALLBIN)/mypl
	cp auspex.1 $(INSTALLMAN)/auspex.1

uninstall:
	rm -f $(INSTALLBIN)/auspex $(INSTALLBIN)/mypl $(INSTALLMAN)/auspex.1
