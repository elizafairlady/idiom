#include "idiom/actor.h"
#include "idiom/artifact.h"
#include "idiom/common.h"
#include "idiom/expand.h"
#include "idiom/ports.h"
#include "idiom/reader.h"
#include "idiom/vm.h"
#include "idm_build_id.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *out) {
    fputs("usage:\n", out);
    fputs("  idiomc [-json] COMMAND                     -json: diagnostics as JSON lines on stderr\n", out);
    fputs("  idiomc run <file|package|-> [--] [args...]\n", out);
    fputs("  idiomc <file|package|-> [args...]          run shorthand\n", out);
    fputs("  idiomc eval <source|-> [--] [args...]      evaluate source and print its value\n", out);
    fputs("  idiomc build <file|package|-> [-o OUT]\n", out);
    fputs("  idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-benchtime MS|Nx] [-json] [path...]\n", out);
    fputs("  idiomc repl\n", out);
    fputs("  idiomc dump reader|core|bytecode <file|package|->\n", out);
    fputs("  idiomc dump surface [prelude]\n", out);
    fputs("  idiomc explain <file|package>[:LINE[:COL]]  why each form means what it means\n", out);
    fputs("  idiomc version\n", out);
    fputs("  idiomc help\n", out);
}

static bool g_diag_json = false;

static bool diag_json_span(IdmBuffer *b, IdmSpan span) {
    bool ok = true;
    if (span.file) {
        ok = idm_buf_append(b, ",\"file\":") &&
             idm_buf_append_json_string(b, span.file, strlen(span.file));
    }
    if (span.line != 0) {
        ok = ok && idm_buf_appendf(b, ",\"line\":%u,\"column\":%u", span.line, span.column);
    }
    if (span.end > span.start) {
        ok = ok && idm_buf_appendf(b, ",\"start\":%zu,\"end\":%zu", span.start, span.end);
    }
    return ok;
}

static void cli_error_print(const char *stage, const IdmError *err) {
    if (!err || !err->present) return;
    if (!g_diag_json) {
        idm_error_fprint(stderr, err);
        return;
    }
    IdmBuffer b;
    idm_buf_init(&b);
    const char *code = stage;
    IdmValue reason = idm_nil();
    bool has_reason = false;
    if (err->reason) {
        reason = *(const IdmValue *)err->reason;
        has_reason = true;
        IdmValue detail = reason;
        IdmError ignore;
        idm_error_init(&ignore);
        if (idm_value_is_error(detail) && idm_sequence_count(detail) == 2u) {
            detail = idm_sequence_item(detail, 1, &ignore);
        }
        if (idm_value_tag(detail) == IDM_VAL_TUPLE && idm_sequence_count(detail) != 0) {
            IdmValue head = idm_sequence_item(detail, 0, &ignore);
            if (idm_value_tag(head) == IDM_VAL_ATOM) code = idm_symbol_text(idm_value_symbol(head));
        }
        idm_error_clear(&ignore);
    }
    bool ok = idm_buf_append(&b, "{\"action\":\"diagnostic\",\"stage\":") &&
              idm_buf_append_json_string(&b, stage, strlen(stage)) &&
              idm_buf_append(&b, ",\"code\":") &&
              idm_buf_append_json_string(&b, code, strlen(code)) &&
              idm_buf_append(&b, ",\"message\":") &&
              idm_buf_append_json_string(&b, err->message ? err->message : "error", strlen(err->message ? err->message : "error")) &&
              diag_json_span(&b, err->span) &&
              idm_buf_append(&b, ",\"notes\":[");
    for (size_t i = 0; ok && i < err->note_count; i++) {
        const IdmErrorNote *note = &err->notes[i];
        const char *message = note->message ? note->message : "";
        ok = (i == 0 || idm_buf_append_char(&b, ',')) &&
             idm_buf_append(&b, "{\"message\":") &&
             idm_buf_append_json_string(&b, message, strlen(message)) &&
             diag_json_span(&b, note->span) &&
             idm_buf_append_char(&b, '}');
    }
    ok = ok && idm_buf_append_char(&b, ']');
    if (ok && has_reason) {
        ok = idm_buf_append(&b, ",\"reason\":") && idm_value_write_json(&b, reason);
    }
    ok = ok && idm_buf_append(&b, "}\n");
    if (ok && b.data) {
        fputs(b.data, stderr);
    } else {
        idm_error_fprint(stderr, err);
    }
    idm_buf_destroy(&b);
}

static bool read_source_arg(const char *arg, IdmRuntime *rt, char **source, const char **label, IdmError *err) {
    *source = NULL;
    *label = strcmp(arg, "-") == 0 ? "<stdin>" : arg;
    if (strcmp(arg, "-") == 0) {
        size_t len = 0;
        return idm_read_stream(stdin, "<stdin>", source, &len, err);
    }
    struct stat st;
    bool is_path = stat(arg, &st) == 0;
    if (!is_path || S_ISDIR(st.st_mode)) {
        IdmBuffer src;
        idm_buf_init(&src);
        const char *pkg_label = NULL;
        bool ok = idm_package_read_source(rt, arg, &src, &pkg_label, NULL, idm_span_unknown(arg), err);
        if (!ok) {
            idm_buf_destroy(&src);
            return false;
        }
        *source = idm_buf_take(&src);
        if (!*source) return idm_error_oom(err, idm_span_unknown(arg));
        *label = pkg_label ? pkg_label : arg;
        return true;
    }
    size_t len = 0;
    return idm_read_file(arg, source, &len, err);
}

static int dump_reader(const char *path) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    const char *file = NULL;
    if (!read_source_arg(path, &rt, &source, &file, &err)) {
        cli_error_print("read", &err);
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmSyntax *program = NULL;
    if (!idm_expand_read_source_string(&rt, file, source, &program, &err)) {
        cli_error_print("read", &err);
        idm_error_clear(&err);
        free(source);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_syn_dump_pretty(&buf, program) || !idm_buf_append_char(&buf, '\n')) {
        fputs("idiomc: out of memory while dumping reader syntax\n", stderr);
        idm_buf_destroy(&buf);
        idm_syn_free(program);
        free(source);
        idm_runtime_destroy(&rt);
        return 1;
    }
    fputs(buf.data, stdout);
    idm_buf_destroy(&buf);
    idm_syn_free(program);
    free(source);
    idm_runtime_destroy(&rt);
    return 0;
}

static bool expand_input(const char *path, IdmRuntime *rt, IdmCore **out, IdmError *err) {
    char *source = NULL;
    const char *file = NULL;
    if (!read_source_arg(path, rt, &source, &file, err)) return false;
    bool ok = idm_expand_source_string(rt, file, source, out, err);
    free(source);
    return ok;
}

static int dump_core(const char *path) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    if (!expand_input(path, &rt, &core, &err)) {
        cli_error_print("expand", &err);
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_core_dump_pretty(&buf, core) || !idm_buf_append_char(&buf, '\n')) {
        fputs("idiomc: out of memory while dumping core\n", stderr);
        idm_buf_destroy(&buf);
        idm_core_free(core);
        idm_runtime_destroy(&rt);
        return 1;
    }
    fputs(buf.data, stdout);
    idm_buf_destroy(&buf);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return 0;
}

static bool compile_input(const char *path, IdmRuntime *rt, IdmBytecodeModule *module, uint32_t *main_fn, IdmError *err) {
    IdmCore *core = NULL;
    if (!expand_input(path, rt, &core, err)) return false;
    bool ok = idm_core_compile_main(rt, core, module, main_fn, err);
    idm_core_free(core);
    return ok;
}

