#ifndef IDM_EXPAND_H
#define IDM_EXPAND_H

#include "idiom/core.h"
#include "idiom/reader.h"

typedef struct IdmMacroRunner IdmMacroRunner;

typedef bool (*IdmMacroInvokeFn)(void *user, IdmRuntime *rt, uint32_t payload, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err);

struct IdmMacroRunner {
    void *user;
    IdmMacroInvokeFn invoke;
};

bool idm_expand_syntax(IdmRuntime *rt, const IdmSyntax *syntax, IdmCore **out, IdmError *err);
bool idm_expand_syntax_with_runner(IdmRuntime *rt, const IdmSyntax *syntax, IdmMacroRunner *runner, IdmCore **out, IdmError *err);
bool idm_expand_string(IdmRuntime *rt, const char *file, const char *source, IdmCore **out, IdmError *err);
typedef struct IdmRepl IdmRepl;
IdmRepl *idm_repl_create(IdmRuntime *rt, IdmError *err);
bool idm_repl_eval(IdmRepl *repl, const char *source, IdmValue *out_value, bool *out_has_value, IdmError *err);
void idm_repl_destroy(IdmRepl *repl);

bool idm_expand_string_with_runner(IdmRuntime *rt, const char *file, const char *source, IdmMacroRunner *runner, IdmCore **out, IdmError *err);

#endif
