#include "idiom/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int failures = 0;

static void check(bool cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "grow test failed: %s\n", msg);
        failures++;
    }
}

static void check_next_capacity(void) {
    size_t out = 0;
    check(idm_next_capacity(8u, 4u, 3u, &out) && out == 8u, "keeps existing capacity");
    check(idm_next_capacity(0u, 4u, 1u, &out) && out == 4u, "uses seed");
    check(idm_next_capacity(4u, 4u, 5u, &out) && out == 8u, "doubles once");
    check(idm_next_capacity(4u, 4u, 17u, &out) && out == 32u, "doubles to need");
    check(!idm_next_capacity(SIZE_MAX / 2u + 1u, 4u, SIZE_MAX, &out), "rejects overflow");
}

static void check_grow_single(void) {
    int *items = NULL;
    size_t cap = 0;
    check(idm_grow((void **)&items, &cap, sizeof(*items), 2u, 1u) && cap == 2u, "single grow seed");
    items[0] = 7;
    items[1] = 9;
    check(idm_grow((void **)&items, &cap, sizeof(*items), 2u, 3u) && cap == 4u, "single grow doubles");
    check(items[0] == 7 && items[1] == 9, "single grow preserves data");
    free(items);
}

static void check_grow_multi(void) {
    int *ints = NULL;
    char *chars = NULL;
    size_t cap = 0;
    IdmGrowItem items[] = {
        { .base = (void **)&ints, .elem_size = sizeof(*ints) },
        { .base = (void **)&chars, .elem_size = sizeof(*chars) },
    };
    check(idm_growv(items, 2u, &cap, 2u, 2u) && cap == 2u, "multi grow seed");
    ints[0] = 11;
    ints[1] = 13;
    chars[0] = 'a';
    chars[1] = 'b';
    check(idm_growv(items, 2u, &cap, 2u, 3u) && cap == 4u, "multi grow doubles");
    check(ints[0] == 11 && ints[1] == 13 && chars[0] == 'a' && chars[1] == 'b', "multi grow preserves data");
    free(ints);
    free(chars);
}

int idm_unit_grow(void) {
    check_next_capacity();
    check_grow_single();
    check_grow_multi();
    return failures == 0 ? 0 : 1;
}