static int dump_bytecode(const char *path) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    if (!compile_input(path, &rt, &module, &main_fn, &err)) {
        (void)main_fn;
        cli_error_print("compile", &err);
        idm_bc_destroy(&module);
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return 1;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_bc_disassemble(&buf, &module)) {
        fputs("idiomc: out of memory while dumping bytecode\n", stderr);
        idm_buf_destroy(&buf);
        idm_bc_destroy(&module);
        idm_runtime_destroy(&rt);
        return 1;
    }
    fputs(buf.data, stdout);
    idm_buf_destroy(&buf);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return 0;
}

static char **g_cli_args = NULL;
static size_t g_cli_arg_count = 0;

static int run_sealed(const char *file, const unsigned char *data, size_t len);

typedef struct {
    uint64_t expand_ns;
    uint64_t compile_ns;
    uint64_t intern_ns;
    uint64_t run_ns;
} RunTimings;

static bool tool_result_load(IdmRuntime *rt, const char *kind, const unsigned char key[32], IdmExpandTrace *trace, char **out, size_t *out_len, char **events, size_t *events_len);
static void tool_result_store(const char *kind, const unsigned char key[32], const IdmExpandTrace *trace, const RunTimings *timing, const char *out, size_t out_len, const char *events, size_t events_len);

static uint64_t run_timing_total(const RunTimings *timing) {
    return timing->expand_ns + timing->compile_ns + timing->intern_ns + timing->run_ns;
}

static uint64_t ns_since(uint64_t start) {
    uint64_t end = idm_profile_now_ns();
    return end >= start ? end - start : 0u;
}

static void set_cli_args(int argc, char **argv, int first) {
    if (first < argc && strcmp(argv[first], "--") == 0) first++;
    if (first < argc) {
        g_cli_args = argv + first;
        g_cli_arg_count = (size_t)(argc - first);
    } else {
        g_cli_args = NULL;
        g_cli_arg_count = 0;
    }
}

static bool compile_cache_key(const char *file, const char *source, unsigned char out[32]) {
    IdmBuffer input;
    idm_buf_init(&input);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    const char *root = getenv("IDIOMROOT");
    const char *path = getenv("IDIOMPATH");
    bool ok = idm_buf_append(&input, "IDM-COMPILE-v1" IDM_BUILD_ID) &&
              idm_buf_put_str(&input, file ? file : "<program>", strlen(file ? file : "<program>")) &&
              idm_buf_put_str(&input, source ? source : "", strlen(source ? source : "")) &&
              idm_buf_put_str(&input, cwd, strlen(cwd)) &&
              idm_buf_put_str(&input, root ? root : "", root ? strlen(root) : 0u) &&
              idm_buf_put_str(&input, path ? path : "", path ? strlen(path) : 0u);
    if (ok) idm_sha256(input.data ? input.data : "", input.len, out);
    idm_buf_destroy(&input);
    return ok;
}

static bool compile_cache_load(IdmRuntime *rt, const unsigned char key[32], IdmExpandTrace *trace, IdmBytecodeModule *module, uint32_t *main_fn) {
    char *payload = NULL;
    size_t payload_len = 0;
    char *unused = NULL;
    size_t unused_len = 0;
    if (!tool_result_load(rt, "compile", key, trace, &payload, &payload_len, &unused, &unused_len)) return false;
    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)payload, payload_len);
    uint32_t cached_main = idm_rd_u32(&r);
    IdmError err;
    idm_error_init(&err);
    bool ok = r.ok && idm_ic_deserialize(rt, r.data + r.pos, r.len - r.pos, module, &err) && cached_main < module->function_count;
    idm_error_clear(&err);
    free(payload);
    free(unused);
    if (!ok) {
        idm_bc_destroy(module);
        idm_bc_init(module);
        idm_expand_trace_destroy(trace);
        return false;
    }
    *main_fn = cached_main;
    return true;
}

static void compile_cache_store(const unsigned char key[32], const IdmExpandTrace *trace, const IdmBytecodeModule *module, uint32_t main_fn) {
    IdmBuffer payload;
    idm_buf_init(&payload);
    IdmError err;
    idm_error_init(&err);
    bool ok = idm_buf_put_u32(&payload, main_fn) && idm_ic_serialize(module, &payload, &err);
    if (ok) tool_result_store("compile", key, trace, NULL, payload.data, payload.len, "", 0u);
    idm_error_clear(&err);
    idm_buf_destroy(&payload);
}

static int run_source_traced_timed(const char *file, const char *source, bool print_result, RunTimings *timing, IdmExpandTrace *trace) {
    if (timing) memset(timing, 0, sizeof(*timing));
    IdmRuntime rt;
    idm_runtime_init(&rt);
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        rt.interactive = true;
        idm_job_control_init();
    }
    rt.cli_args = g_cli_args;
    rt.cli_arg_count = g_cli_arg_count;
    IdmError err;
    idm_error_init(&err);
    int status = 1;
    IdmCore *core = NULL;
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmScheduler *sched = NULL;
    IdmExpandTrace local_trace;
    memset(&local_trace, 0, sizeof(local_trace));
    IdmExpandTrace *compile_trace = trace ? trace : &local_trace;
    IdmPhaseReads reads;
    memset(&reads, 0, sizeof(reads));
    IdmPhaseReads *old_reads = rt.phase_reads;
    if (trace) rt.phase_reads = &reads;
    const char *stage = "expand";
    unsigned char cache_key[32];
    bool have_cache_key = compile_cache_key(file, source, cache_key);
    uint32_t main_fn = 0;
    if (have_cache_key && compile_cache_load(&rt, cache_key, compile_trace, &module, &main_fn)) goto run_module;
    uint64_t start = idm_profile_now_ns();
    bool expanded = idm_expand_source_string_traced(&rt, file, source, &core, compile_trace, &err);
    if (!expanded) {
        if (timing) timing->expand_ns = ns_since(start);
        goto done;
    }
    if (timing) timing->expand_ns = ns_since(start);
    stage = "compile";
    start = idm_profile_now_ns();
    if (!idm_core_compile_main(&rt, core, &module, &main_fn, &err)) {
        if (timing) timing->compile_ns = ns_since(start);
        goto done;
    }
    if (timing) timing->compile_ns = ns_since(start);
    start = idm_profile_now_ns();
    if (!idm_bc_intern_literals(&rt, &module, &err)) {
        if (timing) timing->intern_ns = ns_since(start);
        goto done;
    }
    if (timing) timing->intern_ns = ns_since(start);
    if (have_cache_key && compile_trace->cacheable) compile_cache_store(cache_key, compile_trace, &module, main_fn);
run_module:
    stage = "run";
    start = idm_profile_now_ns();
    sched = idm_sched_create(&rt, &module, &err);
    if (!sched) {
        if (timing) timing->run_ns = ns_since(start);
        goto done;
    }
    IdmValue out = idm_nil();
    bool interrupted = false;
    if (!idm_sched_run_main(sched, main_fn, &out, &interrupted, &err)) {
        if (timing) timing->run_ns = ns_since(start);
        goto done;
    }
    if (interrupted) {
        if (timing) timing->run_ns = ns_since(start);
        status = 130;
        goto done;
    }
    if (print_result) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        if (!idm_value_write(&buf, out) || !idm_buf_append_char(&buf, '\n')) {
            idm_buf_destroy(&buf);
            idm_error_set(&err, idm_span_unknown(NULL), "out of memory while printing result");
            if (timing) timing->run_ns = ns_since(start);
            goto done;
        }
        fputs(buf.data, stdout);
        idm_buf_destroy(&buf);
    }
    if (timing) timing->run_ns = ns_since(start);
    status = 0;
