#include "test_util.h"

#include "idiom/artifact.h"

#include <utime.h>

static void test_kernel_runtime_exports(void) {
    const char *old = getenv("IDIOMROOT");
    char *saved = old ? idm_strdup(old) : NULL;
    setenv("IDIOMROOT", "tests/fixtures/kernelx", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "kernel-answer 41\n", "42");
    check_value_written(&rt, "kernel-answer (kernel-answer 40)\n", "42");
    expect_expand_error_rt(&rt, "<kernel-private-hidden>", "kernel-hidden 1\n", "unbound identifier 'kernel-hidden'");
    idm_runtime_destroy(&rt);
    if (saved) {
        setenv("IDIOMROOT", saved, 1);
        free(saved);
    } else {
        unsetenv("IDIOMROOT");
    }
}

static void test_kernel_uses_package_declaration_prep(void) {
    const char *old = getenv("IDIOMROOT");
    char *saved = old ? idm_strdup(old) : NULL;
    CHECK(mkdir("build/orderlesskernel", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/orderlesskernel/kernel", 0755) == 0 || errno == EEXIST);
    remove("build/orderlesskernel/kernel.ic");
    CHECK(write_text_file("build/orderlesskernel/kernel/a_use.id",
        "export defn kernel-answer x -> later-kernel x\n"));
    CHECK(write_text_file("build/orderlesskernel/kernel/z_surface.id",
        "defn later-kernel x -> add x 1\n"));
    setenv("IDIOMROOT", "build/orderlesskernel", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "kernel-answer 41\n", "42");
    idm_runtime_destroy(&rt);
    if (saved) {
        setenv("IDIOMROOT", saved, 1);
        free(saved);
    } else {
        unsetenv("IDIOMROOT");
    }
}

static void test_surface_forms_are_kernel_sourced(void) {
    const char *old = getenv("IDIOMROOT");
    char *saved = old ? idm_strdup(old) : NULL;
    setenv("IDIOMROOT", "tests/fixtures/kernelx", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<match-is-kernel>", "match 1 do\n  1 -> :one\nend\n", "unbound identifier 'match'");
    expect_expand_error_rt(&rt, "<error-is-kernel>", "error :boom\n", "unbound identifier 'error'");
    expect_expand_error_rt(&rt, "<if-is-kernel>", "if :true do 1 else do 2 end\n", "unbound identifier 'if'");
    expect_expand_error_rt(&rt, "<and-is-kernel>", "and 1 2\n", "unbound identifier 'and'");
    expect_expand_error_rt(&rt, "<operator-is-kernel>", "operator glue precedence: 50 -> add\n", "unbound identifier 'operator'");
    expect_expand_error_rt(&rt, "<infix-ops-are-kernel>", "1 + 2\n", "unbound identifier '+'");
    idm_runtime_destroy(&rt);
    if (saved) {
        setenv("IDIOMROOT", saved, 1);
        free(saved);
    } else {
        unsetenv("IDIOMROOT");
    }
}

static void test_artifact_cache_relocation(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "use tests/pkg/macropriv\nactivate MacroPriv\nphase-answer x\n", "77");
    check_value_written(&rt, "use tests/pkg/macropriv\nactivate MacroPriv\ninc-private 41\n", "42");
    expect_expand_error_rt(&rt, "<cached-private-still-hidden>",
        "use tests/pkg/macropriv\n"
        "hidden 1\n",
        "unbound identifier 'hidden'");
    check_value_written(&rt, "use tests/pkg/exporter\nactivate Exporter\n3 <+> 4\nanswer x\n", "99");
    check_value_written(&rt, "use tests/pkg/exporter\nactivate Exporter\n3 <+> 4\n", "7");
    check_value_written(&rt,
        "use app/ish\n"
        "activate Shell\n"
        "x = do\n"
        "  use app/ish\n"
        "  activate Shell\n"
        "  1\n"
        "end\n"
        "add x 1\n",
        "2");
    idm_runtime_destroy(&rt);
}

static void test_ishc_cache_invalidation(void) {
    remove("build/cachepkg.ic");
    CHECK(write_text_file("build/cachepkg.id", "package cachepkg\nexport defn cval x -> add x 1\n"));
    check_pkg_value("use build/cachepkg\ncval 41\n", "42");
    FILE *probe = fopen("build/cachepkg.ic", "rb");
    CHECK(probe != NULL);
    if (probe) fclose(probe);
    check_pkg_value("use build/cachepkg\ncval 41\n", "42");

    CHECK(write_text_file("build/cachepkg.ic", "garbage that is not an artifact"));
    check_pkg_value("use build/cachepkg\ncval 41\n", "42");

    CHECK(write_text_file("build/cachepkg.id", "package cachepkg\nexport defn cval x -> add x 2\n"));
    check_pkg_value("use build/cachepkg\ncval 41\n", "43");

    remove("build/cacheinner.ic");
    remove("build/cacheouter.ic");
    CHECK(write_text_file("build/cacheinner.id", "package cacheinner\nexport defn ival x -> 1\n"));
    CHECK(write_text_file("build/cacheouter.id", "package cacheouter\nuse build/cacheinner\nexport defn oval x -> add x (ival x)\n"));
    check_pkg_value("use build/cacheouter\noval 41\n", "42");
    CHECK(write_text_file("build/cacheinner.id", "package cacheinner\nexport defn ival x -> 10\n"));
    check_pkg_value("use build/cacheouter\noval 41\n", "51");

    remove("build/cachepkg.ic");
    setenv("IDIOMCACHE", "0", 1);
    check_pkg_value("use build/cachepkg\ncval 41\n", "43");
    probe = fopen("build/cachepkg.ic", "rb");
    CHECK(probe == NULL);
    if (probe) fclose(probe);
    unsetenv("IDIOMCACHE");

    const char *old_std = getenv("IDIOMROOT");
    char *saved = old_std ? idm_strdup(old_std) : NULL;
    CHECK(mkdir("build/kernelfix", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/kernelfix/kernel", 0755) == 0 || errno == EEXIST);
    remove("build/kernelfix/kernel.ic");
    remove("build/cachek.ic");
    CHECK(write_text_file("build/kernelfix/kernel/00-base.id", "export defn kfix x -> add x 1\n"));
    CHECK(write_text_file("build/cachek.id", "package cachek\nexport defn kval x -> kfix x\n"));
    setenv("IDIOMROOT", "build/kernelfix", 1);
    check_pkg_value("use build/cachek\nkval 41\n", "42");
    CHECK(write_text_file("build/kernelfix/kernel/00-base.id", "export defn kfix x -> add x 100\n"));
    check_pkg_value("use build/cachek\nkval 41\n", "141");
    if (saved) {
        setenv("IDIOMROOT", saved, 1);
        free(saved);
    } else {
        unsetenv("IDIOMROOT");
    }
}

static void test_ishc_std_root_identity(void) {
    const char *old_std = getenv("IDIOMROOT");
    char *saved = old_std ? idm_strdup(old_std) : NULL;
    CHECK(mkdir("build/stdA", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/stdA/kernel", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/stdB", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/stdB/kernel", 0755) == 0 || errno == EEXIST);
    CHECK(write_text_file("build/stdA/kernel/00-base.id", "export defn kfix x -> add x 1\n"));
    CHECK(write_text_file("build/stdB/kernel/00-base.id", "export defn kfix x -> add x 100\n"));
    CHECK(write_text_file("build/stdpkg.id", "package stdpkg\nexport defn sval x -> kfix x\n"));
    remove("build/stdpkg.ic");
    remove("build/stdA/kernel.ic");
    remove("build/stdB/kernel.ic");
    setenv("IDIOMROOT", "build/stdA", 1);
    check_pkg_value("use build/stdpkg\nsval 41\n", "42");
    setenv("IDIOMROOT", "build/stdB", 1);
    check_pkg_value("use build/stdpkg\nsval 41\n", "141");
    setenv("IDIOMROOT", "build/stdA", 1);
    check_pkg_value("use build/stdpkg\nsval 41\n", "42");
    if (saved) {
        setenv("IDIOMROOT", saved, 1);
        free(saved);
    } else {
        unsetenv("IDIOMROOT");
    }

    remove("build/cachenamed.ic");
    CHECK(write_text_file("build/cachenamed.id", "package cachenamed\nexport defn nval x -> add x 5\nexport defmacro nmac s -> (make-syntax-int s 77)\n"));
    check_pkg_value("use build/cachenamed\nnval 37\n", "42");
    check_pkg_value("import build/cachenamed as N\nN.nval 37\n", "42");
    check_pkg_value("import build/cachenamed as N\nN.nmac x\n", "77");
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<cached-import-no-unqualified-leak>",
        "import build/cachenamed as N\n"
        "nval 37\n",
        "unbound identifier 'nval'");
    expect_expand_error_rt(&rt, "<cached-import-no-unqualified-macro-leak>",
        "import build/cachenamed as N\n"
        "nmac x\n",
        "unbound identifier 'nmac'");
    idm_runtime_destroy(&rt);
}

static void test_directory_package_declaration_prep(void) {
    CHECK(mkdir("build/orderlesspkg", 0755) == 0 || errno == EEXIST);
    remove("build/orderlessdep.ic");
    remove("build/orderlesspkg.ic");
    CHECK(write_text_file("build/orderlessdep.id",
        "package orderlessdep\n"
        "export defn bump x -> 1000\n"
        "export defn depadd x -> add x 10\n"));
    CHECK(write_text_file("build/orderlesspkg/a_use.id",
        "package orderlesspkg\n"
        "export defn run x -> add (bump x) (later-macro y)\n"
        "export defn makep x -> Point x\n"
        "export defn labelp p -> p.v\n"
        "export defn use-forward-op x -> x <++> 1\n"
        "export defn makeb x -> Box x\n"
        "export defn call-label b -> label b\n"
        "export defn call-base b -> label-base b\n"
        "export defn makeq x -> Q.box x\n"
        "export defn call-q-label b -> label b\n"
        "export defn call-q-base b -> label-base b\n"
        "export defn use-forward-proto x -> x <!!> 1\n"
        "export defn use-dep x -> depadd x\n"));
    CHECK(write_text_file("build/orderlesspkg/z_surface.id",
        "package orderlesspkg\n"
        "use build/orderlessdep\n"
        "import tests/pkg/boxa as Q\n"
        "defn bump x -> add x 1\n"
        "defmacro later-macro stx -> make-syntax-int stx 40\n"
        "operator <++> precedence: 50 assoc: left capture: :infix -> add\n"
        "protocol LocalOps do\n"
        "  defn jump a b -> add a b\n"
        "  operator <!!> precedence: 50 assoc: left capture: :infix -> jump\n"
        "end\n"
        "activate LocalOps\n"
        "export record Point do\n"
        "  field v\n"
        "end\n"
        "export record Box do\n"
        "  field v\n"
        "end\n"
        "export trait Label do\n"
        "  require BaseLabel\n"
        "  method label x -> label-base x\n"
        "end\n"
        "export trait BaseLabel do\n"
        "  method label-base x\n"
        "end\n"
        "implement Label on Box\n"
        "implement BaseLabel on Box do\n"
        "  method label-base b -> add b.v 1\n"
        "end\n"
        "implement Label on Q.box\n"
        "implement BaseLabel on Q.box do\n"
        "  method label-base b -> add (Q.unbox b) 2\n"
        "end\n"));
    check_pkg_value("use build/orderlesspkg\n{(run 1) (labelp (makep 9)) (use-forward-op 41) (call-label (makeb 9)) (call-base (makeb 9)) (call-q-label (makeq 9)) (call-q-base (makeq 9)) (use-forward-proto 41) (use-dep 32)}\n", "{42 9 42 10 10 11 11 42 42}");
}

static void test_stale_cache_runs_no_phase_init(void) {
    const char *marker = "build/phase-init-marker.txt";
    remove(marker);
    remove("build/stalecache.ic");
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);

    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    idm_sha256("stalecache-src", 14u, art.src_hash);
    art.module = malloc(sizeof(*art.module));
    CHECK(art.module != NULL);
    idm_bc_init(art.module);
    uint32_t nil_const = 0;
    CHECK(idm_bc_add_const(art.module, idm_nil(), &nil_const));
    CHECK(idm_bc_add_function(art.module, "init", 0, 0, 0, &art.init_fn));
    CHECK(idm_bc_emit_u32(art.module, IDM_OP_LOAD_CONST, nil_const, NULL));
    CHECK(idm_bc_emit_op(art.module, IDM_OP_RETURN, NULL));

    IdmEnv *phase_runtime_env = idm_fresh_phase_runtime_env(&rt, &err);
    CHECK(phase_runtime_env != NULL && !err.present);
    art.phase_env = idm_phase_env_create(&rt, phase_runtime_env);
    CHECK(art.phase_env != NULL);
    IdmBytecodeModule *phase = malloc(sizeof(*phase));
    CHECK(phase != NULL);
    idm_bc_init(phase);
    uint32_t path_const = 0;
    uint32_t text_const = 0;
    CHECK(idm_bc_add_const(phase, idm_string(&rt, marker, &err), &path_const));
    CHECK(idm_bc_add_const(phase, idm_string(&rt, "ran", &err), &text_const));
    CHECK(!err.present);
    uint32_t file_write_fn = 0;
    CHECK(idm_bc_add_primitive_function(phase, idm_primitive_name(IDM_PRIM_FILE_WRITE), idm_primitive_arity(IDM_PRIM_FILE_WRITE), (uint32_t)IDM_PRIM_FILE_WRITE, &file_write_fn));
    uint32_t phase_fn = 0;
    CHECK(idm_bc_add_function(phase, "phase-init", 0, 0, 0, &phase_fn));
    CHECK(idm_bc_emit_u32(phase, IDM_OP_MAKE_CLOSURE, file_write_fn, NULL));
    CHECK(idm_bc_emit_u32(phase, IDM_OP_LOAD_CONST, path_const, NULL));
    CHECK(idm_bc_emit_u32(phase, IDM_OP_LOAD_CONST, text_const, NULL));
    CHECK(idm_bc_emit_u32(phase, IDM_OP_CALL, 2u, NULL));
    CHECK(idm_bc_emit(phase, 0u, NULL));
    CHECK(idm_bc_emit_op(phase, IDM_OP_RETURN, NULL));
    CHECK(idm_bc_intern_literals(&rt, phase, &err));
    CHECK(idm_phase_env_add_module(art.phase_env, phase, phase_fn, &err));

    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_artifact_serialize(&art, &blob, &err));
    CHECK(!err.present);
    FILE *f = fopen("build/stalecache.ic", "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fwrite(blob.data, 1u, blob.len, f) == blob.len);
        CHECK(fclose(f) == 0);
    }
    idm_buf_destroy(&blob);

    IdmArtifact fresh;
    CHECK(idm_artifact_cache_load(&rt, "build/stalecache", art.src_hash, &fresh, NULL, NULL));
    FILE *probe = fopen(marker, "rb");
    CHECK(probe != NULL);
    if (probe) fclose(probe);
    idm_artifact_destroy(&fresh);
    remove(marker);

    unsigned char stale_hash[32];
    idm_sha256("different-src", 13u, stale_hash);
    IdmArtifact stale;
    CHECK(!idm_artifact_cache_load(&rt, "build/stalecache", stale_hash, &stale, NULL, NULL));
    probe = fopen(marker, "rb");
    CHECK(probe == NULL);
    if (probe) fclose(probe);

    idm_artifact_destroy(&art);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    remove("build/stalecache.ic");
}

static void test_phase_read_invalidation(void) {
    struct utimbuf old_time = { 1000, 1000 };
    struct stat st;
    remove("build/readpkg.ic");
    CHECK(write_text_file("build/readpkg-data.txt", "A"));
    CHECK(write_text_file("build/readpkg.id",
        "package readpkg\n"
        "use std/file\n"
        "use std/string\n"
        "for-syntax do\n"
        "  defn payload do\n"
        "    {:ok s} -> s\n"
        "    _ -> \"\"\n"
        "  end\n"
        "  data = payload (read \"build/readpkg-data.txt\")\n"
        "  defmacro bakedm stx -> make-syntax-int stx (byte data 0)\n"
        "end\n"
        "export defn baked _ -> bakedm x\n"));
    check_pkg_value("use build/readpkg\nbaked 0\n", "65");
    CHECK(utime("build/readpkg.ic", &old_time) == 0);
    check_pkg_value("use build/readpkg\nbaked 0\n", "65");
    CHECK(stat("build/readpkg.ic", &st) == 0 && st.st_mtime == 1000);
    CHECK(write_text_file("build/readpkg-data.txt", "B"));
    check_pkg_value("use build/readpkg\nbaked 0\n", "66");
    CHECK(stat("build/readpkg.ic", &st) == 0 && st.st_mtime != 1000);

    remove("build/readuser.ic");
    CHECK(write_text_file("build/readuser.id", "package readuser\nuse build/readpkg\nexport defn ubaked x -> baked x\n"));
    check_pkg_value("use build/readuser\nubaked 0\n", "66");
    CHECK(write_text_file("build/readpkg-data.txt", "C"));
    check_pkg_value("use build/readuser\nubaked 0\n", "67");

    remove("build/probepkg.ic");
    remove("build/probe-target.txt");
    CHECK(write_text_file("build/probepkg.id",
        "package probepkg\n"
        "use std/file\n"
        "for-syntax do\n"
        "  defn flag do\n"
        "    :true -> 1\n"
        "    _ -> 0\n"
        "  end\n"
        "  seen = flag (exists? \"build/probe-target.txt\")\n"
        "  defmacro seenm stx -> make-syntax-int stx seen\n"
        "end\n"
        "export defn probed _ -> seenm x\n"));
    check_pkg_value("use build/probepkg\nprobed 0\n", "0");
    check_pkg_value("use build/probepkg\nprobed 0\n", "0");
    CHECK(write_text_file("build/probe-target.txt", "now"));
    check_pkg_value("use build/probepkg\nprobed 0\n", "1");
    remove("build/probe-target.txt");
    check_pkg_value("use build/probepkg\nprobed 0\n", "0");

    remove("build/noreads.ic");
    CHECK(write_text_file("build/noreads.id", "package noreads\nexport defn nr x -> add x 1\n"));
    check_pkg_value("use build/noreads\nnr 41\n", "42");
    CHECK(utime("build/noreads.ic", &old_time) == 0);
    CHECK(write_text_file("build/readpkg-data.txt", "D"));
    check_pkg_value("use build/noreads\nnr 41\n", "42");
    CHECK(stat("build/noreads.ic", &st) == 0 && st.st_mtime == 1000);

    remove("build/readpkg.ic");
    setenv("IDIOMCACHE", "0", 1);
    check_pkg_value("use build/readpkg\nbaked 0\n", "68");
    FILE *probe = fopen("build/readpkg.ic", "rb");
    CHECK(probe == NULL);
    if (probe) fclose(probe);
    unsetenv("IDIOMCACHE");
}

static void test_read_set_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    idm_sha256("roundtrip-src", 13u, art.src_hash);
    art.module = malloc(sizeof(*art.module));
    CHECK(art.module != NULL);
    idm_bc_init(art.module);
    uint32_t nil_const = 0;
    CHECK(idm_bc_add_const(art.module, idm_nil(), &nil_const));
    CHECK(idm_bc_add_function(art.module, "init", 0, 0, 0, &art.init_fn));
    CHECK(idm_bc_emit_u32(art.module, IDM_OP_LOAD_CONST, nil_const, NULL));
    CHECK(idm_bc_emit_op(art.module, IDM_OP_RETURN, NULL));
    art.deps = calloc(4u, sizeof(*art.deps));
    CHECK(art.deps != NULL);
    art.dep_count = 4u;
    art.deps[0].path = idm_strdup("std/kernel");
    art.deps[0].kind = IDM_DEP_PACKAGE;
    idm_sha256("kernel", 6u, art.deps[0].hash);
    art.deps[1].path = idm_strdup("/abs/data.txt");
    art.deps[1].kind = IDM_DEP_FILE_HASH;
    idm_sha256("data", 4u, art.deps[1].hash);
    art.deps[2].path = idm_strdup("/abs/somedir");
    art.deps[2].kind = IDM_DEP_FILE_PRESENT;
    art.deps[3].path = idm_strdup("/abs/missing.txt");
    art.deps[3].kind = IDM_DEP_FILE_ABSENT;
    for (size_t i = 0; i < art.dep_count; i++) CHECK(art.deps[i].path != NULL);

    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_artifact_serialize(&art, &blob, &err));
    CHECK(!err.present);
    IdmArtifact back;
    CHECK(idm_artifact_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &back, &err));
    CHECK(!err.present);
    CHECK(back.dep_count == art.dep_count);
    for (size_t i = 0; i < art.dep_count && i < back.dep_count; i++) {
        CHECK_STR(back.deps[i].path, art.deps[i].path);
        CHECK(back.deps[i].kind == art.deps[i].kind);
        CHECK(memcmp(back.deps[i].hash, art.deps[i].hash, 32u) == 0);
    }
    CHECK(!idm_artifact_dep_verified(&rt, &back.deps[0]));
    idm_buf_destroy(&blob);
    idm_artifact_destroy(&back);
    idm_artifact_destroy(&art);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_package_cycle_detected(void) {
    CHECK(write_text_file("build/cyclea.id", "package cyclea\nuse build/cycleb\nexport defn aval x -> x\n"));
    CHECK(write_text_file("build/cycleb.id", "package cycleb\nuse build/cyclea\nexport defn bval x -> x\n"));
    remove("build/cyclea.ic");
    remove("build/cycleb.ic");
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<package-cycle>", "use build/cyclea\naval 1\n", "package dependency cycle");
    idm_runtime_destroy(&rt);
}

static void test_idiompath_lookup(void) {
    const char *old = getenv("IDIOMPATH");
    char *saved = old ? idm_strdup(old) : NULL;
    setenv("IDIOMPATH", "tests/fixtures/pathroot", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "use extra\nquadruple 10\n", "40");
    expect_expand_error_rt(&rt, "<idiompath-miss>", "use not-a-real-package\n1\n", "package 'not-a-real-package' not found");
    idm_runtime_destroy(&rt);
    if (saved) {
        setenv("IDIOMPATH", saved, 1);
        free(saved);
    } else {
        unsetenv("IDIOMPATH");
    }
}

static void test_phase_env_rejects_prepared_uninterned_module(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmEnv *phase_runtime_env = idm_fresh_phase_runtime_env(&rt, &err);
    CHECK(phase_runtime_env != NULL);
    IdmPhaseEnv *env = idm_phase_env_create(&rt, phase_runtime_env);
    CHECK(env != NULL);
    IdmBytecodeModule *module = malloc(sizeof(*module));
    CHECK(module != NULL);
    idm_bc_init(module);
    uint32_t c1 = 0;
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_const(module, idm_int(1), &c1));
    CHECK(idm_bc_add_function(module, "phase", 0, 0, 0, &main_fn));
    CHECK(idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, c1, NULL));
    CHECK(idm_bc_emit_op(module, IDM_OP_RETURN, NULL));
    CHECK(idm_bc_prepare_selectors(module, &err));
    CHECK(!idm_phase_env_add_module(env, module, main_fn, &err));
    CHECK(err.present && err.message && strstr(err.message, "finalized") != NULL);
    idm_error_clear(&err);
    CHECK(idm_bc_intern_literals(&rt, module, &err));
    CHECK(idm_phase_env_add_module(env, module, main_fn, &err));
    CHECK(!err.present);
    idm_phase_env_release(env);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static IdmSyntax *test_tuple2(const char *tag, IdmSyntax *item, IdmSpan span) {
    IdmSyntax *tuple = idm_syn_tuple(span);
    IdmSyntax *head = idm_syn_atom(tag, span);
    if (!tuple || !head || !item) {
        idm_syn_free(tuple);
        idm_syn_free(head);
        idm_syn_free(item);
        return NULL;
    }
    if (!idm_syn_append(tuple, head)) {
        idm_syn_free(head);
        idm_syn_free(item);
        idm_syn_free(tuple);
        return NULL;
    }
    if (!idm_syn_append(tuple, item)) {
        idm_syn_free(item);
        idm_syn_free(tuple);
        return NULL;
    }
    return tuple;
}

