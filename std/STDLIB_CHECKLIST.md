# Potato Stdlib Completion Checklist

This document is the implementation checklist for turning `std/` from a
prototype into a serious standard library.

Read this with `README.md`, [`../docs/language-law.md`](../docs/language-law.md)
(canonical value-formation grammar), `../docs/language-guide.md`,
`../docs/design-decisions.md`, and `../codex/AUDITING.md`. Do not
implement a stdlib feature by weakening the language model.
Potato stdlib code must preserve:

- explicit ownership (`is now`, `is rep`)
- no hidden aliases
- no hidden imports
- no GC or refcounting
- lexical borrows only
- arena plus integer handle patterns for shared long-lived data
- word-style generics (`List of T`, not user-facing `<T>`)

Every completed item needs:

- implementation in `std/` or compiler runtime support if unavoidable
- framework tests under `tests/test_*.ptt`
- compile-fail tests under `tests/errors/*.ptt` for misuse
- docs/examples if user-facing
- `make test` ending with `All tests passed.`

## Current Baseline

Verified current files:

- `std/string.ptt`
- `std/list.ptt`
- `std/map.ptt`
- `std/option.ptt`
- `std/result.ptt`
- `std/stack.ptt`
- `std/queue.ptt`
- `std/deque.ptt`
- `std/arena.ptt`
- `std/byte_buffer.ptt`
- `std/string_builder.ptt`
- `std/ring_buffer.ptt`
- `std/math.ptt`
- `std/algo.ptt`
- `std/basics.ptt`

Verified current state:

- `Option` and `Result` exist and have framework / panic coverage.
- `List`, `Stack`, `Queue`, `Deque`, `String`, `StringBuilder`,
  `ByteBuffer`, `Arena`, `RingBuffer`, `std/math`, and `std/algo` exist.
- `Map` is a linear-scan parallel-array map.
- `Set`, `Pool`, `Path`, `File`, `Reader`, `Writer`, and `Graph` do not
  exist yet.
- `Arena of T` plus integer handles is now the blessed shared-data pattern
  in both docs and tests.
- Growable containers now normalize around zero-value formation
  plus `reserve`; only fixed-cap or semantic-value factories should
  remain.

## Construction and Naming Rules

Verified current behavior:

- Zero-value formation is the primary empty-value path. The
  trailing `()` is required — a bare type expression is never
  a value (see [`../docs/language-law.md`](../docs/language-law.md)):
  - non-generic structs: `StringBuilder()`, `ByteBuffer()`
  - generic structs: `List of int()`, `Stack of int()`,
    `Queue of String()`, `Arena of User()`
- Growable containers preallocate through `reserve`, not through
  alternate value-formation spellings.
- Enum factory formation uses free functions:
  `some of int (7)`, `none of int ()`, `ok of int, String ("x")`,
  `err of int, String ("x")`. `RingBuffer` keeps a dedicated
  capacity factory: `ring_with_cap of int (64)`.

Normalization target:

- Keep the language-level two-form construction model for structs:
  zero-value formation `Type()` and named-field formation
  `Type(field is value, ...)`.
- Prefer zero-value formation for fresh empty values.
- Use `reserve` on growable containers when preallocation matters.
- Keep enum/fallible-value factories bare:
  `some`, `none`, `ok`, `err`.
- Keep dedicated construction helpers only when the capacity or shape is a
  real invariant, such as `RingBuffer`'s fixed capacity.
- Do not add `Type.new(...)` as an additional public convention while
  the language already has struct construction and free factories.
- Do not normalize toward type-receiver calls like
  `List of int .with_cap(64)` (rejected by the parser per the
  language-law cleanup).

## Phase 0: Foundation Types

### `Option of T`

Purpose: represent optional values without sentinel `0` / `-1` hacks.

Required API:

- `some(value T) Option of T`
- `none() Option of T`
- `Option.is_some(self Option of T) bool`
- `Option.is_none(self Option of T) bool`
- `Option.unwrap(self Option of T) T`
- `Option.unwrap_or(self Option of T, fallback T) T`

Algorithm / representation:

- enum-backed: `Some(value T) | None`
- `is_some` / `is_none`: tag check
- `unwrap`: `match`, panic on `None`
- no allocation unless `T` owns heap data