done:
    if (trace) {
        rt.phase_reads = old_reads;
        if (!idm_expand_trace_add_reads(trace, &reads, &err)) status = 1;
    }
    idm_phase_reads_destroy(&reads);
    if (err.present) cli_error_print(stage, &err);
    idm_error_clear(&err);
    idm_sched_destroy(sched);
    idm_core_free(core);
    idm_bc_destroy(&module);
    if (!trace) idm_expand_trace_destroy(&local_trace);
    idm_runtime_destroy(&rt);
    return status;
}

static int run_source_timed(const char *file, const char *source, bool print_result, RunTimings *timing) {
    return run_source_traced_timed(file, source, print_result, timing, NULL);
}

static int run_source(const char *file, const char *source, bool print_result) {
    return run_source_timed(file, source, print_result, NULL);
}

static bool source_arg_package_like(const char *arg);
static bool build_package_entry_source(const char *package_path, char **out_source, IdmError *err);

static int run_source_arg(const char *path) {
    IdmRuntime loader;
    idm_runtime_init(&loader);
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    const char *label = NULL;
    int status = 1;
    if (source_arg_package_like(path)) {
        label = path;
        if (!build_package_entry_source(path, &source, &err)) {
            cli_error_print("read", &err);
            goto done;
        }
    } else if (!read_source_arg(path, &loader, &source, &label, &err)) {
        cli_error_print("read", &err);
        goto done;
    }
    char *owned_label = idm_strdup(label ? label : path);
    if (!owned_label) {
        idm_error_oom(&err, idm_span_unknown(path));
        cli_error_print("read", &err);
        goto done;
    }
    status = run_source(owned_label, source, false);
    free(owned_label);
done:
    idm_error_clear(&err);
    free(source);
    idm_runtime_destroy(&loader);
    return status;
}

static int run_file(const char *path) {
    if (strcmp(path, "-") == 0) return run_source_arg(path);
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) return run_source_arg(path);

    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    size_t len = 0;
    if (!idm_read_file(path, &source, &len, &err)) {
        cli_error_print("read", &err);
        idm_error_clear(&err);
        return 1;
    }
    const unsigned char *p = (const unsigned char *)source;
    size_t remaining = len;
    if (remaining >= 2 && p[0] == '#' && p[1] == '!') {
        while (remaining > 0 && *p != '\n') { p++; remaining--; }
        if (remaining > 0) { p++; remaining--; }
    }
    if (remaining >= 12 && memcmp(p, IDM_WIRE_MAGIC, 4u) == 0) {
        int status = run_sealed(path, p, remaining);
        free(source);
        return status;
    }
    int status = run_source(path, source, false);
    free(source);
    idm_error_clear(&err);
    return status;
}

typedef struct {
    const char *run;
    const char *bench;
    const char *count;
    const char *benchtime;
    bool json;
} TestOptions;

static bool test_filter_match(const char *text, const char *pattern) {
    if (!pattern || pattern[0] == '\0' || strcmp(pattern, ".") == 0) return true;
    return strstr(text, pattern) != NULL;
}

static bool positive_decimal(const char *s) {
    if (!s || s[0] == '\0') return false;
    bool nonzero = false;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        if (*p != '0') nonzero = true;
    }
    return nonzero;
}

static int run_source_capture(const char *file, const char *source, bool print_result, char **out, size_t *out_len, RunTimings *timing, IdmExpandTrace *trace) {
    *out = NULL;
    *out_len = 0;
    FILE *tmp = tmpfile();
    if (!tmp) {
        fprintf(stderr, "idiomc test: cannot create stdout capture\n");
        return 1;
    }
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) {
        fclose(tmp);
        fprintf(stderr, "idiomc test: cannot capture stdout\n");
        return 1;
    }
    if (dup2(fileno(tmp), STDOUT_FILENO) < 0) {
        close(saved);
        fclose(tmp);
        fprintf(stderr, "idiomc test: cannot redirect stdout\n");
        return 1;
    }
    int status = run_source_traced_timed(file, source, print_result, timing, trace);
    fflush(stdout);
    if (dup2(saved, STDOUT_FILENO) < 0) {
        close(saved);
        fclose(tmp);
        fprintf(stderr, "idiomc test: cannot restore stdout\n");
        return 1;
    }
    close(saved);
    rewind(tmp);
    IdmError err;
    idm_error_init(&err);
    if (!idm_read_stream(tmp, file, out, out_len, &err)) {
        cli_error_print("read", &err);
        idm_error_clear(&err);
        fclose(tmp);
        return 1;
    }
    idm_error_clear(&err);
    fclose(tmp);
    return status;
}

static void json_string(FILE *out, const char *s) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (idm_buf_append_json_string(&buf, s ? s : "", s ? strlen(s) : 0u) && buf.data) {
        fputs(buf.data, out);
    } else {
        fputs("\"\"", out);
    }
    idm_buf_destroy(&buf);
}

static void json_file_event_cached(const char *action, const char *file, bool cached) {
    fputs("{\"action\":", stdout);
    json_string(stdout, action);
    fputs(",\"file\":", stdout);
    json_string(stdout, file);
    if (cached) fputs(",\"cached\":true", stdout);
    fputs("}\n", stdout);
}

static void json_file_event(const char *action, const char *file) {
    json_file_event_cached(action, file, false);
}

static void json_test_events(const char *action, const char *file, const char *events_path, long *offset) {
    FILE *events = fopen(events_path, "r");
    if (!events) return;
    if (fseek(events, *offset, SEEK_SET) != 0) {
        fclose(events);
        return;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, events)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] != '{') continue;
        fprintf(stdout, "{\"action\":\"%s\",\"file\":", action);
        json_string(stdout, file);
        fputs(",\"data\":", stdout);
        fputs(line, stdout);
        fputs("}\n", stdout);
    }
    *offset = ftell(events);
    free(line);
    fclose(events);
}

static void json_test_events_data(const char *action, const char *file, const char *data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && data[end] != '\n' && data[end] != '\r') end++;
        if (end > pos && data[pos] == '{') {
            fprintf(stdout, "{\"action\":\"%s\",\"file\":", action);
            json_string(stdout, file);
            fputs(",\"data\":", stdout);
            fwrite(data + pos, 1u, end - pos, stdout);
            fputs("}\n", stdout);
        }
        while (end < len && (data[end] == '\n' || data[end] == '\r')) end++;
        pos = end;
    }
}

static void json_output_event(const char *file, const char *output) {
    if (!output || output[0] == '\0') return;
    fputs("{\"action\":\"output\",\"file\":", stdout);
    json_string(stdout, file);
    fputs(",\"output\":", stdout);
    json_string(stdout, output);
    fputs("}\n", stdout);
}

static void json_bench_file_event(const char *file, const RunTimings *timing) {
    fprintf(stdout,
            "{\"action\":\"benchfile\",\"file\":");
    json_string(stdout, file);
    fprintf(stdout,
            ",\"expand_ns\":%" PRIu64 ",\"compile_ns\":%" PRIu64 ",\"intern_ns\":%" PRIu64 ",\"run_ns\":%" PRIu64 ",\"total_ns\":%" PRIu64 "}\n",
            timing->expand_ns,
            timing->compile_ns,
            timing->intern_ns,
            timing->run_ns,
            run_timing_total(timing));
}

