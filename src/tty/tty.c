#include "idiom/tty.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;
static bool g_saved_present;
static bool g_raw_active;

int idm_tty_in_fd(void) {
    return STDIN_FILENO;
}

static void tty_restore_backstop(void) {
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_saved);
        g_raw_active = false;
    }
}

static IdmValue tty_errno_atom(IdmRuntime *rt) {
    switch (errno) {
        case ENOTTY: return idm_atom(rt, "enotty");
        case EBADF: return idm_atom(rt, "ebadf");
        case EINVAL: return idm_atom(rt, "einval");
        default: return idm_atom(rt, "eio");
    }
}

static bool tty_error(IdmRuntime *rt, IdmError *err, const char *op) {
    idm_error_set(err, idm_span_unknown(NULL), "tty-%s!: %s", op, strerror(errno));
    return idm_error_reason(rt, err, "tty", 2, idm_atom(rt, op), tty_errno_atom(rt));
}

bool idm_prim_tty_pred(IdmRuntime *rt, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_atom(rt, isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) ? "true" : "false");
    return true;
}

bool idm_prim_tty_raw(IdmRuntime *rt, IdmValue *out, IdmError *err) {
    if (g_raw_active) {
        *out = idm_atom(rt, "ok");
        return true;
    }
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return tty_error(rt, err, "raw");
    if (!g_saved_present) {
        g_saved = t;
        g_saved_present = true;
        atexit(tty_restore_backstop);
    }
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_cflag |= (tcflag_t)CS8;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &t) != 0) return tty_error(rt, err, "raw");
    g_raw_active = true;
    *out = idm_atom(rt, "ok");
    return true;
}

bool idm_prim_tty_restore(IdmRuntime *rt, IdmValue *out, IdmError *err) {
    if (g_raw_active) {
        if (tcsetattr(STDIN_FILENO, TCSADRAIN, &g_saved) != 0) return tty_error(rt, err, "restore");
        g_raw_active = false;
    }
    *out = idm_atom(rt, "ok");
    return true;
}

bool idm_prim_tty_write(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_is_string(args[0])) {
        idm_error_set(err, idm_span_unknown(NULL), "tty-write expects a string");
        return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "tty-write"), args[0]);
    }
    const char *bytes = idm_string_bytes(args[0]);
    size_t len = idm_string_length(args[0]);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, bytes + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return tty_error(rt, err, "write");
        }
        off += (size_t)n;
    }
    *out = idm_atom(rt, "ok");
    return true;
}

bool idm_prim_tty_size(IdmRuntime *rt, IdmValue *out, IdmError *err) {
    struct winsize ws;
    int cols = 80;
    int rows = 24;
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 || ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) && ws.ws_col != 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    IdmValue items[2] = {idm_int(cols), idm_int(rows)};
    *out = idm_tuple(rt, items, 2u, err);
    return !(err && err->present);
}
