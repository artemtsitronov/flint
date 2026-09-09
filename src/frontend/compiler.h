/* SPDX-License-Identifier: MIT */
/*
 * Single-pass pratt parser and bytecode compiler.
 */
#ifndef FL_COMPILER_H
#define FL_COMPILER_H

#include "chunk.h"
#include "object.h"
#include "vm.h"

/*
 * Compiles source into a top-level ObjFunction. Returns NULL if the source
 * had errors, in which case the function must not be used.
 */
ObjFunction *compile(VM *vm, const char *source);

/* called by the GC to mark functions that are still being compiled. */
void compiler_mark_roots(VM *vm);

#endif /* FL_COMPILER_H */