static void plain_bench_file_event(const char *file, const RunTimings *timing) {
    printf("benchfile file=%s expand_ns=%" PRIu64 " compile_ns=%" PRIu64 " intern_ns=%" PRIu64 " run_ns=%" PRIu64 " total_ns=%" PRIu64 "\n",
           file,
           timing->expand_ns,
           timing->compile_ns,
           timing->intern_ns,
           timing->run_ns,
           run_timing_total(timing));
}

static size_t emit_bench_output(const char *file, char *output, bool json) {
    size_t count = 0;
    char *cursor = output;
    while (cursor && *cursor) {
        char *line = cursor;
        char *nl = strchr(cursor, '\n');
        if (nl) {
            *nl = '\0';
            cursor = nl + 1;
        } else {
            cursor = NULL;
        }
        if (strncmp(line, "bench ", 6u) == 0) {
            count++;
            if (!json) {
                fputs(line, stdout);
                fputc('\n', stdout);
            }
        } else if (line[0] != '\0') {
            if (json) json_output_event(file, line);
            else {
                fputs(line, stdout);
                fputc('\n', stdout);
            }
        }
    }
    return count;
}

static int collect_test_files(const char *dir_path, char ***out_files, size_t *out_count, size_t *out_cap) {
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 3, ".id") != 0) continue;
        if (*out_count == *out_cap) {
            if (!idm_grow((void **)out_files, out_cap, sizeof(**out_files), 16u, *out_count + 1u)) { closedir(dir); return -1; }
        }
        size_t full_len = strlen(dir_path) + 1u + len + 1u;
        char *full = malloc(full_len);
        if (!full) { closedir(dir); return -1; }
        snprintf(full, full_len, "%s/%s", dir_path, entry->d_name);
        (*out_files)[(*out_count)++] = full;
    }
    closedir(dir);
    return 0;
}

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool test_add_path(char ***paths, size_t *count, size_t *cap, char *path) {
    if (*count == *cap && !idm_grow((void **)paths, cap, sizeof(**paths), 8u, *count + 1u)) return false;
    (*paths)[(*count)++] = path;
    return true;
}

static int test_collect_path(const char *path, char ***files, size_t *count, size_t *cap, const char *file_filter) {
    size_t before = *count;
    if (collect_test_files(path, files, count, cap) == 0) {
        size_t write = before;
        for (size_t i = before; i < *count; i++) {
            if (test_filter_match((*files)[i], file_filter)) {
                (*files)[write++] = (*files)[i];
            } else {
                free((*files)[i]);
            }
        }
        *count = write;
        return 0;
    }
    if (!test_filter_match(path, file_filter)) return 0;
    char *copy = idm_strdup(path);
    if (!copy) return -1;
    if (!test_add_path(files, count, cap, copy)) {
        free(copy);
        return -1;
    }
    return 0;
}

typedef struct {
    pid_t pid;
    int status;
    RunTimings timing;
    bool cached;
    char *cached_output;
    size_t cached_output_len;
    char *cached_events;
    size_t cached_events_len;
    char output_path[512];
    char timing_path[512];
    char events_path[512];
} TestJob;

typedef struct {
    int status;
    RunTimings timing;
} TestJobResult;

static bool test_write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len != 0u) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return false;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool test_cache_key(const char *file, const char *source, size_t source_len, bool json, unsigned char out[32]) {
    IdmBuffer input;
    idm_buf_init(&input);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    const char *root = getenv("IDIOMROOT");
    const char *path = getenv("IDIOMPATH");
    const char *run = getenv("IDIOM_TEST_RUN");
    bool ok = idm_buf_append(&input, "IDM-TEST-CANDIDATE-v1" IDM_BUILD_ID) &&
              idm_buf_put_str(&input, file, strlen(file)) &&
              idm_buf_put_u64(&input, (uint64_t)source_len) &&
              idm_buf_append_n(&input, source, source_len) &&
              idm_buf_put_str(&input, cwd, strlen(cwd)) &&
              idm_buf_put_str(&input, root ? root : "", root ? strlen(root) : 0u) &&
              idm_buf_put_str(&input, path ? path : "", path ? strlen(path) : 0u) &&
              idm_buf_put_str(&input, run ? run : "", run ? strlen(run) : 0u) &&
              idm_buf_put_u8(&input, json ? 1u : 0u);
    if (ok) idm_sha256(input.data ? input.data : "", input.len, out);
    idm_buf_destroy(&input);
    return ok;
}

static bool test_cache_read_bytes(IdmByteReader *r, size_t len, const unsigned char **out) {
    if (!r->ok || len > r->len - r->pos) {
        r->ok = false;
        return false;
    }
    *out = r->data + r->pos;
    r->pos += len;
    return true;
}

static bool tool_result_load(IdmRuntime *rt, const char *kind, const unsigned char key[32], IdmExpandTrace *trace, char **out, size_t *out_len, char **events, size_t *events_len) {
    *out = NULL;
    *out_len = 0;
    *events = NULL;
    *events_len = 0;
    char *wire = NULL;
    size_t wire_len = 0;
    if (!idm_tool_cache_load(kind, key, &wire, &wire_len)) return false;
    static const char magic[] = "IDM-TOOL-RESULT-v2";
    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)wire, wire_len);
    const unsigned char *bytes = NULL;
    bool ok = test_cache_read_bytes(&r, sizeof(magic) - 1u, &bytes) &&
              memcmp(bytes, magic, sizeof(magic) - 1u) == 0 &&
              test_cache_read_bytes(&r, 32u, &bytes);
    unsigned char source_hash[32];
    if (ok) memcpy(source_hash, bytes, 32u);
    ok = ok && test_cache_read_bytes(&r, 32u, &bytes);
    unsigned char action_hash[32];
    if (ok) memcpy(action_hash, bytes, 32u);
    uint32_t dep_count = ok ? idm_rd_u32(&r) : 0u;
    if (!r.ok || dep_count > 1048576u) ok = false;
    IdmError err;
    idm_error_init(&err);
    for (uint32_t i = 0; ok && i < dep_count; i++) {
        IdmArtifactDep dep;
        memset(&dep, 0, sizeof(dep));
        dep.kind = idm_rd_u8(&r);
        dep.path = idm_rd_string(&r, NULL);
        ok = r.ok && dep.path && test_cache_read_bytes(&r, 32u, &bytes);
        if (ok) memcpy(dep.hash, bytes, 32u);
        if (ok && dep.kind == IDM_DEP_PACKAGE) {
            unsigned char current[32];
            ok = idm_expand_package_action_hash(rt, dep.path, current, &err) && memcmp(current, dep.hash, 32u) == 0;
        } else if (ok) {
            ok = dep.kind <= IDM_DEP_ENV && idm_artifact_dep_verified(rt, &dep);
        }
        if (ok && trace) {
            IdmArtifactDep *next = realloc(trace->deps, (trace->dep_count + 1u) * sizeof(*next));
            if (!next) {
                ok = false;
            } else {
                trace->deps = next;
                IdmArtifactDep *copy = &trace->deps[trace->dep_count];
                memset(copy, 0, sizeof(*copy));
                copy->path = idm_strdup(dep.path);
                if (!copy->path) ok = false;
                else {
                    memcpy(copy->hash, dep.hash, 32u);
                    copy->kind = dep.kind;
                    trace->dep_count++;
                }
            }
        }
        free(dep.path);
    }
    for (size_t i = 0; ok && i < 4u; i++) (void)idm_rd_u64(&r);
    if (ok) {
        *out = idm_rd_string(&r, out_len);
        *events = idm_rd_string(&r, events_len);
        ok = r.ok && *out && *events && r.pos == r.len;
    }
    idm_error_clear(&err);
    free(wire);
    if (!ok) {
        idm_expand_trace_destroy(trace);
        free(*out);
        free(*events);
        *out = NULL;
        *events = NULL;
        *out_len = 0;
        *events_len = 0;
    } else if (trace) {
        memcpy(trace->source_hash, source_hash, 32u);
        memcpy(trace->action_hash, action_hash, 32u);
        trace->cacheable = true;
    }
    return ok;
}