Tests:

- some/none branches
- unwrap success
- unwrap panic on none
- heap-shaped `T` ownership behavior

### `Result of T, E`

Purpose: standard fallible return type for parsing, I/O, checked lookups,
and future APIs that should not panic.

Required API:

- `ok(value T) Result of T, E`
- `err(error E) Result of T, E`
- `Result.is_ok(self Result of T, E) bool`
- `Result.is_err(self Result of T, E) bool`
- `Result.unwrap(self Result of T, E) T`
- `Result.unwrap_err(self Result of T, E) E`
- `Result.unwrap_or(self Result of T, E, fallback T) T`

Algorithm / representation:

- enum-backed: `Ok(value T) | Err(error E)`
- methods use `match`
- `unwrap` panics only on wrong variant

Tests:

- ok/err branches
- unwrap success
- unwrap panic on err
- heap-shaped success and error payloads

## Phase 1: Core Containers

### `List of T`

Purpose: growable contiguous array.

Required API:

- zero-value formation: `xs is List of T()`
- `List.reserve(self ref List of T, cap int)`
- `List.len(self List of T) int`
- `List.cap(self List of T) int`
- `List.empty(self List of T) bool`
- `List.push(self ref List of T, v T)`
- `List.pop(self ref List of T) T`
- `List.try_pop(self ref List of T) Option of T`
- `List.get(self List of T, i int) T`
- `List.try_get(self List of T, i int) Option of T`
- `List.set(self ref List of T, i int, v T)`
- `List.insert(self ref List of T, i int, v T)`
- `List.remove(self ref List of T, i int) T`
- `List.clear(self ref List of T)`
- `List.contains(self List of T, v T) bool`
- `List.index_of(self List of T, v T) int`
- `List.reverse(self ref List of T)`
- `List.clone(self List of T) List of T`

Algorithm / representation:

- fields: `count int`, `data array of T`
- growth: capacity doubles, minimum cap 8
- `insert`: bounds-check, grow if needed, shift elements right
- `remove`: bounds-check, save element, shift elements left
- `contains` / `index_of`: linear scan using `eq`
- `reverse`: two-pointer swap
- `clone`: deep clone elements with explicit copy semantics

Hard requirements:

- no hidden heap aliasing
- replacement/growth must not leak old arrays
- `get` returning heap-shaped values must not allow mutation through a
  non-`ref` list parameter

Tests:

- primitive and heap-shaped elements
- growth over multiple capacity doublings
- insert/remove at front/middle/back
- panic and `try_*` behavior
- alias-safety compile-fail tests

### `Deque of T`

Purpose: double-ended queue with O(1) amortized pushes/pops at both ends.

Required API:

- zero-value formation: `d is Deque of T()`
- `Deque.reserve(self ref Deque of T, cap int)`
- `Deque.len(self Deque of T) int`
- `Deque.cap(self Deque of T) int`
- `Deque.empty(self Deque of T) bool`
- `Deque.push_back(self ref Deque of T, v T)`
- `Deque.push_front(self ref Deque of T, v T)`
- `Deque.pop_back(self ref Deque of T) T`
- `Deque.pop_front(self ref Deque of T) T`
- `Deque.try_pop_back(self ref Deque of T) Option of T`
- `Deque.try_pop_front(self ref Deque of T) Option of T`
- `Deque.front(self Deque of T) T`
- `Deque.back(self Deque of T) T`
- `Deque.get(self Deque of T, i int) T`
- `Deque.clear(self ref Deque of T)`

Algorithm / representation:

- circular buffer: `head int`, `count int`, `data array of T`
- physical index: `(head + logical_index) mod cap`
- growth: allocate double capacity and copy logical order to index `0`
- `push_front`: decrement head modulo cap
- `push_back`: write at `(head + count) mod cap`

Tests:

- wraparound push/pop behavior
- growth after wraparound
- primitive and heap-shaped elements
- bounds and empty behavior

### `Stack of T`

Purpose: LIFO container. Replace current int-only helper module.

Required API:

