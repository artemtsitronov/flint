/* SPDX-License-Identifier: MIT */
/*
 * Heap objects.
 *
 * Every object starts with an Obj header, which is what lets the collector
 * walk vm->objects without knowing the concrete type.
 */
#ifndef FL_OBJECT_H
#define FL_OBJECT_H

#include "chunk.h"
#include "common.h"
#include "value.h"

typedef struct VM VM;
typedef struct Obj Obj;
typedef struct ObjString ObjString;

typedef enum {
	OBJ_STRING,
	OBJ_FUNCTION,
	OBJ_NATIVE,
	OBJ_CLOSURE,
	OBJ_UPVALUE,
	OBJ_LIST,
	OBJ_TABLE
} ObjType;

struct Obj {
	ObjType type;
	bool is_marked;
	struct Obj *next; /* the collector's list of all objects */
};

/*
 * Header and bytes in one allocation. chars is a flexible array member, so
 * the string is copied into space that is already there.
 */
struct ObjString {
	Obj obj;
	int length;
	uint32_t hash;
	char chars[];
};

typedef struct {
	Obj obj;
	int arity;
	int upvalue_count;
	Chunk chunk;
	ObjString *name; /* NULL for the top-level script */
} ObjFunction;

/* a C function exposed to flint. arity of -1 means variadic. */
typedef Value (*NativeFn)(VM *vm, int argc, Value *argv);

typedef struct {
	Obj obj;
	NativeFn function;
	int arity;
} ObjNative;

/*
 * A captured variable. location points into the value stack while the
 * enclosing frame is alive, then at closed once the frame returns. Reads and
 * writes go through location either way, so nothing else has to care.
 */
typedef struct ObjUpvalue {
	Obj obj;
	Value *location;
	Value closed;
	struct ObjUpvalue *next;
} ObjUpvalue;

typedef struct {
	Obj obj;
	ObjFunction *function;
	ObjUpvalue **upvalues;
	int upvalue_count;
} ObjClosure;

typedef struct {
	Obj obj;
	int count;
	int capacity;
	Value *items;
} ObjList;

/*
 * A flint-level table. This is not the Table from table.h, which is the
 * internal hash table behind globals and string interning. They share a name
 * and nothing else.
 */
typedef struct {
	Obj obj;
	int count;
	int capacity;
	ObjString **keys; /* parallel arrays: insertion ordered */
	Value *values;
} ObjTable;

#define AS_OBJ(value)   ((Obj *)AS_OBJ_PTR(value))
#define OBJ_TYPE(value) (AS_OBJ(value)->type)

static inline bool is_obj_type(Value value, ObjType type)
{
	return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#define IS_STRING(value)      is_obj_type(value, OBJ_STRING)
#define IS_FUNCTION(value)    is_obj_type(value, OBJ_FUNCTION)
#define IS_NATIVE(value)      is_obj_type(value, OBJ_NATIVE)
#define IS_CLOSURE(value)     is_obj_type(value, OBJ_CLOSURE)
#define IS_LIST(value)        is_obj_type(value, OBJ_LIST)
#define IS_FLINT_TABLE(value) is_obj_type(value, OBJ_TABLE)

#define AS_STRING(value)      ((ObjString *)AS_OBJ_PTR(value))
#define AS_CSTRING(value)     (((ObjString *)AS_OBJ_PTR(value))->chars)
#define AS_FUNCTION(value)    ((ObjFunction *)AS_OBJ_PTR(value))
#define AS_NATIVE(value)      ((ObjNative *)AS_OBJ_PTR(value))
#define AS_CLOSURE(value)     ((ObjClosure *)AS_OBJ_PTR(value))
#define AS_LIST(value)        ((ObjList *)AS_OBJ_PTR(value))
#define AS_FLINT_TABLE(value) ((ObjTable *)AS_OBJ_PTR(value))

/* all of these allocate, so all of them can trigger a collection. */
ObjString *copy_string(VM *vm, const char *chars, int length);

/* takes ownership of chars, which must come from ALLOCATE. frees it either way. */
ObjString *take_string(VM *vm, char *chars, int length);

ObjFunction *new_function(VM *vm);
ObjNative *new_native(VM *vm, NativeFn function, int arity);
ObjClosure *new_closure(VM *vm, ObjFunction *function);
ObjUpvalue *new_upvalue(VM *vm, Value *slot);
ObjList *new_list(VM *vm);
ObjTable *new_flint_table(VM *vm);

/*
 * The name of a value's type, as a string. These are the same seven words
 * `type()` returns, so the language has exactly one vocabulary for types and
 * a cast can quote it in its error message without inventing a second
 * spelling. the returned pointer is a static string: do not free it.
 */
const char *flint_type_name(Value value);

/* the name of a cast target, given the tag the compiler emitted. */
const char *flint_type_name_of(FlType type);

/*
 * Does this value match the tag? The check behind `x as T`.
 *
 * Kept next to the type predicates on purpose: this is where the two agree,
 * and having the mapping written down twice is how a cast ends up accepting
 * something `type()` would call something else.
 */
bool value_has_type(Value value, FlType type);

/* debug printing. no trailing newline. */
void print_object(Value value);

/*
 * The same, but for any value rather than only an object.
 *
 * print_object() switches on OBJ_TYPE(), which reads a pointer out of the
 * value. A number, boolean or nil has no such pointer, so calling it on one
 * is a segfault. Anything that recurses into a container has to go through
 * this instead, or `print([1, 2])` dies on the first element.
 */
void print_value(Value value);

#endif /* FL_OBJECT_H */
