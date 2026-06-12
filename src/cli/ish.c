#include "idiom/actor.h"
#include "idiom/bytecode.h"
#include "idiom/common.h"
#include "idiom/core.h"
#include "idiom/expand.h"
#include "idiom/reader.h"
#include "idiom/syntax.h"
#include "idiom/value.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef IDM_VERSION
#define IDM_VERSION "0.0.0-dev"
#endif


static char **g_cli_args = NULL;
static size_t g_cli_arg_count = 0;

static int masked_status(int64_t n) {
    return (int)(((n % 256) + 256) % 256);
}

static bool atom_is(IdmValue v, const char *name) {
    return v.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(v.as.symbol), name) == 0;
}

static int shell_report(IdmError *err) {
    IdmValue reason;
    if (idm_error_take_reason(err, &reason)) {
        if (reason.tag == IDM_VAL_INT) return masked_status(reason.as.i);
        if (idm_is_tuple(reason) && idm_sequence_count(reason) == 2 && atom_is(idm_sequence_item(reason, 0, NULL), "error")) {
            IdmValue detail = idm_sequence_item(reason, 1, NULL);
            if (idm_is_tuple(detail) && idm_sequence_count(detail) == 3 && atom_is(idm_sequence_item(detail, 0, NULL), "command") &&
                idm_sequence_item(detail, 1, NULL).tag == IDM_VAL_STRING) {
                const char *name = idm_string_bytes(idm_sequence_item(detail, 1, NULL));
                IdmValue status = idm_sequence_item(detail, 2, NULL);
                if (status.tag == IDM_VAL_INT) {
                    fprintf(stderr, "ish: %s: exit status %lld\n", name, (long long)status.as.i);
                    int code = masked_status(status.as.i);
                    return code == 0 ? 1 : code;
                }
                if (atom_is(status, "capture-overflow")) {
                    fprintf(stderr, "ish: %s: capture overflow\n", name);
                    return 1;
                }
            }
        }
    }
    idm_error_fprint(stderr, err);
    return 1;
}

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
    wrapped = idm_syn_program_prepend_implement(program, "std/shell", file);
    if (!wrapped) {
        idm_error_oom(&err, idm_span_unknown(file));
        goto done;
    }
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
    if (err.present) status = shell_report(&err);
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
    if (argc == 1 && !isatty(0)) {
        IdmError err;
        idm_error_init(&err);
        char *source = NULL;
        size_t len = 0;
        if (!idm_read_stream(stdin, "<stdin>", &source, &len, &err)) {
            idm_error_fprint(stderr, &err);
            idm_error_clear(&err);
            return 1;
        }
        int status = run_shell_source("<stdin>", source, false);
        free(source);
        return status;
    }
    usage(stderr);
    return 64;
}