- zero-value formation: `s is Stack of T()`
- `Stack.reserve(self ref Stack of T, cap int)`
- `Stack.len(self Stack of T) int`
- `Stack.empty(self Stack of T) bool`
- `Stack.push(self ref Stack of T, v T)`
- `Stack.pop(self ref Stack of T) T`
- `Stack.try_pop(self ref Stack of T) Option of T`
- `Stack.peek(self Stack of T) T`
- `Stack.try_peek(self Stack of T) Option of T`
- `Stack.clear(self ref Stack of T)`

Algorithm / representation:

- wrapper struct: `items List of T`
- `push`: `items.push`
- `pop`: `items.pop`
- `peek`: `items.get(items.len() - 1)`

Tests:

- generic int and String stacks
- empty `try_*`
- panic on empty `pop` / `peek`

### `Queue of T`

Purpose: FIFO container. Replace current int-only `List + external head`
helper module.

Required API:

- zero-value formation: `q is Queue of T()`
- `Queue.reserve(self ref Queue of T, cap int)`
- `Queue.len(self Queue of T) int`
- `Queue.empty(self Queue of T) bool`
- `Queue.push(self ref Queue of T, v T)`
- `Queue.pop(self ref Queue of T) T`
- `Queue.try_pop(self ref Queue of T) Option of T`
- `Queue.front(self Queue of T) T`
- `Queue.try_front(self Queue of T) Option of T`
- `Queue.clear(self ref Queue of T)`

Algorithm / representation:

- either wrap `Deque of T` or use the same circular-buffer algorithm
- do not implement as unbounded `List + head`, because it retains dead
  front elements

Tests:

- FIFO order
- wraparound
- growth
- empty behavior

## Phase 2: Hash Containers

### `Map of K, V` — implemented (open-addressed)

Purpose: serious associative map. Current implementation is an
open-addressed hash table with linear probing and tombstone-
reusing inserts; the linear-scan map it replaced is gone.

Shipped API (all required entries present in `std/map.ptt` and
covered by `tests/test_map_methods.ptt`):

- zero-value formation: `m is Map of K, V()` ✓
- `Map.reserve(self ref Map of K, V, cap int)` ✓
- `Map.len(self Map of K, V) int` ✓
- `Map.cap(self Map of K, V) int` ✓
- `Map.empty(self Map of K, V) bool` ✓
- `Map.set(self ref Map of K, V, k K, v V)` ✓ (deep-clones K, V)
- `Map.get(self Map of K, V, k K) V` ✓ (panics on missing)
- `Map.try_get(self Map of K, V, k K) Option of V` ✓
- `Map.has(self Map of K, V, k K) bool` ✓
- `Map.remove(self ref Map of K, V, k K) bool` ✓ (tombstone)
- `Map.clear(self ref Map of K, V)` ✓
- `Map.keys(self Map of K, V) List of K` ✓
- `Map.values(self Map of K, V) List of V` ✓
- `Map.entries(self Map of K, V) List of Entry of K, V` — deferred
  pending an `Entry of K, V` decision (see "Why Entry deferred"
  below); not blocking stdlib consolidation since `keys()` plus
  `try_get(k)` covers every present user need.

Algorithm / representation:

- open-addressed hash table; power-of-two cap for fast modulo;
- arrays: `keys array of K`, `vals array of V`, `states array of byte`
  with states 0=EMPTY, 1=FULL, 2=DELETED;
- resize-before-insert at 70% load (`occupied >= cap*7/10`);
- hash algorithms:
  - `int`: Knuth multiplicative (`int.hash` in `std/math`),
  - `String`: djb2-shaped (`String.hash`, already in `std/string`),
  - generic code calls `k.hash()` and `eq`; the compiler resolves
    both to the typed implementation post-monomorph.

Why Entry deferred: the spec'd `entries()` requires a generic
`Entry of K, V` struct. With the new value-formation law, the
returned list would have to push values formed via
`Entry of K, V(key is k, value is v)` inside generic code; that
works syntactically but the auto-init recursion for struct-typed
fields is still being audited. Punting until the audit clears.

Compiler/runtime root-cause fixes that landed alongside Map:

1. `arr[i] be now src` / `arr[i] be rep src` — the language now
   supports ownership transfer and deep-clone on index-assign,
   so `Map.set` can store deep-cloned keys/values into the
   `array of K` / `array of V` slots without aliasing the
   caller's locals. (Parser, checker, irgen, monomorph clone.)