static void tool_result_store(const char *kind, const unsigned char key[32], const IdmExpandTrace *trace, const RunTimings *timing, const char *out, size_t out_len, const char *events, size_t events_len) {
    static const char magic[] = "IDM-TOOL-RESULT-v2";
    if (trace->dep_count > UINT32_MAX) return;
    IdmBuffer wire;
    idm_buf_init(&wire);
    bool ok = idm_buf_append_n(&wire, magic, sizeof(magic) - 1u) &&
              idm_buf_append_n(&wire, (const char *)trace->source_hash, 32u) &&
              idm_buf_append_n(&wire, (const char *)trace->action_hash, 32u) &&
              idm_buf_put_u32(&wire, (uint32_t)trace->dep_count);
    for (size_t i = 0; ok && i < trace->dep_count; i++) {
        const IdmArtifactDep *dep = &trace->deps[i];
        ok = idm_buf_put_u8(&wire, dep->kind) &&
             idm_buf_put_str(&wire, dep->path, strlen(dep->path)) &&
             idm_buf_append_n(&wire, (const char *)dep->hash, 32u);
    }
    if (ok) ok = idm_buf_put_u64(&wire, timing ? timing->expand_ns : 0u) &&
                 idm_buf_put_u64(&wire, timing ? timing->compile_ns : 0u) &&
                 idm_buf_put_u64(&wire, timing ? timing->intern_ns : 0u) &&
                 idm_buf_put_u64(&wire, timing ? timing->run_ns : 0u) &&
                 idm_buf_put_str(&wire, out ? out : "", out_len) &&
                 idm_buf_put_str(&wire, events ? events : "", events_len);
    if (ok) (void)idm_tool_cache_store(kind, key, wire.data, wire.len);
    idm_buf_destroy(&wire);
}

static bool test_temp_file(char path[512], const char *tmpdir, const char *kind, int *out_fd) {
    int n = snprintf(path, 512u, "%s/idiom-test-%s.XXXXXX", tmpdir, kind);
    if (n <= 0 || n >= 512) return false;
    int fd = mkstemp(path);
    if (fd < 0) return false;
    *out_fd = fd;
    return true;
}

static bool test_job_start(TestJob *job, const char *file, bool json, bool use_cache, const char *tmpdir) {
    int output_fd = -1;
    int timing_fd = -1;
    int events_fd = -1;
    memset(job, 0, sizeof(*job));
    job->status = 1;
    if (!test_temp_file(job->output_path, tmpdir, "output", &output_fd) ||
        !test_temp_file(job->timing_path, tmpdir, "timing", &timing_fd) ||
        (json && !test_temp_file(job->events_path, tmpdir, "events", &events_fd))) {
        if (output_fd >= 0) close(output_fd);
        if (timing_fd >= 0) close(timing_fd);
        if (events_fd >= 0) close(events_fd);
        if (job->output_path[0]) unlink(job->output_path);
        if (job->timing_path[0]) unlink(job->timing_path);
        if (job->events_path[0]) unlink(job->events_path);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(output_fd);
        close(timing_fd);
        if (events_fd >= 0) close(events_fd);
        unlink(job->output_path);
        unlink(job->timing_path);
        if (job->events_path[0]) unlink(job->events_path);
        return false;
    }
    if (pid == 0) {
        if (events_fd >= 0) close(events_fd);
        if (json) (void)setenv("IDIOM_TEST_EVENTS", job->events_path, 1);
        else (void)unsetenv("IDIOM_TEST_EVENTS");
        IdmError err;
        idm_error_init(&err);
        char *source = NULL;
        char *output = NULL;
        size_t source_len = 0;
        size_t output_len = 0;
        TestJobResult result;
        memset(&result, 0, sizeof(result));
        result.status = 1;
        if (idm_read_file(file, &source, &source_len, &err)) {
            unsigned char cache_key[32];
            char *events = NULL;
            size_t events_len = 0;
            bool have_key = use_cache && test_cache_key(file, source, source_len, json, cache_key);
            IdmExpandTrace trace;
            memset(&trace, 0, sizeof(trace));
            result.status = run_source_capture(file, source, false, &output, &output_len, &result.timing, &trace);
            if (result.status == 0 && have_key) {
                if (json) {
                    IdmError events_err;
                    idm_error_init(&events_err);
                    if (!idm_read_file(job->events_path, &events, &events_len, &events_err)) {
                        idm_error_clear(&events_err);
                        free(events);
                        events = idm_strdup("");
                        events_len = 0;
                    } else {
                        idm_error_clear(&events_err);
                    }
                }
                if (trace.cacheable) tool_result_store("tests", cache_key, &trace, &result.timing, output, output_len, events, events_len);
            }
            idm_expand_trace_destroy(&trace);
            free(events);
        } else {
            cli_error_print("read", &err);
        }
        bool wrote = test_write_all(output_fd, output ? output : "", output_len) &&
                     test_write_all(timing_fd, &result, sizeof(result));
        close(output_fd);
        close(timing_fd);
        free(output);
        free(source);
        idm_error_clear(&err);
        _exit(wrote ? 0 : 1);
    }
    close(output_fd);
    close(timing_fd);
    if (events_fd >= 0) close(events_fd);
    job->pid = pid;
    return true;
}

static void test_job_read(TestJob *job, char **out, size_t *out_len) {
    if (job->cached) {
        *out = job->cached_output;
        *out_len = job->cached_output_len;
        job->cached_output = NULL;
        job->cached_output_len = 0;
        return;
    }
    *out = NULL;
    *out_len = 0;
    FILE *timing = fopen(job->timing_path, "rb");
    TestJobResult result;
    bool got_result = timing && fread(&result, 1u, sizeof(result), timing) == sizeof(result);
    if (timing) fclose(timing);
    if (got_result) {
        job->status = result.status;
        job->timing = result.timing;
    }
    IdmError err;
    idm_error_init(&err);
    if (!idm_read_file(job->output_path, out, out_len, &err)) idm_error_clear(&err);
    else idm_error_clear(&err);
}

static void test_job_destroy(TestJob *job) {
    free(job->cached_output);
    free(job->cached_events);
    if (job->output_path[0]) unlink(job->output_path);
    if (job->timing_path[0]) unlink(job->timing_path);
    if (job->events_path[0]) unlink(job->events_path);
}

