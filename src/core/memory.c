/* SPDX-License-Identifier: MIT */
/*
 * Allocation and the garbage collector: non-moving, stop-the-world,
 * mark and sweep.
 *
 * Objects are never moved, so nothing in the VM holds a raw interior pointer
 * that a collector could invalidate. The one place that comes close is an open
 * upvalue, which points into the value stack, and that is handled by closing
 * upvalues before the frame goes away.
 *
 * Running out of memory is not recoverable and is not worth pretending
 * otherwise, so the collector panics on allocation failure.
 */
#include "memory.h"
#include "chunk.h"
#include "compiler.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
/* collect again once the heap has doubled since the last collection. */
#define GC_HEAP_GROW_FACTOR 2

static void mark_value(VM *vm, Value value);

/*
 * The one allocator. Also the GC trigger, which is why every growing
 * array in the VM goes through it: the threshold is only checked on
 * allocation, so a collection happens when memory is actually needed.
 *
 * A vm of NULL means no collector, which is what the unit tests use.
 */
void *fl_reallocate(VM *vm, void *pointer, size_t old_size, size_t new_size)
{
	if (vm != NULL) {
		vm->bytes_allocated += new_size - old_size;
		if (new_size > old_size) {
#ifdef FL_GC_STRESS
			/* collect on every single allocation. slow, and it
			 * finds roots that are missing in the normal build. */
			collect_garbage(vm);
#else
			if (vm->bytes_allocated > vm->next_gc)
				collect_garbage(vm);
#endif
		}
	}

	if (new_size == 0) {
		free(pointer);
		return NULL;
	}

	void *result = realloc(pointer, new_size);
	if (result == NULL) {
		fprintf(stderr, "Out of memory.\n");
		exit(1);
	}
	return result;
}

/*
 * Grey an object and queue it for tracing. The mark bit makes this
 * idempotent, so a shared object is queued once and an object graph with
 * cycles terminates.
 *
 * This is the only correct way to mark something. is_marked = true on its own
 * survives the object but not what it references, because nothing ever
 * blackens it.
 */
void mark_object(VM *vm, Obj *object)
{
	if (object == NULL || object->is_marked)
		return;

	object->is_marked = true;

	if (vm->gray_capacity < vm->gray_count + 1) {
		vm->gray_capacity = GROW_CAPACITY(vm->gray_capacity);
		/*
		 * Plain realloc, deliberately. Going through fl_reallocate
		 * here would let growing the gray stack trigger a collection
		 * in the middle of a collection.
		 */
		vm->gray_stack = (Obj **)realloc(
		        vm->gray_stack, sizeof(Obj *) * vm->gray_capacity);
		if (vm->gray_stack == NULL) {
			fprintf(stderr, "Out of memory in gray stack.\n");
			exit(1);
		}
	}

	vm->gray_stack[vm->gray_count++] = object;
}

static void mark_value(VM *vm, Value value)
{
	if (IS_OBJ(value))
		mark_object(vm, AS_OBJ(value));
}

static void mark_array(VM *vm, ValueArray *array)
{
	for (int i = 0; i < array->count; i++)
		mark_value(vm, array->values[i]);
}

/*
 * Mark everything one object points at. This is where the collector needs to
 * know about every new object type: add a case here or the new type's
 * children get swept out from under it.
 */
static void blacken_object(VM *vm, Obj *object)
{
	switch (object->type) {
	case OBJ_STRING:
		/*
		 * a flexible array member of bytes, and a C function pointer
		 * for OBJ_NATIVE. neither is a heap pointer the collector
		 * traces, so they share the one empty arm.
		 */
	case OBJ_NATIVE:
		break;
	case OBJ_UPVALUE:
		mark_value(vm, ((ObjUpvalue *)object)->closed);
		break;
	case OBJ_FUNCTION: {
		ObjFunction *function = (ObjFunction *)object;
		mark_object(vm, (Obj *)function->name);
		mark_array(vm, &function->chunk.constants);
		break;
	}
	case OBJ_CLOSURE: {
		ObjClosure *closure = (ObjClosure *)object;
		mark_object(vm, (Obj *)closure->function);
		for (int i = 0; i < closure->upvalue_count; i++)
			mark_object(vm, (Obj *)closure->upvalues[i]);
		break;
	}
	case OBJ_LIST: {
		ObjList *list = (ObjList *)object;
		for (int i = 0; i < list->count; i++)
			mark_value(vm, list->items[i]);
		break;
	}
	case OBJ_TABLE: {
		ObjTable *table = (ObjTable *)object;
		for (int i = 0; i < table->count; i++) {
			mark_object(vm, (Obj *)table->keys[i]);
			mark_value(vm, table->values[i]);
		}
		break;
	}
	}
}