2. `_heap_alloc` zero-fills the returned block on both the
   free-list-reuse and bump paths. Without this, allocations of
   `array of byte` (e.g. `states`) saw stale heap bytes and
   reported the wrong slot as FULL=1 on the very first read.
3. `_heap_free` rounds the requested size up to a 16-byte
   multiple before threading the block onto the free list. The
   metadata header (next pointer + size) is 16 bytes; freeing
   a sub-16-byte block (e.g. `array of byte with cap 8`) was
   corrupting adjacent free-list nodes.
4. `NODE_INDEX_ASSIGN` now has a `clone_node` case in
   monomorph.c, so generic-method bodies that write to array
   slots are deep-copied per instantiation rather than aliased.
5. F-001 fix: `arr[i] be now src` and `arr[i] be rep src`
   over a typed array whose element type is a heap-shaped struct
   now drop the slot's previous owner before storing the new
   pointer. The checker fills `index_assign.elem_struct_name`
   when the array's element resolves to a struct in this program;
   irgen null-guards the slot's prior value and calls
   `_drop_<elem_struct_name>` before the IR_STORE. Plain
   `arr[i] be src` (no `now`/`rep`) keeps the legacy raw-store
   semantics so shift/swap loops do not free aliased pointers.
   `std/list.ptt::List.set` switched its body to the
   clone-then-transfer pattern already in use by `Map.set` and
   `Set.add`: `v_clone is rep v; self.data[i] be now v_clone`.
   The deep-clone is the alias-safety guarantee — a caller can
   pass the slot's own current value (e.g.
   `v is xs.get(0); xs.set(0, v)`) and the slot still ends up
   with an independent block, never a use-after-free. For
   primitive T, `is rep` collapses to a value copy and `be now`
   collapses to a plain store, so the perf cost is nil. The
   same fix flows through every stdlib container that delegates
   to `List.set` (`Pool.set`, `Pool.insert` reuse path).
   `Map.set` and `Set.add` already used `be now <clone>` and
   now correctly drop tombstoned and updated slots' previous
   occupants because of the new compiler-side drop.

Tests in place:

- happy-path set/get/has/try_get/remove/clear/reserve/keys/values
- update-in-place (re-set replaces value)
- growth across cap doublings
- tombstone reuse after remove
- tombstone-doesn't-block-probe-past-it
- panic on `get` of a missing key (`tests/errors/map_get_missing_key_panics.ptt`)
- String key equality by content, not rodata pointer
- heap-shaped value type (`Map of int, String`) survives growth
- negative-key hash bucket index stays in range

### `Set of T` — implemented

Purpose: unique-value collection. Same open-addressed hash
table machinery as `Map`, no value array.

Shipped API (all required entries present in `std/set.ptt`,
covered by `tests/test_set.ptt`):

- zero-value formation: `s is Set of T()` ✓
- `Set.reserve(self ref Set of T, cap int)` ✓
- `Set.len(self Set of T) int` ✓
- `Set.cap(self Set of T) int` ✓ (additional, parallels Map)
- `Set.empty(self Set of T) bool` ✓
- `Set.add(self ref Set of T, v T) bool` ✓ (deep-clones v)
- `Set.has(self Set of T, v T) bool` ✓
- `Set.remove(self ref Set of T, v T) bool` ✓ (tombstone)
- `Set.clear(self ref Set of T)` ✓
- `Set.values(self Set of T) List of T` ✓
- `Set.union(self Set of T, other Set of T) Set of T` ✓
- `Set.intersect(self Set of T, other Set of T) Set of T` ✓
  (iterates the smaller side for fewer hash probes)
- `Set.difference(self Set of T, other Set of T) Set of T` ✓

Compiler/runtime fix landed alongside Set: the parser's
if-continuation heuristic was greedily attaching the next
statement as an else-if when the gap had been newline-only.
Two consecutive guarded blocks
`a.data ne 0 ?{ ... }\n other.data ne 0 ?{ ... }` were
parsing as a single if/else-if chain, so the second block ran
only when the first failed. Fixed in
`compiler/parser.c::parse_if_continuation`: a newline gap
terminates the if. Without this fix, every Set algebra
operation that uses two guards in sequence (e.g. `union`)
silently dropped half its work.

Algorithm / representation:

