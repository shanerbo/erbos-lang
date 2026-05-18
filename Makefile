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
	rm -f tests/leaks/*.s tests/leaks/*.o tests/leaks/named_arg_string_literal
	rm -f named_arg_string_literal.s named_arg_string_literal named_arg_string_literal.o
	rm -f named_arg_nested_string_literal.s named_arg_nested_string_literal named_arg_nested_string_literal.o
	rm -f heap_slot_drop_emits_drop.s heap_slot_drop_emits_drop heap_slot_drop_emits_drop.o

test: $(OUT) test-pass test-fail test-runtime test-framework test-ir test-paths test-leaks
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
		case $$b in negative_index.ptt|empty_pop_panics.ptt|option_unwrap_none_panics.ptt|result_unwrap_err_panics.ptt|result_unwrap_err_on_ok_panics.ptt|list_remove_out_of_range_panics.ptt|list_insert_out_of_range_panics.ptt|stack_pop_empty_panics.ptt|stack_peek_empty_panics.ptt|queue_pop_empty_panics.ptt|queue_front_empty_panics.ptt|deque_pop_front_empty_panics.ptt|deque_pop_back_empty_panics.ptt|deque_front_empty_panics.ptt|deque_back_empty_panics.ptt|deque_get_oob_panics.ptt|string_slice_oob_panics.ptt|string_split_empty_sep_panics.ptt|string_replace_empty_from_panics.ptt|string_builder_push_byte_invalid_panics.ptt|byte_buffer_get_oob_panics.ptt|byte_buffer_set_oob_panics.ptt|arena_get_oob_panics.ptt|arena_set_oob_panics.ptt|ring_buffer_pop_empty_panics.ptt|ring_buffer_peek_empty_panics.ptt|math_pow_negative_panics.ptt|math_pow_mod_bad_args_panics.ptt|math_sqrt_negative_panics.ptt|map_get_missing_key_panics.ptt|pool_get_stale_panics.ptt) continue;; esac; \
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
	for f in examples/oob_test.ptt tests/errors/negative_index.ptt tests/errors/empty_pop_panics.ptt tests/errors/option_unwrap_none_panics.ptt tests/errors/result_unwrap_err_panics.ptt tests/errors/result_unwrap_err_on_ok_panics.ptt tests/errors/list_remove_out_of_range_panics.ptt tests/errors/list_insert_out_of_range_panics.ptt tests/errors/stack_pop_empty_panics.ptt tests/errors/stack_peek_empty_panics.ptt tests/errors/queue_pop_empty_panics.ptt tests/errors/queue_front_empty_panics.ptt tests/errors/deque_pop_front_empty_panics.ptt tests/errors/deque_pop_back_empty_panics.ptt tests/errors/deque_front_empty_panics.ptt tests/errors/deque_back_empty_panics.ptt tests/errors/deque_get_oob_panics.ptt tests/errors/string_slice_oob_panics.ptt tests/errors/string_split_empty_sep_panics.ptt tests/errors/string_replace_empty_from_panics.ptt tests/errors/string_builder_push_byte_invalid_panics.ptt tests/errors/byte_buffer_get_oob_panics.ptt tests/errors/byte_buffer_set_oob_panics.ptt tests/errors/arena_get_oob_panics.ptt tests/errors/arena_set_oob_panics.ptt tests/errors/ring_buffer_pop_empty_panics.ptt tests/errors/ring_buffer_peek_empty_panics.ptt tests/errors/math_pow_negative_panics.ptt tests/errors/math_pow_mod_bad_args_panics.ptt tests/errors/math_sqrt_negative_panics.ptt tests/errors/map_get_missing_key_panics.ptt tests/errors/pool_get_stale_panics.ptt; do \
		b=$$(basename $$f); \
		if ./$(OUT) run "$$f" > /dev/null 2>&1; then \
			echo "  FAIL (should panic): $$b"; fail=1; \
		else \
			echo "  OK (panicked):       $$b"; \
		fi; \
	done; \
	[ $$fail -eq 0 ] || (echo "Some failure tests did not error"; exit 1)

.PHONY: all clean test test-pass test-fail test-runtime test-framework test-ir test-paths test-leaks

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
	@fail=0; \
	$(CC) $(CFLAGS) -Icompiler/runtime -pthread -o tests/compiler/test_runtime tests/compiler/test_runtime.c $(RUNTIME_SRC); \
	if ./tests/compiler/test_runtime > /dev/null; then \
		echo "  OK:   test_runtime"; \
	else \
		echo "  FAIL: test_runtime"; fail=1; \
	fi; \
	$(CC) $(CFLAGS) -Icompiler/runtime -o tests/compiler/test_channel tests/compiler/test_channel.c $(RUNTIME_SRC) $(CHANNEL_SRC); \
	if ./tests/compiler/test_channel > /dev/null; then \
		echo "  OK:   test_channel"; \
	else \
		echo "  FAIL: test_channel"; fail=1; \
	fi; \
	rm -f tests/compiler/test_runtime tests/compiler/test_channel; \
	[ $$fail -eq 0 ] || (echo "Some runtime tests failed"; exit 1)

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

test-paths: $(OUT)
	@echo "=== Path handling regressions ==="
	@fail=0; \
	tmp="/tmp/potato path ' quote"; \
	rm -rf "$$tmp"; \
	mkdir -p "$$tmp"; \
	cp tests/fixtures/path_quote_main.ptt "$$tmp/main.ptt"; \
	if ./$(OUT) run "$$tmp/main.ptt" > /dev/null 2>&1; then \
		echo "  OK:   source path with space and apostrophe"; \
	else \
		echo "  FAIL: source path with space and apostrophe"; fail=1; \
	fi; \
	rm -rf "$$tmp"; \
	mkdir -p "$$tmp/lib"; \
	cp tests/fixtures/path_project_main.ptt "$$tmp/main.ptt"; \
	cp tests/fixtures/path_project_helper.ptt "$$tmp/lib/helper.ptt"; \
	touch "$$tmp/potato.toml"; \
	if ./$(OUT) run "$$tmp/main.ptt" > /dev/null 2>&1; then \
		echo "  OK:   project import path with spaces and apostrophe"; \
	else \
		echo "  FAIL: project import path with spaces and apostrophe"; fail=1; \
	fi; \
	rm -rf "$$tmp"; \
	[ $$fail -eq 0 ] || (echo "Some path handling tests failed"; exit 1)

test-leaks: $(OUT)
	@echo "=== Leak regressions ==="
	@fail=0; \
	src=tests/leaks/named_arg_string_literal.ptt; \
	asm=named_arg_string_literal.s; \
	rm -f "$$asm" named_arg_string_literal named_arg_string_literal.o; \
	if ! ./$(OUT) ir "$$src" > /dev/null 2>&1; then \
		echo "  FAIL: $$src did not lower to IR"; fail=1; \
	elif awk '/^_make_holder:/{inside=1; next} inside && /^_/{inside=0} inside && /bl _alloc_String/{bad=1} END{exit bad ? 1 : 0}' "$$asm"; then \
		echo "  OK:   named-arg String literal constructor avoids leaked auto-init"; \
	else \
		echo "  FAIL: named-arg String literal constructor leaks auto-init"; fail=1; \
	fi; \
	rm -f "$$asm" named_arg_string_literal named_arg_string_literal.o; \
	src=tests/leaks/named_arg_nested_string_literal.ptt; \
	asm=named_arg_nested_string_literal.s; \
	rm -f "$$asm" named_arg_nested_string_literal named_arg_nested_string_literal.o; \
	if ! ./$(OUT) ir "$$src" > /dev/null 2>&1; then \
		echo "  FAIL: $$src did not lower to IR"; fail=1; \
	elif awk '/^_make_pair:/{inside=1; next} inside && /^_/{inside=0} inside && /bl _alloc_String/{bad=1} END{exit bad ? 1 : 0}' "$$asm"; then \
		echo "  OK:   nested named-arg String literals avoid leaked auto-init"; \
	else \
		echo "  FAIL: nested named-arg String literals leak auto-init"; fail=1; \
	fi; \
	rm -f "$$asm" named_arg_nested_string_literal named_arg_nested_string_literal.o; \
	src=tests/leaks/heap_slot_drop_emits_drop.ptt; \
	asm=heap_slot_drop_emits_drop.s; \
	rm -f "$$asm" heap_slot_drop_emits_drop heap_slot_drop_emits_drop.o; \
	if ! ./$(OUT) ir "$$src" > /dev/null 2>&1; then \
		echo "  FAIL: $$src did not lower to IR"; fail=1; \
	else \
		for fn in _List__String_set _Set__String_add _Map__String__String_set; do \
			if awk -v fn="$$fn:" 'index($$0, fn)==1{inside=1; next} inside && /^_/{inside=0} inside && /bl _drop_String/{found=1} END{exit found ? 0 : 1}' "$$asm"; then \
				echo "  OK:   $$fn drops String before slot overwrite"; \
			else \
				echo "  FAIL: $$fn missing _drop_String before slot overwrite (F-001)"; fail=1; \
			fi; \
		done; \
	fi; \
	rm -f "$$asm" heap_slot_drop_emits_drop heap_slot_drop_emits_drop.o; \
	[ $$fail -eq 0 ] || (echo "Some leak tests failed"; exit 1)
