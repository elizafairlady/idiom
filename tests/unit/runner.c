#include <stdio.h>

typedef int (*IdmUnitFn)(void);

int idm_unit_bytecode_record(void);
int idm_unit_byteprog(void);
int idm_unit_cli(void);
int idm_unit_gc(void);
int idm_unit_grow(void);
int idm_unit_intern(void);
int idm_unit_pattern_selector(void);
int idm_unit_primitive_registry(void);
int idm_unit_reader_escape(void);
int idm_unit_repl(void);
int idm_unit_regex_set(void);
int idm_unit_syntax_equal(void);
int idm_unit_wire_helpers(void);

typedef struct {
    const char *name;
    IdmUnitFn run;
} IdmUnit;

static const IdmUnit tests[] = {
    {"bytecode_record", idm_unit_bytecode_record},
    {"byteprog", idm_unit_byteprog},
    {"cli", idm_unit_cli},
    {"gc", idm_unit_gc},
    {"grow", idm_unit_grow},
    {"intern", idm_unit_intern},
    {"pattern_selector", idm_unit_pattern_selector},
    {"primitive_registry", idm_unit_primitive_registry},
    {"reader_escape", idm_unit_reader_escape},
    {"repl", idm_unit_repl},
    {"regex_set", idm_unit_regex_set},
    {"syntax_equal", idm_unit_syntax_equal},
    {"wire_helpers", idm_unit_wire_helpers},
};

int main(void) {
    size_t failed = 0;
    size_t count = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < count; i++) {
        int status = tests[i].run();
        if (status != 0) {
            fprintf(stderr, "FAIL unit/%s\n", tests[i].name);
            failed++;
        } else {
            printf("pass unit/%s\n", tests[i].name);
        }
    }
    if (failed != 0) {
        fprintf(stderr, "FAIL unit: %zu of %zu\n", failed, count);
        return 1;
    }
    printf("ok unit: %zu tests\n", count);
    return 0;
}
