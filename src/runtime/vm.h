/* SPDX-License-Identifier: MIT */
/*
 * The VM: bytecode interpreter, call frames, and GC roots.
 */
#ifndef FL_VM_H
#define FL_VM_H

#include "chunk.h"
#include "object.h"
#include "table.h"
#include "value.h"

typedef enum {
	INTERPRET_OK,
	INTERPRET_COMPILE_ERROR,
	INTERPRET_RUNTIME_ERROR
} InterpretResult;

/*
 * One entry per active call. slots points at the callee value on the value
 * stack, so argument 0 is slots[1] and local slot 0 is the function itself.
 * That single pointer is why the compiler can use one index for both.
 */
typedef struct {
	ObjClosure *closure;
	uint8_t *ip;
	Value *slots;
} CallFrame;

struct VM {
	CallFrame frames[FRAMES_MAX];
	int frame_count;

	/*
	 * Where this script started, so a failure can unwind to it and
	 * nothing further.
	 *
	 * Both fields, and the distinction matters. Unwinding to a frame
	 * index alone is not enough: the importing script's frame has a
	 * live call on its stack (the import_file callee and its path
	 * argument), and dropping those leaves call_value's
	 * `stack_top -= arg_count + 1` subtracting slots that are already
	 * gone, which walks off the bottom of the array. base_top is the
	 * exact stack position to return to, captured on entry.
	 *
	 * Set by vm_interpret() on entry, restored on exit, so it always
	 * describes the innermost active script.
	 */
	int base_frame;
	Value *base_top;

	Value stack[STACK_MAX];
	Value *stack_top;

	Table globals; /* name -> value, for top-level variables */
	Table strings; /* weak. the intern table. */

	ObjUpvalue *open_upvalues; /* sorted by descending stack address */
	Obj *objects; /* every live object, for sweeping */

	size_t bytes_allocated;
	size_t next_gc;

	/* explicit gray stack. see the tracing note in memory.c. */
	int gray_count;
	int gray_capacity;
	Obj **gray_stack;
};

void vm_init(VM *vm);
void vm_free(VM *vm);

/* compile and run. the entry point for the repl, files and modules alike. */
InterpretResult vm_interpret(VM *vm, const char *source);

/*
 * Stack primitives. These are also the GC roots, so an object must be on the
 * stack before any allocation that could trigger a collection.
 */
void vm_push(VM *vm, Value value);
Value vm_pop(VM *vm);

/* prints the message and a stack trace, then unwinds the stack. */
void vm_runtime_error(VM *vm, const char *format, ...);

void vm_define_native(VM *vm, const char *name, NativeFn function, int arity);

#endif /* FL_VM_H */
