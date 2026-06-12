#include "test_util.h"

int main(void) {
    run_substrate_suite();
    run_runtime_suite();
    run_surface_suite();
    run_macro_suite();
    run_protocol_suite();
    run_package_suite();
    run_diagnostics_suite();
    run_session_suite();
    run_tty_suite();
    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("unit tests passed");
    return 0;
}