static int run_tests(int argc, char **argv) {
    TestOptions opt = {0};
    opt.json = g_diag_json;
    char **paths = NULL;
    size_t path_count = 0;
    size_t path_cap = 0;
    bool parse_options = true;
    for (int i = 0; i < argc; i++) {
        if (parse_options && strcmp(argv[i], "--") == 0) {
            parse_options = false;
            continue;
        }
        if (parse_options && strcmp(argv[i], "-run") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "usage: idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-benchtime MS|Nx] [-json] [path...]\n");
                free(paths);
                return 64;
            }
            opt.run = argv[++i];
            continue;
        }
        if (parse_options && strcmp(argv[i], "-bench") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "usage: idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-benchtime MS|Nx] [-json] [path...]\n");
                free(paths);
                return 64;
            }
            opt.bench = argv[++i];
            continue;
        }
        if (parse_options && strcmp(argv[i], "-count") == 0) {
            if (i + 1 >= argc || !positive_decimal(argv[i + 1])) {
                fprintf(stderr, "usage: idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-benchtime MS|Nx] [-json] [path...]\n");
                free(paths);
                return 64;
            }
            opt.count = argv[++i];
            continue;
        }
        if (parse_options && strcmp(argv[i], "-benchtime") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "usage: idiomc test [-run PATTERN] [-bench PATTERN] [-count N] [-benchtime MS|Nx] [-json] [path...]\n");
                free(paths);
                return 64;
            }
            opt.benchtime = argv[++i];
            continue;
        }
        if (parse_options && strcmp(argv[i], "-json") == 0) {
            opt.json = true;
            continue;
        }
        if (parse_options && argv[i][0] == '-') {
            fprintf(stderr, "idiomc test: unknown option '%s'\n", argv[i]);
            free(paths);
            return 64;
        }
        if (!test_add_path(&paths, &path_count, &path_cap, argv[i])) {
            free(paths);
            return 1;
        }
    }
    char **files = NULL;
    size_t count = 0;
    size_t cap = 0;
    const char *default_path = opt.bench ? "tests/bench" : "tests/lang";
    if (path_count == 0) {
        if (test_collect_path(default_path, &files, &count, &cap, NULL) != 0) {
            fprintf(stderr, "idiomc test: cannot read %s\n", default_path);
            free(paths);
            return 1;
        }
    } else {
        for (size_t i = 0; i < path_count; i++) {
            if (test_collect_path(paths[i], &files, &count, &cap, NULL) != 0) {
                fprintf(stderr, "idiomc test: cannot read %s\n", paths[i]);
                for (size_t j = 0; j < count; j++) free(files[j]);
                free(files);
                free(paths);
                return 1;
            }
        }
    }
    free(paths);
    if (count == 0) {
        fprintf(stderr, "idiomc test: no test files found\n");
        free(files);
        return 1;
    }
    qsort(files, count, sizeof(*files), name_cmp);

    const char *mode_value = opt.bench ? "bench" : "test";
    const char *run_value = opt.run ? opt.run : "";
    const char *bench_value = opt.bench ? opt.bench : "";
    const char *count_value = opt.count ? opt.count : (opt.bench ? "3" : "1");
    (void)setenv("IDIOM_TEST_MODE", mode_value, 1);
    (void)setenv("IDIOM_TEST_RUN", run_value, 1);
    (void)setenv("IDIOM_TEST_BENCH", bench_value, 1);
    (void)setenv("IDIOM_TEST_COUNT", count_value, 1);
    (void)setenv("IDIOM_TEST_BENCHTIME", opt.benchtime ? opt.benchtime : "", 1);

    (void)unsetenv("IDIOM_TEST_EVENTS");
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    TestJob *jobs = calloc(count, sizeof(*jobs));
    if (!jobs) {
        for (size_t i = 0; i < count; i++) free(files[i]);
        free(files);
        return 1;
    }
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    size_t worker_count = cpu_count > 0 ? (size_t)cpu_count : 1u;
    if (worker_count > count) worker_count = count;
    size_t next = 0;
    size_t running = 0;
    size_t finished = 0;
    bool use_cache = opt.count == NULL && !opt.bench;
    IdmRuntime validator;
    idm_runtime_init(&validator);
    if (use_cache) {
        for (size_t i = 0; i < count; i++) {
            IdmError cache_err;
            idm_error_init(&cache_err);
            char *source = NULL;
            size_t source_len = 0;
            unsigned char key[32];
            bool hit = idm_read_file(files[i], &source, &source_len, &cache_err) &&
                       test_cache_key(files[i], source, source_len, opt.json, key) &&
                       tool_result_load(&validator, "tests", key, NULL, &jobs[i].cached_output, &jobs[i].cached_output_len, &jobs[i].cached_events, &jobs[i].cached_events_len);
            free(source);
            idm_error_clear(&cache_err);
            if (hit) {
                jobs[i].cached = true;
                jobs[i].status = 0;
                finished++;
            }
        }
    }
    while (finished < count) {
        while (next < count && running < worker_count) {
            if (jobs[next].cached) {
                next++;
                continue;
            }
            if (test_job_start(&jobs[next], files[next], opt.json, use_cache, tmpdir)) running++;
            else finished++;
            next++;
        }
        if (running == 0) continue;
        int wait_status = 0;
        pid_t pid = waitpid(-1, &wait_status, 0);
        if (pid < 0 && errno == EINTR) continue;
        if (pid < 0) {
            for (size_t i = 0; i < count; i++) {
                if (jobs[i].pid <= 0) continue;
                (void)kill(jobs[i].pid, SIGTERM);
                jobs[i].status = 1;
            }
            while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
            for (size_t i = 0; i < count; i++) jobs[i].pid = 0;
            break;
        }
        for (size_t i = 0; i < count; i++) {
            if (jobs[i].pid != pid) continue;
            if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) jobs[i].status = 1;
            jobs[i].pid = 0;
            break;
        }
        running--;
        finished++;
    }
    idm_runtime_destroy(&validator);
    size_t failed = 0;
    size_t benches = 0;
    for (size_t i = 0; i < count; i++) {
        char *output = NULL;
        size_t output_len = 0;
        test_job_read(&jobs[i], &output, &output_len);
        int status = jobs[i].status;
        if (opt.bench) {
            benches += emit_bench_output(files[i], output ? output : "", opt.json);
            if (opt.json && jobs[i].events_path[0]) {
                long offset = 0;
                json_test_events("bench", files[i], jobs[i].events_path, &offset);
            }
            if (opt.json) json_bench_file_event(files[i], &jobs[i].timing);
            else plain_bench_file_event(files[i], &jobs[i].timing);
        } else if (opt.json) {
            json_output_event(files[i], output ? output : "");
            if (jobs[i].cached) {
                json_test_events_data("test", files[i], jobs[i].cached_events ? jobs[i].cached_events : "", jobs[i].cached_events_len);
            } else if (jobs[i].events_path[0]) {
                long offset = 0;
                json_test_events("test", files[i], jobs[i].events_path, &offset);
            }
        } else if (output && output_len != 0) {
            fputs(output, stdout);
        }
        if (status != 0) {
            if (opt.json) json_file_event("fail", files[i]);
            else fprintf(stderr, "FAIL %s\n", files[i]);
            failed++;
        } else if (!opt.bench) {
            if (opt.json) json_file_event_cached("pass", files[i], jobs[i].cached);
            else printf(jobs[i].cached ? "pass %s (cached)\n" : "pass %s\n", files[i]);
        }
        free(output);
        test_job_destroy(&jobs[i]);
    }

    if (!opt.json) {
        if (failed == 0 && opt.bench) {
            printf("idiomc test: %zu benchmark%s passed in %zu file%s\n", benches, benches == 1 ? "" : "s", count, count == 1 ? "" : "s");
        } else if (failed == 0) {
            printf("idiomc test: %zu file%s passed\n", count, count == 1 ? "" : "s");
        } else {
            printf("idiomc test: %zu of %zu file%s FAILED\n", failed, count, count == 1 ? "" : "s");
        }
    }
    free(jobs);
    for (size_t i = 0; i < count; i++) free(files[i]);
    free(files);
    return failed == 0 ? 0 : 1;
}

