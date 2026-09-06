/* SPDX-License-Identifier: MIT */
/*
 * Internal hash table: open addressing with linear probing.
 *
 * Keys are interned strings, so a hit is a pointer compare. Capacity is
 * always a power of two, which turns the wraparound into a mask instead of a
 * modulo.
 */
#include "table.h"
#include "memory.h"
#include "object.h"
#include "stdint.h"
#include "value.h"

#include <string.h>
/*
 * Grow at 75%. Higher means shorter probe chains and wasted slots. Lower
 * means less memory. For a table holding globals this is not a decision
 * anyone should spend time on.
 */
#define TABLE_MAX_LOAD 0.75

void table_init(Table *table)
{
	table->count = 0;
	table->capacity = 0;
	table->entries = NULL;
}

void table_free(VM *vm, Table *table)
{
	FREE_ARRAY(vm, Entry, table->entries, table->capacity);
	table_init(table);
}

/*
 * Find the slot for a key, or the slot it should go in.
 *
 * An empty slot is key == NULL and value == nil. A tombstone is key == NULL
 * and value == true. Probing continues past tombstones, because the key we
 * want may be further along the same chain, but the first tombstone is
 * remembered and returned if we reach the end without a hit. Without that
 * you would insert a duplicate and quietly lose one of the two.
 */
static Entry *find_entry(Entry *entries, int capacity, ObjString *key)
{
	uint32_t index = key->hash & (capacity - 1);
	Entry *tombstone = NULL;

	for (;;) {
		Entry *entry = &entries[index];
		if (entry->key == NULL) {
			if (IS_NIL(entry->value))
				return tombstone != NULL ? tombstone : entry;
			else if (tombstone == NULL)
				tombstone = entry;
		} else if (entry->key == key) {
			return entry;
		}

		index = (index + 1) & (capacity - 1);
	}
}

/*
 * Allocate a new array and rehash into it. Called on growth and nowhere
 * else; a table full of tombstones but few live keys is not reclaimed, which
 * is a known limitation and has not mattered so far.
 */
static void adjust_capacity(VM *vm, Table *table, int capacity)
{
	Entry *entries = ALLOCATE(vm, Entry, capacity);
	for (int i = 0; i < capacity; i++) {
		entries[i].key = NULL;
		entries[i].value = NIL_VAL;
		entries[i].is_const = false;
	}

	/*
	 * Rehash from scratch rather than copying, because dropping
	 * tombstones means the surviving count is not the old count.
	 */
	table->count = 0;
	for (int i = 0; i < table->capacity; i++) {
		Entry *entry = &table->entries[i];
		if (entry->key == NULL)
			continue;

		Entry *dest = find_entry(entries, capacity, entry->key);
		dest->key = entry->key;
		dest->value = entry->value;
		/* const has to survive a rehash, or a table that grows
		 * after a const was defined would silently make it writable */
		dest->is_const = entry->is_const;
		table->count++;
	}

	FREE_ARRAY(vm, Entry, table->entries, table->capacity);
	table->entries = entries;
	table->capacity = capacity;
}

bool table_get(Table *table, ObjString *key, Value *value)
{
	if (table->count == 0)
		return false;

	Entry *entry = find_entry(table->entries, table->capacity, key);
	if (entry->key == NULL)
		return false;

	*value = entry->value;
	return true;
}

/*
 * Insert or overwrite, optionally marking the binding const.
 *
 * Returns true if the key was not already present, which is how the compiler
 * tells `let` from assignment.
 */
bool table_set(VM *vm, Table *table, ObjString *key, Value value, bool is_const)
{
	if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
		int capacity = GROW_CAPACITY(table->capacity);
		adjust_capacity(vm, table, capacity);
	}

	Entry *entry = find_entry(table->entries, table->capacity, key);

	/* reusing a tombstone does not increase the count */
	bool is_new_key = entry->key == NULL;
	if (is_new_key && IS_NIL(entry->value))
		table->count++;

	/*
	 * is_const applies to a new binding only. Overwriting a live const
	 * entry keeps the flag, so `const x = 1; let x = 2` cannot quietly
	 * downgrade the binding and leave the earlier const meaningless.
	 *
	 * The test is is_new_key, not "was this slot a tombstone": a reused
	 * tombstone still holds is_const from whatever lived there before it
	 * was deleted, so OR-ing that in would resurrect the flag of a
	 * binding that no longer exists. is_new_key is the only test that
	 * separates a live occupant from a dead slot.
	 */
	if (is_new_key)
		entry->is_const = is_const;

	entry->key = key;
	entry->value = value;
	return is_new_key;
}

/*
 * Is this name bound with const?
 *
 * An unknown name answers false, and the caller has to treat that as "not
 * const" rather than as an error: `x = 1` for an undeclared x is a different
 * diagnostic, reported by the VM's own undefined-variable path.
 */
bool table_is_const(Table *table, ObjString *key)
{
	if (table->count == 0)
		return false;

	Entry *entry = find_entry(table->entries, table->capacity, key);
	return entry->key != NULL && entry->is_const;
}

bool table_delete(Table *table, ObjString *key)
{
	if (table->count == 0)
		return false;

	Entry *entry = find_entry(table->entries, table->capacity, key);
	if (entry->key == NULL)
		return false;

	/*
	 * A tombstone, not an empty slot. Emptying the slot would cut the
	 * probe chain in half and any key that probed past it would become
	 * unreachable. Tombstones still count toward the load factor, so a
	 * delete-heavy table grows instead of degrading, and only a rehash
	 * reclaims them.
	 */
	entry->key = NULL;
	entry->value = TRUE_VAL;
	return true;
}

/* used to copy exported names out of a module. */
void table_add_all(VM *vm, Table *from, Table *to)
{
	for (int i = 0; i < from->capacity; i++) {
		Entry *entry = &from->entries[i];
		if (entry->key != NULL)
			table_set(vm,
			        to,
			        entry->key,
			        entry->value,
			        entry->is_const);
	}
}

/*
 * Lookup by content rather than by pointer. Only the interning path needs
 * this: to intern a string you have not allocated yet, so you cannot have
 * its pointer. Compares length and hash first to keep memcmp off the hot
 * path.
 */
ObjString *table_find_string(
        Table *table, const char *chars, int length, uint32_t hash)
{
	if (table->count == 0)
		return NULL;

	uint32_t index = hash & (table->capacity - 1);
	for (;;) {
		Entry *entry = &table->entries[index];
		if (entry->key == NULL) {
			if (IS_NIL(entry->value))
				return NULL;
		} else if (entry->key->length == length &&
		           entry->key->hash == hash &&
		           memcmp(entry->key->chars, chars, length) == 0) {
			return entry->key;
		}

		index = (index + 1) & (table->capacity - 1);
	}
}

/*
 * Drop keys whose string was not marked. Runs between tracing and sweeping,
 * because the intern table holds weak references: once the sweep has run the
 * keys are freed memory and touching them is undefined behaviour.
 */
void table_remove_white(Table *table)
{
	for (int i = 0; i < table->capacity; i++) {
		Entry *entry = &table->entries[i];
		if (entry->key != NULL && !entry->key->obj.is_marked)
			table_delete(table, entry->key);
	}
}