static void mark_table(VM *vm, Table *table)
{
	for (int i = 0; i < table->capacity; i++) {
		Entry *entry = &table->entries[i];
		mark_object(vm, (Obj *)entry->key);
		mark_value(vm, entry->value);
	}
}

/*
 * Everything the collector can reach without being pointed to. If you add a
 * place the VM stashes an object, add it here.
 */
static void mark_roots(VM *vm)
{
	/* the value stack, up to the live top */
	for (Value *slot = vm->stack; slot < vm->stack_top; slot++)
		mark_value(vm, *slot);

	/* the function running in each frame */
	for (int i = 0; i < vm->frame_count; i++)
		mark_object(vm, (Obj *)vm->frames[i].closure);

	/* captured locals still pointing into the stack */
	for (ObjUpvalue *upvalue = vm->open_upvalues; upvalue != NULL;
	        upvalue = upvalue->next) {
		mark_object(vm, (Obj *)upvalue);
	}

	/* top-level variables */
	mark_table(vm, &vm->globals);

	/*
	 * functions being compiled right now. They are not on the stack and
	 * not in any table, so without this a collection in the middle of a
	 * compile frees the half-built function.
	 */
	compiler_mark_roots(vm);
}

/*
 * Drain the gray stack iteratively. An explicit worklist rather than
 * recursion, so a deep list nesting does not blow the C stack.
 */
static void trace_references(VM *vm)
{
	while (vm->gray_count > 0) {
		Obj *object = vm->gray_stack[--vm->gray_count];
		blacken_object(vm, object);
	}
}

/*
 * Walk every object. Marked ones survive and get their mark bit cleared for
 * the next cycle; the rest are unlinked and freed. Unlinking while walking
 * is fine because we saved the next pointer first.
 */
static void sweep(VM *vm)
{
	Obj *previous = NULL;
	Obj *object = vm->objects;

	while (object != NULL) {
		if (object->is_marked) {
			object->is_marked = false;
			previous = object;
			object = object->next;
		} else {
			Obj *unreached = object;
			object = object->next;
			if (previous != NULL)
				previous->next = object;
			else
				vm->objects = object;

			free_object(vm, unreached);
		}
	}
}

void collect_garbage(VM *vm)
{
	if (vm == NULL)
		return;

	mark_roots(vm);
	trace_references(vm);

	/*
	 * Drop dead keys from the intern table before the sweep, not after.
	 * The table holds weak references, so sweeping first would leave it
	 * pointing at memory that has already been freed.
	 */
	table_remove_white(&vm->strings);

	sweep(vm);

	vm->next_gc = vm->bytes_allocated * GC_HEAP_GROW_FACTOR;
}

/*
 * Free one object and everything hanging off it. Sizes must match what
 * allocate_object() asked for or realloc misbehaves; that is why the string
 * case recomputes sizeof(ObjString) + length + 1 instead of using sizeof
 * alone.
 */
void free_object(VM *vm, Obj *object)
{
	switch (object->type) {
	case OBJ_STRING: {
		ObjString *string = (ObjString *)object;
		fl_reallocate(
		        vm, object, sizeof(ObjString) + string->length + 1, 0);
		break;
	}
	case OBJ_FUNCTION: {
		ObjFunction *function = (ObjFunction *)object;
		chunk_free(vm, &function->chunk);
		fl_reallocate(vm, object, sizeof(ObjFunction), 0);
		break;
	}
	case OBJ_NATIVE: {
		fl_reallocate(vm, object, sizeof(ObjNative), 0);
		break;
	}
	case OBJ_CLOSURE: {
		ObjClosure *closure = (ObjClosure *)object;
		FREE_ARRAY(vm,
		        ObjUpvalue *,
		        closure->upvalues,
		        closure->upvalue_count);
		fl_reallocate(vm, object, sizeof(ObjClosure), 0);
		break;
	}
	case OBJ_UPVALUE: {
		fl_reallocate(vm, object, sizeof(ObjUpvalue), 0);
		break;
	}
	case OBJ_LIST: {
		ObjList *list = (ObjList *)object;
		FREE_ARRAY(vm, Value, list->items, list->capacity);
		fl_reallocate(vm, object, sizeof(ObjList), 0);
		break;
	}
	case OBJ_TABLE: {
		ObjTable *table = (ObjTable *)object;
		FREE_ARRAY(vm, ObjString *, table->keys, table->capacity);
		FREE_ARRAY(vm, Value, table->values, table->capacity);
		fl_reallocate(vm, object, sizeof(ObjTable), 0);
		break;
	}
	}
}

/* shutdown. frees everything that is left, collector or no collector. */
void free_objects(VM *vm)
{
	Obj *object = vm->objects;
	while (object != NULL) {
		Obj *next = object->next;
		free_object(vm, object);
		object = next;
	}
	free(vm->gray_stack);
	vm->gray_stack = NULL;
	vm->gray_capacity = 0;
	vm->gray_count = 0;
}