static int run_sealed(const char *file, const unsigned char *data, size_t len) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        rt.interactive = true;
        idm_job_control_init();
    }
    rt.cli_args = g_cli_args;
    rt.cli_arg_count = g_cli_arg_count;
    IdmError err;
    idm_error_init(&err);
    int status = 1;
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmScheduler *sched = NULL;
    IdmByteReader r;
    uint32_t section_count = 0;
    if (!idm_wire_open(&r, data, len, &section_count, &err)) goto done;
    const unsigned char *main_data = NULL;
    size_t main_len = 0;
    const unsigned char *blob_data = NULL;
    size_t blob_len = 0;
    for (uint32_t i = 0; i < section_count; i++) {
        uint32_t kind = 0;
        const unsigned char *payload = NULL;
        size_t plen = 0;
        if (!idm_wire_next(&r, &kind, &payload, &plen, &err)) goto done;
        switch (kind) {
            case IDM_WIRE_SECTION_MAIN: main_data = payload; main_len = plen; break;
            case IDM_WIRE_SECTION_BYTECODE: blob_data = payload; blob_len = plen; break;
            default: break;
        }
    }
    if (!main_data || main_len != 4u || !blob_data) {
        idm_error_set(&err, idm_span_unknown(file), "sealed program is missing its main or bytecode section");
        goto done;
    }
    IdmByteReader mr = { main_data, main_len, 0u, true };
    uint32_t main_fn = idm_rd_u32(&mr);
    if (!idm_ic_deserialize(&rt, blob_data, blob_len, &module, &err)) goto done;
    if (main_fn >= module.function_count) {
        idm_error_set(&err, idm_span_unknown(file), "sealed program main function is out of bounds");
        goto done;
    }
    sched = idm_sched_create(&rt, &module, &err);
    if (!sched) goto done;
    IdmValue thunk = idm_closure_in_module(&rt, &module, main_fn, NULL, 0, rt.main_env, &err);
    if (err.present) goto done;
    IdmValue out = idm_nil();
    IdmValue reason = idm_nil();
    if (!idm_sched_run_session(sched, thunk, &out, &reason, &err)) goto done;
    status = idm_sched_session_status(sched, out, reason);
done:
    if (err.present) cli_error_print("run", &err);
    idm_error_clear(&err);
    idm_sched_destroy(sched);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    return status;
}

static bool source_arg_package_like(const char *arg) {
    if (strcmp(arg, "-") == 0) return false;
    struct stat st;
    if (stat(arg, &st) != 0) return true;
    return S_ISDIR(st.st_mode);
}

static bool build_package_entry_source(const char *package_path, char **out_source, IdmError *err) {
    IdmBuffer src;
    idm_buf_init(&src);
    bool ok = idm_buf_append(&src, "use ")
        && idm_buf_append(&src, package_path)
        && idm_buf_append(&src, "\nuse std/system with [args]\nmain args\n");
    if (!ok) {
        idm_buf_destroy(&src);
        return idm_error_oom(err, idm_span_unknown(package_path));
    }
    *out_source = idm_buf_take(&src);
    if (!*out_source) return idm_error_oom(err, idm_span_unknown(package_path));
    return true;
}

static bool build_cache_key(const char *file, const char *source, unsigned char out[32]) {
    IdmBuffer input;
    idm_buf_init(&input);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
    const char *root = getenv("IDIOMROOT");
    const char *path = getenv("IDIOMPATH");
    bool ok = idm_buf_append(&input, "IDM-SEALED-BUILD-v1" IDM_BUILD_ID) &&
              idm_buf_put_str(&input, file, strlen(file)) &&
              idm_buf_put_str(&input, source, strlen(source)) &&
              idm_buf_put_str(&input, cwd, strlen(cwd)) &&
              idm_buf_put_str(&input, root ? root : "", root ? strlen(root) : 0u) &&
              idm_buf_put_str(&input, path ? path : "", path ? strlen(path) : 0u);
    if (ok) idm_sha256(input.data ? input.data : "", input.len, out);
    idm_buf_destroy(&input);
    return ok;
}

static bool build_write_output(const char *path, const char *data, size_t len, IdmError *err) {
    FILE *f = fopen(path, "wb");
    if (!f) return idm_error_set(err, idm_span_unknown(path), "cannot write '%s'", path);
    bool wrote = fwrite(data, 1u, len, f) == len;
    if (fclose(f) != 0) wrote = false;
    if (!wrote) return idm_error_set(err, idm_span_unknown(path), "cannot write '%s'", path);
    if (chmod(path, 0755) != 0) return idm_error_set(err, idm_span_unknown(path), "cannot make '%s' executable", path);
    return true;
}

static int build_sealed(const char *src_path, const char *out_path) {
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    const char *file = NULL;
    int status = 1;
    IdmCore *core = NULL;
    IdmExpandTrace trace;
    memset(&trace, 0, sizeof(trace));
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmBuffer blob;
    idm_buf_init(&blob);
    IdmBuffer out;
    idm_buf_init(&out);
    if (source_arg_package_like(src_path)) {
        file = src_path;
        if (!build_package_entry_source(src_path, &source, &err)) goto done;
    } else if (!read_source_arg(src_path, &rt, &source, &file, &err)) goto done;
    unsigned char cache_key[32];
    bool have_key = build_cache_key(file, source, cache_key);
    char *cached = NULL;
    size_t cached_len = 0;
    char *unused = NULL;
    size_t unused_len = 0;
    if (have_key && tool_result_load(&rt, "builds", cache_key, NULL, &cached, &cached_len, &unused, &unused_len)) {
        bool wrote = build_write_output(out_path, cached, cached_len, &err);
        free(cached);
        free(unused);
        if (!wrote) goto done;
        printf("sealed %s (%zu bytes)\n", out_path, cached_len);
        status = 0;
        goto done;
    }
    free(cached);
    free(unused);
    if (!idm_expand_source_string_traced(&rt, file, source, &core, &trace, &err)) goto done;
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(&rt, core, &module, &main_fn, &err)) goto done;
    if (!idm_bc_tree_shake(&module, &main_fn, &err)) goto done;
    if (!idm_bc_verify(&module, &err)) goto done;
    if (!idm_ic_serialize(&module, &blob, &err)) goto done;
    IdmBuffer main_meta;
    idm_buf_init(&main_meta);
    bool sealed_ok = idm_buf_put_u32(&main_meta, main_fn) &&
                     idm_buf_append(&out, "#!/usr/bin/env idiomc\n") &&
                     idm_wire_begin(&out, 2u, &err) &&
                     idm_wire_section(&out, IDM_WIRE_SECTION_MAIN, main_meta.data, main_meta.len, &err) &&
                     idm_wire_section(&out, IDM_WIRE_SECTION_BYTECODE, blob.data, blob.len, &err);
    idm_buf_destroy(&main_meta);
    if (!sealed_ok) {
        if (!err.present) idm_error_oom(&err, idm_span_unknown(src_path));
        goto done;
    }
    if (!build_write_output(out_path, out.data, out.len, &err)) goto done;
    if (have_key && trace.cacheable) tool_result_store("builds", cache_key, &trace, NULL, out.data, out.len, "", 0u);
    printf("sealed %s (%zu bytes)\n", out_path, out.len);
    status = 0;
done:
    if (err.present) cli_error_print("build", &err);
    idm_error_clear(&err);
    idm_buf_destroy(&out);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_expand_trace_destroy(&trace);
    free(source);
    idm_runtime_destroy(&rt);
    return status;
}

