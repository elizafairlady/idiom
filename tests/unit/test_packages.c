#include "test_util.h"

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
    check_value_written(&rt, "use tests/pkg/macropriv\nphase-answer x\n", "77");
    check_value_written(&rt, "use tests/pkg/macropriv\ninc-private 41\n", "42");
    expect_expand_error_rt(&rt, "<cached-private-still-hidden>",
        "use tests/pkg/macropriv\n"
        "hidden 1\n",
        "unbound identifier 'hidden'");
    check_value_written(&rt, "use tests/pkg/exporter\n3 <+> 4\nanswer x\n", "99");
    check_value_written(&rt, "use tests/pkg/exporter\n3 <+> 4\n", "7");
    check_value_written(&rt,
        "implements std/shell\n"
        "x = do\n"
        "  implements std/shell\n"
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

void run_package_suite(void) {
    test_kernel_runtime_exports();
    test_surface_forms_are_kernel_sourced();
    test_artifact_cache_relocation();
    test_ishc_cache_invalidation();
    test_ishc_std_root_identity();
    test_package_cycle_detected();
    test_idiompath_lookup();
}