static bool test_make_grammar_rule(IdmGrammarRule *rule, const char *name, IdmSpan span) {
    memset(rule, 0, sizeof(*rule));
    rule->name = idm_strdup(name);
    rule->kind = (uint8_t)IDM_GRAMMAR_RULE_TOKEN;
    IdmSyntax *regex_source = idm_syn_string("x", span);
    IdmSyntax *capture_name = idm_syn_word(name, span);
    IdmSyntax *pattern = test_tuple2("regex", regex_source, span);
    IdmSyntax *constructor = test_tuple2("capture", capture_name, span);
    IdmError err;
    idm_error_init(&err);
    bool ok = rule->name && pattern && constructor &&
              idm_grammar_terminal_from_ir(pattern, &rule->terminal, &err) &&
              idm_reader_ctor_compile_ir(constructor, &rule->constructor, &err);
    if (!ok) {
        idm_grammar_rule_destroy(rule);
    }
    idm_syn_free(pattern);
    idm_syn_free(constructor);
    idm_error_clear(&err);
    return ok;
}

static void test_artifact_rejects_duplicate_grammar_surfaces(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    idm_sha256("dup-grammar-surfaces", 20u, art.src_hash);
    art.grammar_count = 2u;
    art.grammars = calloc(art.grammar_count, sizeof(*art.grammars));
    CHECK(art.grammars != NULL);
    for (size_t i = 0; i < art.grammar_count; i++) {
        art.grammars[i].name = idm_strdup("ReaderSurface");
        art.grammars[i].mode = (uint8_t)IDM_GRAMMAR_MODE_EXTEND;
        art.grammars[i].rule_count = 1u;
        art.grammars[i].rules = calloc(1u, sizeof(*art.grammars[i].rules));
        CHECK(art.grammars[i].name != NULL);
        CHECK(art.grammars[i].rules != NULL);
        CHECK(test_make_grammar_rule(&art.grammars[i].rules[0], i == 0 ? "a" : "b", idm_span_unknown(NULL)));
    }
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(!idm_artifact_serialize(&art, &blob, &err));
    CHECK(err.present && err.message && strstr(err.message, "declares surface") != NULL);
    idm_buf_destroy(&blob);
    idm_artifact_destroy(&art);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_artifact_rejects_missing_source_reader_grammar(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    idm_sha256("missing-source-reader", 21u, art.src_hash);
    art.source_reader = idm_strdup("MissingReader");
    art.grammar_count = 1u;
    art.grammars = calloc(art.grammar_count, sizeof(*art.grammars));
    CHECK(art.source_reader != NULL);
    CHECK(art.grammars != NULL);
    art.grammars[0].name = idm_strdup("PresentReader");
    art.grammars[0].mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    art.grammars[0].rule_count = 1u;
    art.grammars[0].rules = calloc(1u, sizeof(*art.grammars[0].rules));
    CHECK(art.grammars[0].name != NULL);
    CHECK(art.grammars[0].rules != NULL);
    CHECK(test_make_grammar_rule(&art.grammars[0].rules[0], "a", idm_span_unknown(NULL)));
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(!idm_artifact_serialize(&art, &blob, &err));
    CHECK(err.present && err.message && strstr(err.message, "has no core-grammar") != NULL);
    idm_buf_destroy(&blob);
    idm_artifact_destroy(&art);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

void run_package_suite(void) {
    test_kernel_runtime_exports();
    test_kernel_uses_package_declaration_prep();
    test_surface_forms_are_kernel_sourced();
    test_artifact_cache_relocation();
    test_ishc_cache_invalidation();
    test_ishc_std_root_identity();
    test_directory_package_declaration_prep();
    test_stale_cache_runs_no_phase_init();
    test_phase_read_invalidation();
    test_read_set_roundtrip();
    test_package_cycle_detected();
    test_idiompath_lookup();
    test_phase_env_rejects_prepared_uninterned_module();
    test_artifact_rejects_duplicate_grammar_surfaces();
    test_artifact_rejects_missing_source_reader_grammar();
}
