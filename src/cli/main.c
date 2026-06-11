#include "ish/actor.h"
#include "ish/common.h"
#include "ish/expand.h"
#include "ish/reader.h"
#include "ish/vm.h"

#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
    fputs("usage:\n", out);
    fputs("  ish <file|->            run a script (\"-\" reads stdin)\n", out);
    fputs("  ish --eval <source>     run source text and print its value\n", out);
    fputs("  ish --dump-reader <file|->\n", out);
    fputs("  ish --dump-core <file|->\n", out);
    fputs("  ish --dump-bytecode <file|->\n", out);
}

static bool read_input_arg(const char *arg, char **source, IshError *err) {
    size_t len = 0;
    if (strcmp(arg, "-") == 0) return ish_read_stream(stdin, "<stdin>", source, &len, err);
    return ish_read_file(arg, source, &len, err);
}

static int dump_reader(const char *path) {
    IshError err;
    ish_error_init(&err);
    char *source = NULL;
    if (!read_input_arg(path, &source, &err)) {
        ish_error_fprint(stderr, &err);
        ish_error_clear(&err);
        return 1;
    }
    IshSyntax *program = NULL;
    const char *file = strcmp(path, "-") == 0 ? "<stdin>" : path;
    if (!ish_reader_read_string(file, source, &program, &err)) {
        ish_error_fprint(stderr, &err);
        ish_error_clear(&err);
        free(source);
        return 1;
    }
    IshBuffer buf;
    ish_buf_init(&buf);
    if (!ish_syn_dump(&buf, program) || !ish_buf_append_char(&buf, '\n')) {
        fputs("ishc: out of memory while dumping reader syntax\n", stderr);
        ish_buf_destroy(&buf);
        ish_syn_free(program);
        free(source);
        return 1;
    }
    fputs(buf.data, stdout);
    ish_buf_destroy(&buf);
    ish_syn_free(program);
    free(source);
    return 0;
}

static bool expand_input(const char *path, IshRuntime *rt, IshCore **out, IshError *err) {
    char *source = NULL;
    if (!read_input_arg(path, &source, err)) return false;
    const char *file = strcmp(path, "-") == 0 ? "<stdin>" : path;
    bool ok = ish_expand_string(rt, file, source, out, err);
    free(source);
    return ok;
}

static int dump_core(const char *path) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    if (!expand_input(path, &rt, &core, &err)) {
        ish_error_fprint(stderr, &err);
        ish_error_clear(&err);
        ish_runtime_destroy(&rt);
        return 1;
    }
    IshBuffer buf;
    ish_buf_init(&buf);
    if (!ish_core_dump(&buf, core) || !ish_buf_append_char(&buf, '\n')) {
        fputs("ishc: out of memory while dumping core\n", stderr);
        ish_buf_destroy(&buf);
        ish_core_free(core);
        ish_runtime_destroy(&rt);
        return 1;
    }
    fputs(buf.data, stdout);
    ish_buf_destroy(&buf);
    ish_core_free(core);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
    return 0;
}

static bool compile_input(const char *path, IshRuntime *rt, IshBytecodeModule *module, uint32_t *main_fn, IshError *err) {
    IshCore *core = NULL;
    if (!expand_input(path, rt, &core, err)) return false;
    bool ok = ish_core_compile_main(core, module, main_fn, err);
    ish_core_free(core);
    return ok;
}

static int dump_bytecode(const char *path) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    if (!compile_input(path, &rt, &module, &main_fn, &err)) {
        (void)main_fn;
        ish_error_fprint(stderr, &err);
        ish_bc_destroy(&module);
        ish_error_clear(&err);
        ish_runtime_destroy(&rt);
        return 1;
    }
    IshBuffer buf;
    ish_buf_init(&buf);
    if (!ish_bc_disassemble(&buf, &module)) {
        fputs("ishc: out of memory while dumping bytecode\n", stderr);
        ish_buf_destroy(&buf);
        ish_bc_destroy(&module);
        ish_runtime_destroy(&rt);
        return 1;
    }
    fputs(buf.data, stdout);
    ish_buf_destroy(&buf);
    ish_bc_destroy(&module);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
    return 0;
}

static int run_source(const char *file, const char *source, bool print_result) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    int status = 1;
    IshCore *core = NULL;
    IshBytecodeModule module;
    ish_bc_init(&module);
    IshScheduler *sched = NULL;
    if (!ish_expand_string(&rt, file, source, &core, &err)) goto done;
    uint32_t main_fn = 0;
    if (!ish_core_compile_main(core, &module, &main_fn, &err)) goto done;
    sched = ish_sched_create(&rt, &module, &err);
    if (!sched) goto done;
    IshValue out = ish_nil();
    if (!ish_sched_run_main(sched, main_fn, &out, &err)) goto done;
    if (print_result) {
        IshBuffer buf;
        ish_buf_init(&buf);
        if (!ish_value_write(&buf, out) || !ish_buf_append_char(&buf, '\n')) {
            ish_buf_destroy(&buf);
            ish_error_set(&err, ish_span_unknown(NULL), "out of memory while printing result");
            goto done;
        }
        fputs(buf.data, stdout);
        ish_buf_destroy(&buf);
    }
    status = 0;
done:
    if (err.present) ish_error_fprint(stderr, &err);
    ish_error_clear(&err);
    ish_sched_destroy(sched);
    ish_core_free(core);
    ish_bc_destroy(&module);
    ish_runtime_destroy(&rt);
    return status;
}

static int run_file(const char *path) {
    IshError err;
    ish_error_init(&err);
    char *source = NULL;
    if (!read_input_arg(path, &source, &err)) {
        ish_error_fprint(stderr, &err);
        ish_error_clear(&err);
        return 1;
    }
    const char *file = strcmp(path, "-") == 0 ? "<stdin>" : path;
    int status = run_source(file, source, false);
    free(source);
    return status;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--eval") == 0) return run_source("<eval>", argv[2], true);
    if (argc == 3 && strcmp(argv[1], "--dump-reader") == 0) return dump_reader(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--dump-core") == 0) return dump_core(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--dump-bytecode") == 0) return dump_bytecode(argv[2]);
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "--dump-surface") == 0) {
        const char *prelude = argc == 3 ? argv[2] : "";
        char source[4096];
        snprintf(source, sizeof(source),
                 "%s\n"
                 "for-syntax do\n"
                 "  println (tuple :operators (expander-surface :operators))\n"
                 "  println (tuple :macros (expander-surface :macros))\n"
                 "  println (tuple :protocols (expander-surface :protocols))\n"
                 "  println (tuple :resolvers (expander-surface :resolvers))\n"
                 "  println (tuple :methods (expander-surface :methods))\n"
                 "  println (tuple :active (expander-surface :active))\n"
                 "end\n"
                 ":ok\n", prelude);
        return run_source("<dump-surface>", source, false);
    }
    if (argc == 2 && argv[1][0] != '-') return run_file(argv[1]);
    if (argc == 2 && strcmp(argv[1], "-") == 0) return run_file("-");
    usage(stderr);
    return 64;
}
