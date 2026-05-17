CC = cc
CFLAGS = -Wall -Wextra -std=c11

# Compiler frontend (the `erbos` binary).
COMPILER_SRC = compiler/main.c compiler/lexer.c compiler/parser.c compiler/monomorph.c compiler/checker.c compiler/optimizer.c compiler/runtime_emit.c compiler/irgen.c compiler/iropt.c compiler/regalloc.c compiler/iremit.c

# Green-thread runtime + channels — a separate C library that's
# only linked into the runtime / channel C tests today (not yet
# wired into compiled .ptt output).
RUNTIME_SRC = compiler/runtime/runtime.c compiler/runtime/runtime_asm.s
CHANNEL_SRC = compiler/runtime/channel.c

OUT = erbos

all: $(OUT)

$(OUT): $(COMPILER_SRC) compiler/*.h
	$(CC) $(CFLAGS) -o $(OUT) $(COMPILER_SRC)

clean:
	rm -f $(OUT) *.o *.s examples/*.s examples/*.o tests/ir/*.s tests/ir/*.o
	rm -f $(addprefix tests/ir/,$(notdir $(basename $(wildcard tests/ir/*.ptt))))

test: $(OUT) test-pass test-fail test-runtime test-framework test-ir
	@echo ""
	@echo "All tests passed."

test-pass: $(OUT)
	@echo "=== Passing examples ==="
	@fail=0; \
	for f in examples/*.ptt; do \
		b=$$(basename $$f); \
		case $$b in nomut_test.ptt|oob_test.ptt|move_test.ptt) continue;; esac; \
		if ! ./$(OUT) run "$$f" > /dev/null 2>&1; then \
			echo "  FAIL: $$b"; fail=1; \
		else \
			echo "  OK:   $$b"; \
		fi; \
	done; \
	echo "=== leetcode library compile check ==="; \
	for f in tests/lib/leetcode/*.ptt; do \
		b=$$(basename $$f); \
		if ! ./$(OUT) ir "$$f" > /dev/null 2>&1; then \
			echo "  FAIL: $$b (won't compile to IR)"; fail=1; \
		else \
			echo "  OK:   $$b"; \
		fi; \
		rm -f $${b%.ptt}.s; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some passing tests failed"; exit 1)

test-fail: $(OUT)
	@echo "=== Expected compile failures ==="
	@fail=0; \
	for f in tests/errors/*.ptt; do \
		b=$$(basename $$f); \
		case $$b in negative_index.ptt|empty_pop_panics.ptt) continue;; esac; \
		if ./$(OUT) "$$f" > /dev/null 2>&1; then \
			echo "  FAIL (should error): $$b"; fail=1; \
		else \
			echo "  OK (errored):        $$b"; \
		fi; \
	done; \
	for f in examples/nomut_test.ptt examples/move_test.ptt; do \
		b=$$(basename $$f); \
		if ./$(OUT) "$$f" > /dev/null 2>&1; then \
			echo "  FAIL (should error): $$b"; fail=1; \
		else \
			echo "  OK (errored):        $$b"; \
		fi; \
	done; \
	echo "=== Expected runtime panics ==="; \
	for f in examples/oob_test.ptt tests/errors/negative_index.ptt tests/errors/empty_pop_panics.ptt; do \
		b=$$(basename $$f); \
		if ./$(OUT) run "$$f" > /dev/null 2>&1; then \
			echo "  FAIL (should panic): $$b"; fail=1; \
		else \
			echo "  OK (panicked):       $$b"; \
		fi; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some failure tests did not error"; exit 1)

.PHONY: all clean test test-pass test-fail test-runtime test-framework test-ir

# Run every tests/ir/*.ptt through the framework runner
# (`erbos test`) at each optimization level. Each test file uses
# `test "name" { ... assert(...) }` blocks; non-zero exit on any
# level means an assertion failed or the program crashed. The
# multi-level sweep is what guarantees no iropt pass changed
# observable program behaviour.
test-ir: $(OUT)
	@echo "=== IR backend regression tests (matrix: -O0/-O1/-O2) ==="
	@fail=0; \
	for f in tests/ir/*.ptt; do \
		b=$$(basename $$f .ptt); \
		for level in -O0 -O1 -O2; do \
			if ./$(OUT) $$level test "$$f" > /dev/null 2>&1; then \
				echo "  OK:   $$b $$level"; \
			else \
				echo "  FAIL: $$b $$level"; \
				fail=1; \
			fi; \
		done; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some IR backend tests failed"; exit 1)

test-runtime:
	@echo "=== Runtime C tests ==="
	@$(CC) $(CFLAGS) -Icompiler/runtime -pthread -o tests/compiler/test_runtime tests/compiler/test_runtime.c $(RUNTIME_SRC)
	@./tests/compiler/test_runtime > /dev/null && echo "  OK:   test_runtime" || echo "  FAIL: test_runtime"
	@$(CC) $(CFLAGS) -Icompiler/runtime -o tests/compiler/test_channel tests/compiler/test_channel.c $(RUNTIME_SRC) $(CHANNEL_SRC)
	@./tests/compiler/test_channel > /dev/null && echo "  OK:   test_channel" || echo "  FAIL: test_channel"
	@rm -f tests/compiler/test_runtime tests/compiler/test_channel

test-framework: $(OUT)
	@echo "=== Framework tests (erbos test) ==="
	@fail=0; \
	for f in tests/test_*.ptt tests/leetcode/test_*.ptt; do \
		b=$$(basename $$f); \
		if ! ./$(OUT) test "$$f" > /dev/null 2>&1; then \
			echo "  FAIL: $$b"; fail=1; \
		else \
			echo "  OK:   $$b"; \
		fi; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some framework tests failed"; exit 1)
