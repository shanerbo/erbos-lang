CC = cc
CFLAGS = -Wall -Wextra -std=c11
SRC = src/main.c src/lexer.c src/parser.c src/checker.c src/codegen.c
OUT = erbos

all: $(OUT)

$(OUT): $(SRC) src/*.h
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

clean:
	rm -f $(OUT) *.o *.s examples/*.s examples/*.o

test: $(OUT) test-pass test-fail
	@echo ""
	@echo "All tests passed."

test-pass: $(OUT)
	@echo "=== Passing examples ==="
	@fail=0; \
	for f in examples/*.erbos; do \
		b=$$(basename $$f); \
		case $$b in nomut_test.erbos|oob_test.erbos|move_test.erbos) continue;; esac; \
		if ! ./$(OUT) run "$$f" > /dev/null 2>&1; then \
			echo "  FAIL: $$b"; fail=1; \
		else \
			echo "  OK:   $$b"; \
		fi; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some passing tests failed"; exit 1)

test-fail: $(OUT)
	@echo "=== Expected compile failures ==="
	@fail=0; \
	for f in tests/errors/*.erbos; do \
		b=$$(basename $$f); \
		if ./$(OUT) "$$f" > /dev/null 2>&1; then \
			echo "  FAIL (should error): $$b"; fail=1; \
		else \
			echo "  OK (errored):        $$b"; \
		fi; \
	done; \
	for f in examples/nomut_test.erbos examples/move_test.erbos; do \
		b=$$(basename $$f); \
		if ./$(OUT) "$$f" > /dev/null 2>&1; then \
			echo "  FAIL (should error): $$b"; fail=1; \
		else \
			echo "  OK (errored):        $$b"; \
		fi; \
	done; \
	echo "=== Expected runtime panics ==="; \
	for f in examples/oob_test.erbos; do \
		b=$$(basename $$f); \
		if ./$(OUT) run "$$f" > /dev/null 2>&1; then \
			echo "  FAIL (should panic): $$b"; fail=1; \
		else \
			echo "  OK (panicked):       $$b"; \
		fi; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some failure tests did not error"; exit 1)

.PHONY: all clean test test-pass test-fail
