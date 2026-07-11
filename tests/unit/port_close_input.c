#include "idiom/ports.h"
#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "port_close_input: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static IdmValue lit(IdmRuntime *rt, const char *s, IdmError *err) {
    IdmValue items[2] = { idm_atom(rt, "lit"), idm_string(rt, s, err) };
    return idm_tuple(rt, items, 2u, err);
}

static IdmValue pty_stage_graph(IdmRuntime *rt, const char *command, IdmError *err) {
    IdmValue argv = idm_empty_list();
    argv = idm_cons(rt, lit(rt, command, err), argv, err);
    argv = idm_cons(rt, lit(rt, "-c", err), argv, err);
    argv = idm_cons(rt, lit(rt, "sh", err), argv, err);
    IdmValue stage_items[4] = { idm_atom(rt, "stage"), argv, idm_empty_list(), idm_empty_list() };
    IdmValue stage = idm_tuple(rt, stage_items, 4u, err);
    IdmValue stdio_items[4] = { idm_atom(rt, "stdio"), idm_atom(rt, "pty"), idm_atom(rt, "pty"), idm_atom(rt, "null") };
    IdmValue stdio = idm_tuple(rt, stdio_items, 4u, err);
    IdmValue exec_items[3] = { idm_atom(rt, "exec"), stage, stdio };
    return idm_tuple(rt, exec_items, 3u, err);
}

static void test_close_input_delivers_veof_then_is_idempotent(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);

    IdmValue graph = pty_stage_graph(&rt, "exec cat >/dev/null", &err);
    check(!err.present, "build graph");
    IdmPort *port = idm_port_launch(&rt, graph, NULL, &err);
    check(port != NULL && !err.present, "launch canonical pty port");

    IdmPortIoStatus first = idm_port_close_input(port);
    check(first == IDM_PORT_IO_OK, "close-input delivers VEOF on a drainable canonical pty");

    IdmPortIoStatus again = idm_port_close_input(port);
    check(again == IDM_PORT_IO_CLOSED, "close-input on an already-closed stdin reports :closed");

    idm_port_free(port);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

int idm_unit_port_close_input(void) {
    test_close_input_delivers_veof_then_is_idempotent();
    return 0;
}
