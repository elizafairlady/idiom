#include "idiom/actor.h"
#include "idiom/bytecode.h"
#include "idiom/common.h"
#include "idiom/core.h"
#include "idiom/expand.h"
#include "idiom/reader.h"
#include "idiom/syntax.h"
#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IDM_VERSION
#define IDM_VERSION "0.0.0-dev"
#endif

static IdmSyntax *with_shell(IdmSyntax *program, const char *file, IdmError *err) {
    IdmSpan span = idm_span_unknown(file);
    span.line = 1;
    span.column = 1;
    IdmSyntax *wrapped = idm_syn_list(IDM_SEQ_PAREN, span);
    IdmSyntax *expr = idm_syn_list(IDM_SEQ_PAREN, span);
    IdmSyntax *marker = idm_syn_word("%-package-begin", span);
    IdmSyntax *expr_marker = idm_syn_word("%-expr", span);
    IdmSyntax *head = idm_syn_word("implements", span);
    IdmSyntax *target = idm_syn_word("std/shell", span);
    bool ok = wrapped && expr && marker && expr_marker && head && target;
    ok = ok && idm_syn_append(expr, expr_marker);
    ok = ok && idm_syn_append(expr, head);
    ok = ok && idm_syn_append(expr, target);
    ok = ok && idm_syn_append(wrapped, marker);
    ok = ok && idm_syn_append(wrapped, expr);
    for (size_t i = 1; ok && i < program->as.seq.count; i++) {
        IdmSyntax *item = idm_syn_clone(program->as.seq.items[i]);
        ok = item != NULL && idm_syn_append(wrapped, item);
        if (!ok) idm_syn_free(item);
    }
    if (!ok) {
        idm_syn_free(wrapped);
        if (expr && (!wrapped || wrapped->as.seq.count == 0)) idm_syn_free(expr);
        idm_error_oom(err, span);
        return NULL;
    }
    return wrapped;
}

static char **g_cli_args = NULL;
static size_t g_cli_arg_count = 0;

static int run_shell_source(const char *file, const char *source, bool print_result) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    rt.cli_args = g_cli_args;
    rt.cli_arg_count = g_cli_arg_count;
    IdmError err;
    idm_error_init(&err);
    int status = 1;
    IdmSyntax *program = NULL;
    IdmSyntax *wrapped = NULL;
    IdmCore *core = NULL;
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmScheduler *sched = NULL;
    if (!idm_reader_read_string(file, source, &program, &err)) goto done;
    wrapped = with_shell(program, file, &err);
    if (!wrapped) goto done;
    if (!idm_expand_syntax(&rt, wrapped, &core, &err)) goto done;
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(core, &module, &main_fn, &err)) goto done;
    sched = idm_sched_create(&rt, &module, &err);
    if (!sched) goto done;
    IdmValue out = idm_nil();
    if (!idm_sched_run_main(sched, main_fn, &out, &err)) goto done;
    if (print_result) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        if (idm_value_write(&buf, out) && idm_buf_append_char(&buf, '\n')) fputs(buf.data, stdout);
        idm_buf_destroy(&buf);
    }
    status = 0;
done:
    if (err.present) idm_error_fprint(stderr, &err);
    idm_error_clear(&err);
    idm_sched_destroy(sched);
    idm_core_free(core);
    idm_syn_free(wrapped);
    idm_syn_free(program);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    return status;
}

static int run_shell_file(const char *path) {
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    size_t len = 0;
    if (!idm_read_file(path, &source, &len, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        return 1;
    }
    int status = run_shell_source(path, source, false);
    free(source);
    return status;
}

static void usage(FILE *out) {
    fprintf(out, "ish — the idiom shell\n");
    fprintf(out, "  ish FILE.ish        run a shell script\n");
    fprintf(out, "  ish -c 'COMMANDS'   run shell commands\n");
    fprintf(out, "  ish --version\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("ish %s (idiom)\n", IDM_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "-c") == 0) return run_shell_source("<command>", argv[2], false);
    if (argc >= 2 && argv[1][0] != '-') {
        g_cli_args = argv + 2;
        g_cli_arg_count = (size_t)(argc - 2);
        return run_shell_file(argv[1]);
    }
    usage(stderr);
    return 64;
}