- same hash table strategy as `Map`, without values
- `intersect`: iterate smaller set
- `union`: clone larger then add smaller
- `difference`: iterate left set and include items absent from right

Tests:

- add duplicates
- remove
- resize
- set algebra
- String values

## Phase 3: Text and Bytes

### `String`

Purpose: owned or borrowed UTF-8 byte string.

Required API additions:

- `String.starts_with(self String, prefix String) bool`
- `String.ends_with(self String, suffix String) bool`
- `String.contains(self String, needle String) bool`
- `String.index_of(self String, needle String) int`
- `String.slice(self String, start int, end int) String`
- `String.trim(self String) String`
- `String.trim_start(self String) String`
- `String.trim_end(self String) String`
- `String.split(self String, sep String) List of String`
- `String.replace(self String, from String, to String) String`
- `String.to_int(self String) Result of int, String`
- `String.hash(self String) int`
- `String.clone(self String) String`

Algorithm / representation:

- keep current layout unless compiler/runtime contracts change:
  `cap`, `count`, `data`, `owned`
- `starts_with` / `ends_with`: byte compare
- `index_of` / `contains`: naive search first; KMP later for long needles
- `slice`: allocate a new owned string
- `trim`: scan ASCII whitespace at edges
- `split`: repeated `index_of`, allocate list of owned slices
- `replace`: two-pass count then allocate exact output size
- `to_int`: checked base-10 parser with overflow detection
- `hash`: FNV-1a over active bytes

Tests:

- empty strings
- literals and owned strings
- ASCII trim/split/replace
- parse success/failure/overflow
- ownership after concat/slice/replace

### `StringBuilder`

Purpose: efficient repeated string construction.

Required API:

- zero-value formation: `b is StringBuilder()`
- `StringBuilder.reserve(self ref StringBuilder, cap int)`
- `StringBuilder.len(self StringBuilder) int`
- `StringBuilder.empty(self StringBuilder) bool`
- `StringBuilder.push_byte(self ref StringBuilder, b int)`
- `StringBuilder.push_string(self ref StringBuilder, s String)`
- `StringBuilder.push_int(self ref StringBuilder, x int)`
- `StringBuilder.clear(self ref StringBuilder)`
- `StringBuilder.to_string(self StringBuilder) String`

Algorithm / representation:

- growable `array of byte` plus `count`
- capacity doubles
- `push_string`: copy bytes
- `push_int`: use decimal conversion into scratch or `int.to_string`
- `to_string`: allocate exact owned String

Tests:

- many appends
- growth
- conversion to String
- clear and reuse

### `ByteBuffer`

Purpose: raw byte storage for I/O, parsing, and binary protocols.

Required API:

- zero-value formation: `b is ByteBuffer()`
- `ByteBuffer.reserve(self ref ByteBuffer, cap int)`
- `ByteBuffer.len(self ByteBuffer) int`
- `ByteBuffer.cap(self ByteBuffer) int`
- `ByteBuffer.empty(self ByteBuffer) bool`
- `ByteBuffer.push(self ref ByteBuffer, b int)`
- `ByteBuffer.extend(self ref ByteBuffer, other ByteBuffer)`
- `ByteBuffer.get(self ByteBuffer, i int) int`
- `ByteBuffer.set(self ref ByteBuffer, i int, b int)`
- `ByteBuffer.slice(self ByteBuffer, start int, end int) ByteBuffer`
- `ByteBuffer.clear(self ref ByteBuffer)`
- `ByteBuffer.to_string(self ByteBuffer) String`

Algorithm / representation:

- specialized growable `array of byte`
- doubling growth
- exact-copy slice
- `to_string`: allocate owned `String` using copied bytes

Tests:

- binary bytes including zero
- growth
- slice
- string conversion

## Phase 4: Potato-Native Shared Data

### `Arena of T`

Purpose: first-class blessed pattern for shared data without GC,
refcounting, inheritance, or long-lived borrows.

Required API:

- zero-value formation: `a is Arena of T()`
- `Arena.reserve(self ref Arena of T, cap int)`
- `Arena.len(self Arena of T) int`
- `Arena.add(self ref Arena of T, value T) int`
- `Arena.get(self Arena of T, id int) T`
- `Arena.try_get(self Arena of T, id int) Option of T`
- `Arena.set(self ref Arena of T, id int, value T)`
- `Arena.has(self Arena of T, id int) bool`
- `Arena.clear(self ref Arena of T)`

