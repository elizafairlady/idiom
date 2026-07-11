#ifndef IDM_EXPAND_H
#define IDM_EXPAND_H

#include "idiom/artifact.h"
#include "idiom/core.h"
#include "idiom/reader.h"
#include "idiom/vm.h"

typedef struct IdmMacroRunner IdmMacroRunner;

typedef bool (*IdmMacroInvokeFn)(void *user, IdmRuntime *rt, uint32_t payload, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err);

struct IdmMacroRunner {
    void *user;
    IdmMacroInvokeFn invoke;
};

bool idm_expand_read_source_string(IdmRuntime *rt, const char *file, const char *source, IdmSyntax **out, IdmError *err);
bool idm_expand_source_string(IdmRuntime *rt, const char *file, const char *source, IdmCore **out, IdmError *err);
typedef struct {
    unsigned char source_hash[32];
    unsigned char action_hash[32];
    IdmArtifactDep *deps;
    size_t dep_count;
    bool cacheable;
} IdmExpandTrace;
void idm_expand_trace_destroy(IdmExpandTrace *trace);
bool idm_expand_trace_add_reads(IdmExpandTrace *trace, const IdmPhaseReads *reads, IdmError *err);
bool idm_expand_source_string_traced(IdmRuntime *rt, const char *file, const char *source, IdmCore **out, IdmExpandTrace *trace, IdmError *err);
typedef enum {
    IDM_REPL_OK,
    IDM_REPL_INCOMPLETE,
    IDM_REPL_ERROR
} IdmReplStatus;

IdmRepl *idm_repl_create(IdmRuntime *rt, IdmError *err);
IdmRepl *idm_repl_create_with(IdmRuntime *rt, IdmScheduler *sched, IdmError *err);
void idm_repl_destroy(IdmRepl *repl);
IdmReplStatus idm_repl_compile(IdmRepl *repl, const char *source, IdmValue *out_thunk, uint64_t *out_token, IdmError *err);
void idm_repl_abort(IdmRepl *repl, uint64_t token);
bool idm_repl_run(IdmRepl *repl, IdmValue thunk, IdmValue *out_value, IdmError *err);
IdmScheduler *idm_repl_scheduler(IdmRepl *repl);
bool idm_repl_loop_thunk(IdmRepl *repl, const char *source, IdmValue *out_thunk, IdmError *err);
bool idm_expand_package_artifact_serialize(IdmRuntime *rt, const char *path, IdmBuffer *out, IdmError *err);
bool idm_expand_package_action_hash(IdmRuntime *rt, const char *path, unsigned char out[32], IdmError *err);
bool idm_expand_surface_dump(IdmRuntime *rt, const char *prelude, IdmBuffer *out, IdmError *err);
bool idm_expand_explain_source(IdmRuntime *rt, const char *file, const char *source, unsigned line, unsigned column, bool json, IdmBuffer *out, IdmError *err);

#endif
