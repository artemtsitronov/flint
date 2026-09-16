# architecture

notes on the internals, for people reading the source. if something here is
wrong, the source is right and this file is a bug.

## values

every value is 64 bits. numbers are doubles. anything with the top 13 bits set
is a boxed tag with a 48-bit payload.

```
double:     [ sign:1 ][ exponent:11 ][ mantissa:52 ]
boxed:      [ 1111 1111 1111 1000 ][ tag:3 ][ payload:48 ]
```

| tag | meaning |
|---|---|
| 1 | nil |
| 2 | false |
| 3 | true |
| 4 | heap pointer to `Obj` |

a 64-bit virtual address uses at most 48 bits, so it fits in the payload. this
is the only reason flint requires a 64-bit host.

arithmetic that produces a NaN gets canonicalized to
`0x7FF8000000000000` before boxing. a NaN with an arbitrary payload could
otherwise collide with a tag.

## compiler

single-pass pratt parser. no ast, no tree walk, no second pass. tokens are
scanned on demand and bytecode goes straight into the chunk buffer.

scopes are a fixed-size stack of `Local` descriptors in the `Compiler` struct,
chained to the enclosing compiler through `enclosing`. slot 0 is always the
callee, so a local index doubles as a stack offset from `frame->slots`.

upvalues are resolved at compile time. `resolve_upvalue()` walks the enclosing
compilers looking for a captured local, marks it captured, and records an
`(index, is_local)` pair. the pair is emitted after `OP_CLOSURE` as two bytes
per upvalue.

constants start as a 1-byte operand. once a chunk passes 256, emission
switches to `OP_CONSTANT_LONG` with a 3-byte index.

### `as`

`OP_CAST` takes one byte, an `FlType` tag, and does nothing but compare. the
value is already on the stack and stays exactly where it is; there is no
variant of this opcode that rewrites anything, because an assertion has nothing
to rewrite.

the interesting part is where the work happens. the type *name* is resolved at
compile time in `as_()`, so `1 as frobnicate` never makes it to the run loop.
the type *value* cannot be: it may be a parameter, a list element, or something
a module supplied, and none of that is knowable while compiling. so the opcode
is a one-byte switch in the dispatch loop, and a mismatch is a runtime error.

the tags in `chunk.h` and the names in `flint_type_name()` are kept in step by
hand, which is the one fragile thing here. `type()` now calls
`flint_type_name()` too, so there is a single list of names and a cast's error
message cannot drift from what `type()` says. the tags are a separate list
because they are the bytecode encoding, and a rename there is a format change.

`as` sits between `PREC_FACTOR` and `PREC_UNARY`, which makes it bind tighter
than arithmetic and looser than unary minus. `-1 as number` therefore asserts
on the `1`. that is the same rule every C-like language uses for a cast, and
it is why `1 + 2 as number` needs no parentheses to mean what it looks like.

### const

a local `const` is decided at compile time. `named_variable()` sees the
`OP_GET_LOCAL` it is about to emit and the `is_const` flag on the local, and
refuses. no runtime cost and no extra instruction.

a global `const` cannot work that way, so the flag lives on the table entry
instead. `OP_DEFINE_GLOBAL_CONST` is `OP_DEFINE_GLOBAL` with one difference:
it passes `is_const = true` to `table_set()`, and the entry remembers it. the
check then happens in `OP_SET_GLOBAL`, via `table_is_const()`.

the reason it has to be the table and not the compiler is ordering. a name can
be made const by a module that this file imported, and the import runs at
runtime, long after this file was compiled. nothing the compiler saw when it
read the assignment says anything about whether the binding is const. the
table is the only place that knows.

`let` on an existing const is refused in `OP_DEFINE_GLOBAL` as well, and that
one is not obvious. redeclaring an ordinary global is allowed and overwrites.
Doing it to a const would leave the entry's flag set, so the "new" binding
would be unwritable too: the source would read as a plain `let` and the
language would disagree.

the flag has to survive a table rehash, since globals are rehashed as a script
defines more names. `adjust_capacity()` copies it for that reason; without
that, a script that declared a const and then defined twenty more names would
quietly find the const writable.

## call frames and upvalues

the call stack is a fixed array of `CallFrame`, 256 entries. each frame holds
the running `ObjClosure`, the instruction pointer, and `slots` - a pointer to
the callee value on the value stack. argument 0 is `slots[1]`, which is why
`call()` can set `slots = stack_top - argc - 1`.

`capture_upvalue()` keeps the open upvalue list sorted by descending stack
address. when a function captures a local, the list is walked until an entry
points at the same slot, and the existing one is reused. two closures sharing a
variable share the upvalue, which is what makes by-reference capture work.

`close_upvalues()` copies the value out of the stack slot into the upvalue's
own storage and repoints the upvalue at that storage. the VM emits
`OP_CLOSE_UPVALUE` for any local leaving scope that was captured. a plain
`OP_POP` would leave the upvalue pointing at a slot that the next call reuses.

## garbage collection

non-moving, stop-the-world, mark and sweep. collection triggers when
`bytes_allocated` passes `next_gc`, which is set to twice the heap size after
each cycle.

roots:

- the value stack, from `vm->stack` to `vm->stack_top`
- the closure in every live `CallFrame`
- the open upvalue list
- the globals table
- every `Compiler` on the chain from `vm->current_compiler`

`mark_object()` sets the mark bit and pushes onto an explicit gray stack. the
gray stack grows with plain `realloc`, not `fl_reallocate`, because allocating
during marking must not re-enter the collector. `trace_references()` drains it
iteratively, so object graphs of any depth do not blow the C stack.

`collect_garbage()` order matters:

1. `mark_roots()`
2. `trace_references()`
3. `table_remove_white()` - drops unmarked keys from the interning table
4. `sweep()`

step 3 must happen before step 4, because the intern table holds weak
references. sweep first and the collector reads freed memory to decide what to
free.

## object types

| type | representation |
|---|---|
| `OBJ_STRING` | `ObjString` with a trailing `char chars[]` |
| `OBJ_LIST` | `Value *items` with `count`/`capacity` |
| `OBJ_TABLE` | parallel `ObjString **keys` and `Value *values` |
| `OBJ_FUNCTION` | `Chunk` plus `arity` and `name` |
| `OBJ_CLOSURE` | `ObjFunction *` plus an upvalue pointer array |
| `OBJ_NATIVE` | C function pointer plus `arity` |
| `OBJ_UPVALUE` | `Value *location`, or `closed` once closed |

`ObjString` uses a flexible array member so the header and the bytes are one
allocation. with thousands of interned strings, two allocations per string is
a measurable difference.

`ObjTable` is what flint code sees as a table. `Table` in `table.h` is the
internal C-level hash table used for globals and interning. they share no code
and the similarity is entirely accidental.

`Entry` carries an `is_const` flag alongside the key and the value. it is
almost always false, and it is there for one case: a global bound with `const`,
which has to be a property of the binding rather than of any one assignment.
see the const section above for why that cannot be a compiler-side check.