Algorithm / representation:

- append-only `List of T`
- handles are integer indexes
- no deletion in the first version
- later: `Pool of T` for deletion/reuse

Tests:

- add/get/set
- invalid handle
- heap-shaped items
- example showing `App` struct with multiple arenas

### `Pool of T` — implemented

Purpose: arena with deletion and slot reuse.

Shipped API (all required entries present in `std/pool.ptt`,
covered by `tests/test_pool.ptt`):

- zero-value formation: `p is Pool of T()` ✓
- `Pool.reserve(self ref Pool of T, cap int)` ✓
- `Pool.len(self Pool of T) int` ✓ (live slots)
- `Pool.slot_count(self Pool of T) int` ✓ (high-water mark)
- `Pool.empty(self Pool of T) bool` ✓
- `Pool.insert(self ref Pool of T, value T) int` ✓ (reuses freed)
- `Pool.remove(self ref Pool of T, id int) bool` ✓
- `Pool.has(self Pool of T, id int) bool` ✓
- `Pool.get(self Pool of T, id int) T` ✓ (panics on stale id)
- `Pool.try_get(self Pool of T, id int) Option of T` ✓
- `Pool.set(self ref Pool of T, id int, value T)` ✓
- `Pool.clear(self ref Pool of T)` ✓

Generational handles are not part of the first version; a
stale handle whose slot has been reused silently addresses
the new occupant. Future upgrade per the spec:
generation-tagged ids that reject stale reads.

Tests in place:

- happy-path insert/get/set/has/try_get/remove/clear/reserve
- monotonically increasing ids when freelist is empty
- freelist reuse: remove slot, insert reuses same id, slot
  count doesn't grow
- panic on `get` of a removed slot
  (`tests/errors/pool_get_stale_panics.ptt`)
- heap-shaped T (Pool of String): mass insert + remove evens +
  re-insert; high-water mark stays fixed
- live count vs slot count after partial removes

Algorithm / representation:

- `items List of T`
- `alive List of bool`
- `free List of int`
- reuse slots from `free`
- future upgrade: generational handles to reject stale ids
- `BitSet` is optional here, not a prerequisite for the first serious
  version

Tests:

- insert/remove/reuse
- invalid and stale id behavior
- heap-shaped items drop correctly

## Phase 5: Specialized Containers

### `PriorityQueue of T`

Status: optional later. Do not block stdlib consolidation on this.

Purpose: heap-backed priority queue, only once a concrete workload needs
it.

Required API:

- zero-value formation: `pq is PriorityQueue of T()`
- `PriorityQueue.reserve(self ref PriorityQueue of T, cap int)`
- `PriorityQueue.len(self PriorityQueue of T) int`
- `PriorityQueue.empty(self PriorityQueue of T) bool`
- `PriorityQueue.push(self ref PriorityQueue of T, v T)`
- `PriorityQueue.pop(self ref PriorityQueue of T) T`
- `PriorityQueue.try_pop(self ref PriorityQueue of T) Option of T`
- `PriorityQueue.peek(self PriorityQueue of T) T`
- `PriorityQueue.try_peek(self PriorityQueue of T) Option of T`

Algorithm / representation:

- binary heap in `List of T`
- `push`: append and sift up
- `pop`: swap root with last, pop, sift down
- generic comparison requires a language-level comparison story; implement
  `PriorityQueue of int` or `PriorityItem is { priority int, value T }`
  first

Tests:

- ordering
- duplicates
- empty behavior
- many inserts/pops

### `RingBuffer of T`

Purpose: fixed-capacity bounded buffer.

Required API:

- `ring_with_cap of T (cap int) RingBuffer of T` (free-function factory)
- `RingBuffer.len(self RingBuffer of T) int`
- `RingBuffer.cap(self RingBuffer of T) int`
- `RingBuffer.empty(self RingBuffer of T) bool`
- `RingBuffer.full(self RingBuffer of T) bool`
- `RingBuffer.push(self ref RingBuffer of T, v T) bool`
- `RingBuffer.pop(self ref RingBuffer of T) T`
- `RingBuffer.try_pop(self ref RingBuffer of T) Option of T`
- `RingBuffer.peek(self RingBuffer of T) T`
- `RingBuffer.clear(self ref RingBuffer of T)`

