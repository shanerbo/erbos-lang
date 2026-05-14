CC = cc
CFLAGS = -Wall -Wextra -std=c11
SRC = src/main.c src/lexer.c src/parser.c src/checker.c src/optimizer.c src/codegen.c src/irgen.c src/regalloc.c src/iremit.c
OUT = erbos

all: $(OUT)

$(OUT): $(SRC) src/*.h
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

clean:
	rm -f $(OUT) *.o *.s examples/*.s examples/*.o

test: $(OUT) test-pass test-fail test-runtime test-framework
	@echo ""
	@echo "All tests passed."

test-pass: $(OUT)
	@echo "=== Passing examples ==="
	@fail=0; \
	for f in examples/*.ptt examples/leetcode/*.ptt; do \
		b=$$(basename $$f); \
		case $$b in nomut_test.ptt|oob_test.ptt|move_test.ptt) continue;; esac; \
		if [ -f "$$f.expected" ]; then \
			actual=$$(./$(OUT) run "$$f" 2>/dev/null); \
			expected=$$(cat "$$f.expected"); \
			if [ "$$actual" = "$$expected" ]; then \
				echo "  OK:   $$f (output verified)"; \
			else \
				echo "  FAIL: $$f (output mismatch)"; fail=1; \
			fi; \
		else \
			if ! ./$(OUT) run "$$f" > /dev/null 2>&1; then \
				echo "  FAIL: $$b"; fail=1; \
			else \
				echo "  OK:   $$b"; \
			fi; \
		fi; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some passing tests failed"; exit 1)

test-fail: $(OUT)
	@echo "=== Expected compile failures ==="
	@fail=0; \
	for f in tests/errors/*.ptt; do \
		b=$$(basename $$f); \
		case $$b in negative_index.ptt) continue;; esac; \
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
	for f in examples/oob_test.ptt tests/errors/negative_index.ptt; do \
		b=$$(basename $$f); \
		if ./$(OUT) run "$$f" > /dev/null 2>&1; then \
			echo "  FAIL (should panic): $$b"; fail=1; \
		else \
			echo "  OK (panicked):       $$b"; \
		fi; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some failure tests did not error"; exit 1)

.PHONY: all clean test test-pass test-fail test-runtime test-framework

test-runtime:
	@echo "=== Runtime C tests ==="
	@$(CC) $(CFLAGS) -Isrc -pthread -o tests/test_runtime tests/test_runtime.c src/runtime.c src/runtime_asm.s
	@./tests/test_runtime > /dev/null && echo "  OK:   test_runtime" || echo "  FAIL: test_runtime"
	@$(CC) $(CFLAGS) -Isrc -o tests/test_channel tests/test_channel.c src/runtime.c src/channel.c src/runtime_asm.s
	@./tests/test_channel > /dev/null && echo "  OK:   test_channel" || echo "  FAIL: test_channel"
	@rm -f tests/test_runtime tests/test_channel

test-framework: $(OUT)
	@echo "=== Framework tests (erbos test) ==="
	@fail=0; \
	for f in tests/test_*.ptt examples/leetcode/tests/test_*.ptt; do \
		b=$$(basename $$f); \
		if ! ./$(OUT) test "$$f" > /dev/null 2>&1; then \
			echo "  FAIL: $$b"; fail=1; \
		else \
			echo "  OK:   $$b"; \
		fi; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some framework tests failed"; exit 1)
