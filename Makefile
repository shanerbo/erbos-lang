CC = cc
CFLAGS = -Wall -Wextra -std=c11

# Compiler frontend (the `erbos` binary).
COMPILER_SRC = compiler/main.c compiler/lexer.c compiler/parser.c compiler/monomorph.c compiler/checker.c compiler/optimizer.c compiler/runtime_emit.c compiler/irgen.c compiler/iropt.c compiler/regalloc.c compiler/iremit.c compiler/hashmap.c compiler/target_spawn.c compiler/target_darwin_arm64.c compiler/target_linux_arm64.c

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
	rm -f heap_parent_drop_emits.s heap_parent_drop_emits heap_parent_drop_emits.o

test: $(OUT) test-pass test-fail test-runtime test-framework test-ir test-paths test-leaks
	@echo ""
	@echo "All tests passed."

# Parallelism knob shared across the *.ptt-loop targets. Override
# with `make TEST_JOBS=N test`. xargs -P<n> distributes per-file
# work across processes; matches the host's available cores by
# default (10 on the dev machine).
TEST_JOBS ?= 10

test-pass: $(OUT)
	@echo "=== Passing examples ==="
	@ls examples/*.ptt | grep -vE 'examples/(nomut_test|oob_test|move_test)\.ptt$$' \
	  | xargs -P$(TEST_JOBS) -n1 sh -c '\
	    f="$$1"; b=$$(basename $$f); \
	    if ! ./$(OUT) run "$$f" > /dev/null 2>&1; then \
	      echo "  FAIL: $$b"; exit 1; \
	    else \
	      echo "  OK:   $$b"; \
	    fi' _
	@echo "=== leetcode library compile check ==="
	@ls tests/lib/leetcode/*.ptt | xargs -P$(TEST_JOBS) -n1 sh -c '\
	    f="$$1"; b=$$(basename $$f); \
	    if ! ./$(OUT) ir "$$f" > /dev/null 2>&1; then \
	      echo "  FAIL: $$b (wont compile to IR)"; rm -f $${b%.ptt}.s; exit 1; \
	    fi; \
	    echo "  OK:   $$b"; rm -f $${b%.ptt}.s' _

# Files in tests/errors/ that are RUNTIME panic tests (compile
# successfully, exit non-zero at runtime) rather than COMPILE-fail
# tests. Both lists drive separate xargs pipelines below.
RUNTIME_PANIC_BASES = negative_index empty_pop_panics option_unwrap_none_panics \
  result_unwrap_err_panics result_unwrap_err_on_ok_panics \
  list_remove_out_of_range_panics list_insert_out_of_range_panics \
  stack_pop_empty_panics stack_peek_empty_panics \
  queue_pop_empty_panics queue_front_empty_panics \
  deque_pop_front_empty_panics deque_pop_back_empty_panics \
  deque_front_empty_panics deque_back_empty_panics deque_get_oob_panics \
  string_slice_oob_panics string_split_empty_sep_panics \
  string_replace_empty_from_panics string_builder_push_byte_invalid_panics \
  byte_buffer_get_oob_panics byte_buffer_set_oob_panics \
  arena_get_oob_panics arena_set_oob_panics \
  ring_buffer_pop_empty_panics ring_buffer_peek_empty_panics \
  math_pow_negative_panics math_pow_mod_bad_args_panics math_sqrt_negative_panics \
  map_get_missing_key_panics pool_get_stale_panics

RUNTIME_PANIC_FILES = examples/oob_test.ptt $(addprefix tests/errors/,$(addsuffix .ptt,$(RUNTIME_PANIC_BASES)))

test-fail: $(OUT)
	@echo "=== Expected compile failures ==="
	@tmp=$$(mktemp); \
	  printf 'tests/errors/%s.ptt\n' $(RUNTIME_PANIC_BASES) > $$tmp; \
	  { ls tests/errors/*.ptt; echo examples/nomut_test.ptt; echo examples/move_test.ptt; } \
	  | grep -vFf $$tmp \
	  | xargs -P$(TEST_JOBS) -n1 sh -c '\
	    f="$$1"; b=$$(basename $$f); \
	    if ./$(OUT) "$$f" > /dev/null 2>&1; then \
	      echo "  FAIL (should error): $$b"; exit 1; \
	    fi; \
	    echo "  OK (errored):        $$b"' _ ; \
	  rc=$$?; rm -f $$tmp; exit $$rc
	@echo "=== Expected runtime panics ==="
	@printf '%s\n' $(RUNTIME_PANIC_FILES) \
	  | xargs -P$(TEST_JOBS) -n1 sh -c '\
	    f="$$1"; b=$$(basename $$f); \
	    if ./$(OUT) run "$$f" > /dev/null 2>&1; then \
	      echo "  FAIL (should panic): $$b"; exit 1; \
	    fi; \
	    echo "  OK (panicked):       $$b"' _

.PHONY: all clean test test-pass test-fail test-runtime test-framework test-ir test-paths test-leaks

# Run every tests/ir/*.ptt through the framework runner
# (`erbos test`) at each optimization level. Each test file uses
# `test "name" { ... assert(...) }` blocks; non-zero exit on any
# level means an assertion failed or the program crashed. The
# multi-level sweep is what guarantees no iropt pass changed
# observable program behaviour.
test-ir: $(OUT)
	@echo "=== IR backend regression tests (matrix: -O0/-O1/-O2) ==="
	@ls tests/ir/*.ptt | xargs -P$(TEST_JOBS) -n1 sh -c '\
	    f="$$1"; b=$$(basename $$f .ptt); \
	    for level in -O0 -O1 -O2; do \
	      if ! ./$(OUT) $$level test "$$f" > /dev/null 2>&1; then \
	        echo "  FAIL: $$b $$level"; exit 1; \
	      fi; \
	      echo "  OK:   $$b $$level"; \
	    done' _

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
	@ls tests/test_*.ptt tests/leetcode/test_*.ptt | xargs -P$(TEST_JOBS) -n1 sh -c '\
	    f="$$1"; b=$$(basename $$f); \
	    if ! ./$(OUT) test "$$f" > /dev/null 2>&1; then \
	      echo "  FAIL: $$b"; exit 1; \
	    fi; \
	    echo "  OK:   $$b"' _

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
	src=tests/leaks/heap_parent_drop_emits.ptt; \
	asm=heap_parent_drop_emits.s; \
	rm -f "$$asm" heap_parent_drop_emits heap_parent_drop_emits.o; \
	if ! ./$(OUT) ir "$$src" > /dev/null 2>&1; then \
		echo "  FAIL: $$src did not lower to IR"; fail=1; \
	else \
		if awk '/^_drop_array_String:/{found=1} END{exit found ? 0 : 1}' "$$asm"; then \
			echo "  OK:   _drop_array_String helper emitted"; \
		else \
			echo "  FAIL: _drop_array_String helper missing (F-002)"; fail=1; \
		fi; \
		if awk '/^_clone_array_String:/{found=1} END{exit found ? 0 : 1}' "$$asm"; then \
			echo "  OK:   _clone_array_String helper emitted"; \
		else \
			echo "  FAIL: _clone_array_String helper missing (F-002)"; fail=1; \
		fi; \
		if awk '/^_drop_List__String:/{inside=1; next} inside && /^\.globl/{inside=0} inside && /bl _drop_array_String/{found=1} END{exit found ? 0 : 1}' "$$asm"; then \
			echo "  OK:   _drop_List__String delegates to _drop_array_String (cap-bounded)"; \
		else \
			echo "  FAIL: _drop_List__String missing _drop_array_String call (F-002)"; fail=1; \
		fi; \
		if awk '/^_drop_Set__String:/{inside=1; next} inside && /^\.globl/{inside=0} inside && /bl _drop_array_String/{found=1} END{exit found ? 0 : 1}' "$$asm"; then \
			echo "  OK:   _drop_Set__String delegates to _drop_array_String"; \
		else \
			echo "  FAIL: _drop_Set__String missing _drop_array_String call (F-002)"; fail=1; \
		fi; \
		if awk '/^_drop_Map__String__String:/{inside=1; next} inside && /^\.globl/{inside=0} inside && /bl _drop_array_String/{c++} END{exit c >= 2 ? 0 : 1}' "$$asm"; then \
			echo "  OK:   _drop_Map__String__String delegates twice (keys + vals)"; \
		else \
			echo "  FAIL: _drop_Map__String__String missing dual _drop_array_String calls (F-002)"; fail=1; \
		fi; \
		if awk '/^_clone_List__String:/{inside=1; next} inside && /^\.globl/{inside=0} inside && /bl _clone_array_String/{found=1} END{exit found ? 0 : 1}' "$$asm"; then \
			echo "  OK:   _clone_List__String delegates to _clone_array_String"; \
		else \
			echo "  FAIL: _clone_List__String missing _clone_array_String call (F-002)"; fail=1; \
		fi; \
	fi; \
	rm -f "$$asm" heap_parent_drop_emits heap_parent_drop_emits.o; \
	[ $$fail -eq 0 ] || (echo "Some leak tests failed"; exit 1)