Algorithm / representation:

- fixed `array of T`
- `head int`, `count int`
- no growth
- `push` returns false when full

Tests:

- full/empty transitions
- wraparound
- no allocation after construction

### `BitSet`

Status: optional later. Do not block stdlib consolidation on this.

Purpose: compact boolean flags for high-density visited sets or pool slot
tracking once the language has a clear user-facing bitwise story.

Required API:

- `bitset_with_len(bits int) BitSet` (free-function factory; matches `ring_with_cap` shape)
- `BitSet.len(self BitSet) int`
- `BitSet.get(self BitSet, i int) bool`
- `BitSet.set(self ref BitSet, i int)`
- `BitSet.clear_bit(self ref BitSet, i int)`
- `BitSet.toggle(self ref BitSet, i int)`
- `BitSet.clear_all(self ref BitSet)`
- `BitSet.count_ones(self BitSet) int`
- `BitSet.union(self BitSet, other BitSet) BitSet`
- `BitSet.intersect(self BitSet, other BitSet) BitSet`

Algorithm / representation:

- `words array of int`
- word index: `i / word_bits`
- bit mask: `1 << (i mod word_bits)`
- `count_ones`: Kernighan loop until compiler builtin exists

Tests:

- boundary bits
- count
- union/intersect
- out-of-range behavior

## Phase 6: Files and Paths

### `Path` — implemented (free-function module)

Purpose: pure path string manipulation, no filesystem access.

Shape decision: Potato has no static methods, so the spec's
`Path.join(...)` / `Path.basename(...)` lower to module-prefixed
free-function calls — `use std/path` then `path.join(base, child)`
etc. The functions live at top level of `std/path.ptt`.

Shipped API (all required entries present in `std/path.ptt`,
covered by `tests/test_path.ptt`):

- `path.join(base String, child String) String` ✓
- `path.basename(p String) String` ✓
- `path.dirname(p String) String` ✓
- `path.extension(p String) String` ✓
- `path.normalize(p String) String` ✓
- `path.is_absolute(p String) bool` ✓

Compiler/runtime fixes that landed alongside Path:

1. Short-circuit `and` / `or`. The previous emit was straight
   line — both operands always evaluated, even when the left
   already determined the result. Path's basename / dirname /
   normalize all guard with patterns like
   `i gt 0 and arr.byte_at(i - 1) eq 47`, which crash without
   short-circuit. Fixed in `compiler/irgen.c::NODE_BINARY` to
   stash the left-operand result into a hidden stack slot,
   conditionally evaluate the right operand, and load back the
   slot for the final result.
2. `_String_yell` now treats `count == 0` (or `data == 0`) as
   a no-op write before emitting the trailing newline. Without
   this, `yell(String())` (a freshly allocated empty String,
   produced for example by `path.basename("/")`) crashed
   dereferencing the null `data` pointer.

Tests in place (~24 cases):

- absolute / relative path detection
- join: simple, trailing-slash collapse, absolute-child wins,
  empty operands
- basename: typical, trailing slash collapsed, root, empty
- dirname: typical, no-slash relative, trailing slash, root,
  absolute-root
- extension: typical, no-extension, hidden file, trailing dot
- normalize: redundant slashes, dot / dotdot segments,
  dotdot-at-root, leading dotdot in relative paths preserved,
  trailing slash dropped, empty, absolute root

### `File`

Purpose: actual I/O. Requires compiler/runtime syscall support.

Required API:

- `open_file(path String, mode String) Result of File, String` (free-function factory)
- `File.read_all(self ref File) Result of String, String`
- `File.read_bytes(self ref File) Result of ByteBuffer, String`
- `File.write_string(self ref File, s String) Result of int, String`
- `File.write_bytes(self ref File, b ByteBuffer) Result of int, String`
- `File.close(self ref File) Result of void, String`

Algorithm / representation:

- file descriptor integer
- syscall-backed open/read/write/close
- all fallible operations return `Result`
- read-all uses doubling `ByteBuffer`

Tests:

- missing file returns `Err`
- write then read
- binary bytes
- close behavior

### `Reader` / `Writer`

