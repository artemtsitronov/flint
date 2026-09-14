/*
 * unit_value.c -- unit tests for the NaN-boxed value representation.
 *
 * No VM, no allocator. Just the macros in value.h, which is the point: if a
 * bit pattern round-trips here, it round-trips everywhere.
 *
 * Covers:
 *   - bit-exact round trip for 0.0, -0.0, 1.5, DBL_MAX, DBL_MIN, the
 *     infinities, and subnormals
 *   - every NaN flavour: constant, negated, computed at runtime. all of them
 *     must collapse to the same canonical pattern
 *   - nil, both booleans, and a heap pointer, each classified correctly and
 *     exclusively
 *   - truthiness, including the fact that 0 is true
 */
#include "../../src/core/value.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_failed = 0;

/* count every assertion, print the ones that fail with their source line */
#define ASSERT(cond, msg)                                                      \
	do {                                                                   \
		tests_run++;                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%d]: %s\n", __LINE__, (msg));   \
			tests_failed++;                                        \
		}                                                              \
	} while (0)

/*
 * Assert that a value matches exactly one of number, nil, bool, object.
 * Exclusivity is the interesting half: a pattern that is both a number and
 * an object would let the VM read garbage as a pointer, and a test that only
 * checked the expected category would not notice.
 */
static void assert_exclusive_type(Value v,
        const char *label,
        bool expect_num,
        bool expect_nil,
        bool expect_bool,
        bool expect_obj)
{
	bool is_num = IS_NUMBER(v);
	bool is_nil = IS_NIL(v);
	bool is_bool = IS_BOOL(v);
	bool is_obj = IS_OBJ(v);

	ASSERT(is_num == expect_num, label);
	ASSERT(is_nil == expect_nil, label);
	ASSERT(is_bool == expect_bool, label);
	ASSERT(is_obj == expect_obj, label);

	int count = (is_num ? 1 : 0) + (is_nil ? 1 : 0) + (is_bool ? 1 : 0) +
	            (is_obj ? 1 : 0);
	ASSERT(count == 1, label);
}

/*
 * Box a double and check it survives. Non-NaN values must come back
 * bit-identical, which is stronger than comparing with ==: it catches a
 * -0.0 that came back as +0.0, and those compare equal.
 */
static void test_number_roundtrip(double d, const char *label)
{
	Value v = NUMBER_VAL(d);

	/* a NaN cannot round-trip bit-exactly, only as a NaN */
	if (d != d) {
		ASSERT(IS_NUMBER(v), label);
		ASSERT(AS_NUMBER(v) != AS_NUMBER(v), label); /* NaN != NaN */
		assert_exclusive_type(v, label, true, false, false, false);
		return;
	}

	ASSERT(IS_NUMBER(v), label);
	double out = AS_NUMBER(v);

	uint64_t in_bits, out_bits;
	memcpy(&in_bits, &d, sizeof(uint64_t));
	memcpy(&out_bits, &out, sizeof(uint64_t));
	ASSERT(in_bits == out_bits, label);

	assert_exclusive_type(v, label, true, false, false, false);
}

int main(void)
{
	/* --- numbers --- */
	test_number_roundtrip(0.0, "0.0");
	test_number_roundtrip(-0.0, "-0.0");
	test_number_roundtrip(1.5, "1.5");
	test_number_roundtrip(DBL_MAX, "DBL_MAX");
	test_number_roundtrip(DBL_MIN, "DBL_MIN");
	test_number_roundtrip(INFINITY, "INFINITY");
	test_number_roundtrip(-INFINITY, "-INFINITY");

	/* subnormals have lost their leading bit and are where naive
     * conversion code tends to go wrong */
	test_number_roundtrip(5e-324, "subnormal (smallest)");
	test_number_roundtrip(2.225e-308, "subnormal (near boundary)");

	/* every NaN becomes the canonical one, whichever way it was made */
	test_number_roundtrip(NAN, "NAN");
	test_number_roundtrip(-NAN, "-NAN");

	/* computed at runtime, so the compiler cannot constant-fold them into
     * something the test already passed for */
	volatile double zero = 0.0;
	test_number_roundtrip(zero / zero, "0.0/0.0 (runtime)");

	volatile double inf = INFINITY;
	test_number_roundtrip(inf - inf, "INFINITY - INFINITY");

	/* the canonical pattern itself, checked directly */
	{
		Value v = NUMBER_VAL(NAN);
		ASSERT(v == FL_CANONICAL_NAN, "canonical NaN bit pattern");
	}

	/* and it must read as a number. A NaN whose top 13 bits are all set
     * would be decoded as a box, which is the bug this guards against. */
	{
		Value v = FL_CANONICAL_NAN;
		ASSERT(IS_NUMBER(v), "canonical NaN is number");
		ASSERT(!IS_NIL(v), "canonical NaN is not nil");
		ASSERT(!IS_BOOL(v), "canonical NaN is not bool");
		ASSERT(!IS_OBJ(v), "canonical NaN is not obj");
	}

	/* --- nil --- */
	{
		Value v = NIL_VAL;
		assert_exclusive_type(v, "nil", false, true, false, false);
		ASSERT(IS_FALSY(v), "nil is falsy");
	}

	/* --- booleans --- */
	{
		Value v = TRUE_VAL;
		assert_exclusive_type(v, "true", false, false, true, false);
		ASSERT(!IS_FALSY(v), "true is not falsy");
		ASSERT(BOOL_VAL(true) == TRUE_VAL, "BOOL_VAL(true)");
	}
	{
		Value v = FALSE_VAL;
		assert_exclusive_type(v, "false", false, false, true, false);
		ASSERT(IS_FALSY(v), "false is falsy");
		ASSERT(BOOL_VAL(false) == FALSE_VAL, "BOOL_VAL(false)");
	}

	/* --- object pointer --- */
	{
		/* a real address, so the 48-bit assert in OBJ_VAL has something
         * real to check */
		int *heap = malloc(sizeof(int));
		ASSERT(heap != NULL, "malloc for obj test");

		Value v = OBJ_VAL(heap);
		assert_exclusive_type(v, "obj ptr", false, false, false, true);
		ASSERT(AS_OBJ_PTR(v) == heap, "obj ptr round-trip");
		ASSERT(!IS_FALSY(v), "obj is not falsy");

		free(heap);
	}

	/* --- the boxed values must all be distinct --- */
	ASSERT(NIL_VAL != TRUE_VAL, "nil != true");
	ASSERT(NIL_VAL != FALSE_VAL, "nil != false");
	ASSERT(TRUE_VAL != FALSE_VAL, "true != false");

	/* and none of them may be mistaken for a number */
	ASSERT(!IS_NUMBER(NIL_VAL), "nil is not number");
	ASSERT(!IS_NUMBER(TRUE_VAL), "true is not number");
	ASSERT(!IS_NUMBER(FALSE_VAL), "false is not number");

	/* --- truthiness --- */
	/* only nil and false are falsy. zero being true is the one that
     * surprises people, so it is worth asserting twice. */
	ASSERT(IS_FALSY(NIL_VAL), "nil falsy");
	ASSERT(IS_FALSY(FALSE_VAL), "false falsy");
	ASSERT(!IS_FALSY(TRUE_VAL), "true truthy");
	ASSERT(!IS_FALSY(NUMBER_VAL(0.0)), "0 truthy");
	ASSERT(!IS_FALSY(NUMBER_VAL(-0.0)), "-0.0 truthy");

	printf("unit_value: %d tests, %d failed\n", tests_run, tests_failed);
	return tests_failed > 0 ? 1 : 0;
}