static int dump_surface(const char *prelude) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBuffer out;
    idm_buf_init(&out);
    int status = 1;
    if (idm_expand_surface_dump(&rt, prelude, &out, &err)) {
        fputs(out.data ? out.data : "", stdout);
        status = 0;
    } else if (err.present) {
        cli_error_print("expand", &err);
    }
    idm_buf_destroy(&out);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return status;
}

#ifndef IDM_VERSION
#define IDM_VERSION "0.0.0-dev"
#endif

static int run_repl(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    rt.interactive = isatty(0) != 0;
    IdmError err;
    idm_error_init(&err);
    int status = 1;
    IdmRepl *repl = idm_repl_create(&rt, &err);
    if (!repl) goto done;
    IdmValue thunk = idm_nil();
    if (idm_repl_loop_thunk(repl, "use std/repl\nmain\n", &thunk, &err)) {
        IdmValue value = idm_nil();
        IdmValue reason = idm_nil();
        IdmScheduler *sched = idm_repl_scheduler(repl);
        if (idm_sched_run_session(sched, thunk, &value, &reason, &err)) {
            status = idm_sched_session_status(sched, value, reason);
        }
    }
done:
    if (err.present) cli_error_print("run", &err);
    idm_error_clear(&err);
    idm_repl_destroy(repl);
    idm_runtime_destroy(&rt);
    return status;
}

static int command_run(int argc, char **argv) {
    int path_index = 0;
    if (argc > 0 && strcmp(argv[0], "--") == 0) path_index = 1;
    if (path_index >= argc) {
        fprintf(stderr, "usage: idiomc run <file|package|-> [--] [args...]\n");
        return 64;
    }
    set_cli_args(argc, argv, path_index + 1);
    return run_file(argv[path_index]);
}

static int command_eval(int argc, char **argv) {
    int source_index = 0;
    if (argc > 0 && strcmp(argv[0], "--") == 0) source_index = 1;
    if (source_index >= argc) {
        fprintf(stderr, "usage: idiomc eval <source|-> [--] [args...]\n");
        return 64;
    }
    set_cli_args(argc, argv, source_index + 1);
    if (strcmp(argv[source_index], "-") == 0) {
        IdmError err;
        idm_error_init(&err);
        char *source = NULL;
        size_t len = 0;
        if (!idm_read_stream(stdin, "<stdin>", &source, &len, &err)) {
            cli_error_print("read", &err);
            idm_error_clear(&err);
            return 1;
        }
        int status = run_source("<stdin>", source, true);
        free(source);
        idm_error_clear(&err);
        return status;
    }
    return run_source("<eval>", argv[source_index], true);
}

static int command_build(int argc, char **argv) {
    const char *src_path = NULL;
    const char *out_path = NULL;
    bool parse_options = true;
    for (int i = 0; i < argc; i++) {
        if (parse_options && strcmp(argv[i], "--") == 0) {
            parse_options = false;
            continue;
        }
        if (parse_options && strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "usage: idiomc build <file|package|-> [-o OUT]\n");
                return 64;
            }
            out_path = argv[++i];
            continue;
        }
        if (parse_options && argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            fprintf(stderr, "idiomc build: unknown option '%s'\n", argv[i]);
            return 64;
        }
        if (src_path) {
            fprintf(stderr, "idiomc build: expected one source input\n");
            return 64;
        }
        src_path = argv[i];
    }
    if (!src_path) {
        fprintf(stderr, "usage: idiomc build <file|package|-> [-o OUT]\n");
        return 64;
    }
    char derived[1024];
    if (!out_path) {
        if (strcmp(src_path, "-") == 0) {
            fprintf(stderr, "idiomc build: stdin requires -o OUT\n");
            return 64;
        }
        snprintf(derived, sizeof(derived), "%s", src_path);
        char *dot = strrchr(derived, '.');
        if (dot && dot != derived) *dot = '\0';
        out_path = derived;
    }
    return build_sealed(src_path, out_path);
}

static int command_dump(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: idiomc dump reader|core|bytecode <file|package|->\n       idiomc dump surface [prelude]\n");
        return 64;
    }
    if (strcmp(argv[0], "surface") == 0) {
        if (argc > 2) {
            fprintf(stderr, "usage: idiomc dump surface [prelude]\n");
            return 64;
        }
        return dump_surface(argc == 2 ? argv[1] : "");
    }
    if (argc != 2) {
        fprintf(stderr, "usage: idiomc dump reader|core|bytecode <file|package|->\n");
        return 64;
    }
    if (strcmp(argv[0], "reader") == 0) return dump_reader(argv[1]);
    if (strcmp(argv[0], "core") == 0) return dump_core(argv[1]);
    if (strcmp(argv[0], "bytecode") == 0) return dump_bytecode(argv[1]);
    fprintf(stderr, "idiomc dump: unknown target '%s'\n", argv[0]);
    return 64;
}

static int command_explain(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: idiomc explain <file|package>[:LINE[:COL]]\n");
        return 64;
    }
    char *target = idm_strdup(argv[0]);
    if (!target) return 1;
    unsigned line = 0;
    unsigned column = 0;
    for (int part = 0; part < 2; part++) {
        char *colon = strrchr(target, ':');
        if (!colon || colon[1] == '\0') break;
        char *end = NULL;
        unsigned long parsed = strtoul(colon + 1, &end, 10);
        if (!end || *end != '\0') break;
        if (line == 0) {
            line = (unsigned)parsed;
        } else {
            column = line;
            line = (unsigned)parsed;
        }
        *colon = '\0';
    }
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    char *source = NULL;
    const char *file = NULL;
    int status = 1;
    if (!read_source_arg(target, &rt, &source, &file, &err)) {
        cli_error_print("read", &err);
        goto done;
    }
    IdmBuffer out;
    idm_buf_init(&out);
    if (idm_expand_explain_source(&rt, file, source, line, column, g_diag_json, &out, &err)) {
        fputs(out.data ? out.data : "", stdout);
        status = 0;
    } else {
        cli_error_print("expand", &err);
    }
    idm_buf_destroy(&out);
done:
    idm_error_clear(&err);
    free(source);
    free(target);
    idm_runtime_destroy(&rt);
    return status;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-json") == 0) {
        g_diag_json = true;
        argv[1] = argv[0];
        argv++;
        argc--;
    }
    if (argc <= 1) {
        usage(stderr);
        return 64;
    }
    const char *cmd = argv[1];
    if ((strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) && argc == 2) {
        usage(stdout);
        return 0;
    }
    if ((strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) && argc == 2) {
        printf("idiomc %s (idiom)\n", IDM_VERSION);
        return 0;
    }
    if (strcmp(cmd, "repl") == 0 && argc == 2) return run_repl();
    if (strcmp(cmd, "test") == 0) return run_tests(argc - 2, argv + 2);
    if (strcmp(cmd, "build") == 0) return command_build(argc - 2, argv + 2);
    if (strcmp(cmd, "run") == 0) return command_run(argc - 2, argv + 2);
    if (strcmp(cmd, "eval") == 0) return command_eval(argc - 2, argv + 2);
    if (strcmp(cmd, "dump") == 0) return command_dump(argc - 2, argv + 2);
    if (strcmp(cmd, "explain") == 0) return command_explain(argc - 2, argv + 2);
    if (cmd[0] != '-' || strcmp(cmd, "-") == 0) {
        set_cli_args(argc, argv, 2);
        return run_file(cmd);
    }
    usage(stderr);
    return 64;
}