Purpose: buffered I/O on top of `File`.

Required API:

- `new_reader(file File) Reader` (free-function factory)
- `Reader.read_byte(self ref Reader) Result of int, String`
- `Reader.read_line(self ref Reader) Result of String, String`
- `Reader.read_to_end(self ref Reader) Result of String, String`
- `new_writer(file File) Writer` (free-function factory)
- `Writer.write(self ref Writer, s String) Result of int, String`
- `Writer.write_line(self ref Writer, s String) Result of int, String`
- `Writer.flush(self ref Writer) Result of void, String`

Algorithm / representation:

- internal `ByteBuffer`
- reader refill when cursor reaches buffer end
- writer flush when buffer is full or `flush` is called

Tests:

- line reading
- large file bigger than buffer
- flush behavior
- error propagation

## Phase 7: Graph Support

### `Graph of T`

Purpose: common algorithm/data modeling support using arena + indexes.

Required API:

- zero-value formation: `g is Graph of T()`
- `Graph.add_node(self ref Graph of T, value T) int`
- `Graph.add_edge(self ref Graph of T, from int, to int)`
- `Graph.node(self Graph of T, id int) T`
- `Graph.set_node(self ref Graph of T, id int, value T)`
- `Graph.neighbors(self Graph of T, id int) List of int`
- `Graph.bfs(self Graph of T, start int) List of int`
- `Graph.dfs(self Graph of T, start int) List of int`

Algorithm / representation:

- `nodes Arena of T`
- adjacency list: `List of List of int`
- BFS: `Queue of int` plus `List of bool`
- DFS: `Stack of int` plus `List of bool`

Tests:

- disconnected graph
- cycle
- BFS/DFS order
- invalid node id behavior

## Phase 8: Math and Algorithms

### `std/math`

Required additions:

- `clamp(x int, lo int, hi int) int`
- `gcd(a int, b int) int`
- `lcm(a int, b int) int`
- `pow_mod(base int, exp int, mod int) int`
- `sqrt_floor(x int) int`

Algorithms:

- `gcd`: Euclidean algorithm
- `lcm`: `abs(a / gcd(a,b) * b)`, handle zero
- `pow_mod`: exponentiation by squaring
- `sqrt_floor`: binary search
- `pow`: replace repeated multiplication with exponentiation by squaring

Tests:

- negative inputs where valid
- zero cases
- overflow-adjacent behavior documented

### Sorting and Search Helpers

Only add these once comparison rules are clear.

Required APIs for int-first version:

- `sort_ints(xs ref List of int)`
- `binary_search_int(xs List of int, target int) int`
- `lower_bound_int(xs List of int, target int) int`
- `upper_bound_int(xs List of int, target int) int`

Algorithms:

- sorting: iterative merge sort or quicksort with insertion sort cutoff
- binary search: standard half-open interval

Tests:

- empty/single/many
- duplicates
- already sorted and reverse sorted

## Do Not Add Yet

Do not add these until the language deliberately changes:

- inheritance
- traits/interfaces
- operator overloading
- macros
- async/await language syntax
- hidden global allocators exposed to users
- auto-imported stdlib modules
- long-lived borrow/view types that the compiler cannot prove safe

`Slice of T` and borrowed `StringView` are useful, but they should wait
until the compiler can enforce that views do not outlive their owners.

## Acceptance Order

Completed:

1. `Option` and `Result`
2. complete `List`
3. generic `Stack`, `Queue`, `Deque`
4. `String` API and `StringBuilder`
5. `Arena`
6. `ByteBuffer`
7. `RingBuffer`
8. int-first sorting/search helpers and current `std/math`
9. open-addressed hash `Map` (replaces the linear-scan map)
10. `Set of T`
11. `Pool of T`
12. `Path` (free-function module)

Remaining before calling the stdlib consolidated:

1. `File`, `Reader`, `Writer` — requires runtime syscall support;
   kernel-side primitives (`open`/`read`/`write`/`close`) and
   the IR-level fallible-call story still need to land before
   user-facing I/O can ship.

Optional later, only if a concrete workload justifies them:

1. `BitSet`
2. `PriorityQueue`
3. `Graph`

Do not claim stdlib completion while:

- user-facing file I/O is missing
- `make test` fails
