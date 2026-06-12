#ifndef IDM_EXPAND_H
#define IDM_EXPAND_H

#include "idiom/core.h"
#include "idiom/reader.h"
#include "idiom/vm.h"

typedef struct IdmMacroRunner IdmMacroRunner;

typedef bool (*IdmMacroInvokeFn)(void *user, IdmRuntime *rt, uint32_t payload, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err);

struct IdmMacroRunner {
    void *user;
    IdmMacroInvokeFn invoke;
};

bool idm_expand_syntax(IdmRuntime *rt, const IdmSyntax *syntax, IdmCore **out, IdmError *err);
bool idm_expand_syntax_with_runner(IdmRuntime *rt, const IdmSyntax *syntax, IdmMacroRunner *runner, IdmCore **out, IdmError *err);
bool idm_expand_string(IdmRuntime *rt, const char *file, const char *source, IdmCore **out, IdmError *err);
typedef enum {
    IDM_REPL_OK,
    IDM_REPL_INCOMPLETE,
    IDM_REPL_ERROR
} IdmReplStatus;

IdmRepl *idm_repl_create(IdmRuntime *rt, IdmError *err);
void idm_repl_destroy(IdmRepl *repl);
IdmReplStatus idm_repl_compile(IdmRepl *repl, const char *source, IdmValue *out_thunk, uint64_t *out_token, IdmError *err);
void idm_repl_abort(IdmRepl *repl, uint64_t token);
bool idm_repl_run(IdmRepl *repl, IdmValue thunk, IdmValue *out_value, IdmError *err);
IdmScheduler *idm_repl_scheduler(IdmRepl *repl);
uint64_t idm_repl_session_pid(const IdmRepl *repl);
void idm_repl_set_session_pid(IdmRepl *repl, uint64_t pid);
bool idm_repl_loop_thunk(IdmRepl *repl, const char *source, IdmValue *out_thunk, IdmError *err);

bool idm_expand_string_with_runner(IdmRuntime *rt, const char *file, const char *source, IdmMacroRunner *runner, IdmCore **out, IdmError *err);

#endif
