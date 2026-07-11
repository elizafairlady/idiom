#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char *g_seen;
static size_t g_seen_len;
static size_t g_seen_cap;
static int g_expect_exit = -1;

static void die(const char *what) {
    fprintf(stderr, "pty_driver: %s: %s\n", what, strerror(errno));
    exit(2);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void drain(int master, int timeout_ms) {
    struct pollfd p;
    p.fd = master;
    p.events = POLLIN;
    for (;;) {
        p.revents = 0;
        int r = poll(&p, 1u, timeout_ms);
        if (r <= 0 || (p.revents & (POLLIN | POLLHUP)) == 0) return;
        char buf[4096];
        ssize_t n = read(master, buf, sizeof(buf));
        if (n <= 0) return;
        fwrite(buf, 1u, (size_t)n, stdout);
        if (g_seen_len + (size_t)n + 1u > g_seen_cap) {
            size_t cap = g_seen_cap ? g_seen_cap * 2u : 8192u;
            while (cap < g_seen_len + (size_t)n + 1u) cap *= 2u;
            g_seen = realloc(g_seen, cap);
            if (!g_seen) die("realloc");
            g_seen_cap = cap;
        }
        memcpy(g_seen + g_seen_len, buf, (size_t)n);
        g_seen_len += (size_t)n;
        g_seen[g_seen_len] = '\0';
        timeout_ms = 10;
    }
}

static void wait_for(int master, const char *text, long deadline_ms) {
    uint64_t start = now_ms();
    while (now_ms() - start < (uint64_t)deadline_ms) {
        if (g_seen && strstr(g_seen, text)) return;
        drain(master, 25);
    }
    fprintf(stderr, "pty_driver: timed out waiting for '%s'\n", text);
    exit(2);
}

#define GRID_ROWS 24
#define GRID_BYTES 512

static char g_grid[GRID_ROWS][GRID_BYTES];
static int g_cur_row;
static int g_cur_col;

static void grid_clear(void) {
    memset(g_grid, ' ', sizeof(g_grid));
    g_cur_row = 0;
    g_cur_col = 0;
}

static void grid_replay(void) {
    grid_clear();
    for (size_t i = 0; i < g_seen_len; i++) {
        unsigned char c = (unsigned char)g_seen[i];
        if (c == 0x1b && i + 1u < g_seen_len && g_seen[i + 1u] == '[') {
            size_t j = i + 2u;
            int params[4] = {0, 0, 0, 0};
            int nparams = 0;
            int have_digit = 0;
            int private_seq = 0;
            while (j < g_seen_len) {
                char p = g_seen[j];
                if (p >= '0' && p <= '9') {
                    if (nparams < 4) params[nparams] = params[nparams] * 10 + (p - '0');
                    have_digit = 1;
                    j++;
                } else if (p == ';') {
                    if (have_digit && nparams < 4) nparams++;
                    have_digit = 0;
                    j++;
                } else if (p >= 0x20 && p <= 0x3F) {
                    private_seq = 1;
                    j++;
                } else {
                    break;
                }
            }
            if (private_seq) {
                i = j < g_seen_len ? j : g_seen_len - 1u;
                continue;
            }
            if (have_digit && nparams < 4) nparams++;
            if (j >= g_seen_len) break;
            char fin = g_seen[j];
            if (fin == 'H') {
                g_cur_row = nparams >= 1 ? params[0] - 1 : 0;
                g_cur_col = nparams >= 2 ? params[1] - 1 : 0;
                if (g_cur_row < 0) g_cur_row = 0;
                if (g_cur_col < 0) g_cur_col = 0;
            } else if (fin == 'J') {
                if (nparams >= 1 && params[0] == 2) {
                    memset(g_grid, ' ', sizeof(g_grid));
                } else if (nparams == 0 || params[0] == 0) {
                    if (g_cur_row < GRID_ROWS && g_cur_col < GRID_BYTES) {
                        memset(g_grid[g_cur_row] + g_cur_col, ' ', (size_t)(GRID_BYTES - g_cur_col));
                    }
                    for (int r = g_cur_row + 1; r < GRID_ROWS; r++) memset(g_grid[r], ' ', GRID_BYTES);
                }
            } else if (fin == 'K') {
                if (g_cur_row < GRID_ROWS && g_cur_col < GRID_BYTES) {
                    memset(g_grid[g_cur_row] + g_cur_col, ' ', (size_t)(GRID_BYTES - g_cur_col));
                }
            }
            i = j;
            continue;
        }
        if (c == '\r') {
            g_cur_col = 0;
            continue;
        }
        if (c == '\n') {
            if (g_cur_row + 1 < GRID_ROWS) g_cur_row++;
            continue;
        }
        if (c == 0x08) {
            if (g_cur_col > 0) g_cur_col--;
            continue;
        }
        if (c < 32 && c != 9) continue;
        if (g_cur_row < GRID_ROWS && g_cur_col < GRID_BYTES) {
            g_grid[g_cur_row][g_cur_col] = (char)c;
            g_cur_col++;
        }
    }
}

static size_t rtrim_len(const char *s, size_t len) {
    while (len > 0 && (s[len - 1u] == ' ' || s[len - 1u] == '\t')) len--;
    return len;
}

static int grid_matches(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) die("open grid file");
    grid_replay();
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int row = 0;
    int ok = 1;
    while (ok && (n = getline(&line, &cap, f)) >= 0 && row < GRID_ROWS) {
        size_t want = (size_t)n;
        if (want > 0 && line[want - 1u] == '\n') want--;
        want = rtrim_len(line, want);
        size_t have = rtrim_len(g_grid[row], GRID_BYTES);
        if (want != have || memcmp(line, g_grid[row], want) != 0) ok = 0;
        row++;
    }
    for (int r = row; ok && r < GRID_ROWS; r++) {
        if (rtrim_len(g_grid[r], GRID_BYTES) != 0) ok = 0;
    }
    free(line);
    fclose(f);
    return ok;
}

static void grid_dump(void) {
    grid_replay();
    for (int r = 0; r < GRID_ROWS; r++) {
        size_t len = rtrim_len(g_grid[r], GRID_BYTES);
        fprintf(stderr, "%2d|%.*s\n", r + 1, (int)len, g_grid[r]);
    }
    fprintf(stderr, "cursor: %d;%d\n", g_cur_row + 1, g_cur_col + 1);
}

static void wait_for_grid(int master, const char *path, long deadline_ms) {
    uint64_t start = now_ms();
    while (now_ms() - start < (uint64_t)deadline_ms) {
        if (grid_matches(path)) return;
        drain(master, 25);
    }
    fprintf(stderr, "pty_driver: grid never matched '%s'; final screen:\n", path);
    grid_dump();
    exit(2);
}

static void wait_for_cursor(int master, int row, int col, long deadline_ms) {
    uint64_t start = now_ms();
    while (now_ms() - start < (uint64_t)deadline_ms) {
        grid_replay();
        if (g_cur_row + 1 == row && g_cur_col + 1 == col) return;
        drain(master, 25);
    }
    fprintf(stderr, "pty_driver: cursor never reached %d;%d; final screen:\n", row, col);
    grid_dump();
    exit(2);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void send_text(int master, const char *text) {
    char out[1024];
    size_t len = 0;
    for (size_t i = 0; text[i] != '\0' && len + 1u < sizeof(out); i++) {
        char c = text[i];
        if (c == '\\' && text[i + 1u] != '\0') {
            char e = text[++i];
            switch (e) {
                case 'e': c = '\x1b'; break;
                case 'r': c = '\r'; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '\\': c = '\\'; break;
                case 'x': {
                    int hi = hex_value(text[i + 1u]);
                    int lo = hi >= 0 ? hex_value(text[i + 2u]) : -1;
                    if (lo < 0) die("bad \\x escape");
                    c = (char)(hi * 16 + lo);
                    i += 2u;
                    break;
                }
                default: die("bad escape");
            }
        }
        out[len++] = c;
    }
    if (write(master, out, len) != (ssize_t)len) die("write");
}

static void pause_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: pty_driver KEYSCRIPT CMD [ARGS...]\n");
        return 64;
    }
    FILE *script = fopen(argv[1], "r");
    if (!script) die("open keyscript");
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) die("posix_openpt");
    if (grantpt(master) != 0 || unlockpt(master) != 0) die("unlockpt");
    const char *slave_name = ptsname(master);
    if (!slave_name) die("ptsname");
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = 80;
    ws.ws_row = 24;
    ioctl(master, TIOCSWINSZ, &ws);

    pid_t child = fork();
    if (child < 0) die("fork");
    if (child == 0) {
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) die("open slave");
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > 2) close(slave);
        close(master);
        setenv("TERM", "xterm", 1);
        execvp(argv[2], argv + 2);
        die("execvp");
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, script)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        if (line[0] == 's' && line[1] == ' ') send_text(master, line + 2);
        else if (line[0] == 'p' && line[1] == ' ') pause_ms(strtol(line + 2, NULL, 10));
        else if (line[0] == 'w' && line[1] == ' ') wait_for(master, line + 2, 10000);
        else if (line[0] == 'g' && line[1] == ' ') wait_for_grid(master, line + 2, 10000);
        else if (line[0] == 'c' && line[1] == ' ') {
            char *rest = NULL;
            int row = (int)strtol(line + 2, &rest, 10);
            int col = rest ? (int)strtol(rest, NULL, 10) : 0;
            wait_for_cursor(master, row, col, 10000);
        }
        else if (line[0] == 'k' && line[1] == ' ') {
            int signo = strcmp(line + 2, "INT") == 0 ? SIGINT
                      : strcmp(line + 2, "TERM") == 0 ? SIGTERM
                      : 0;
            if (signo == 0) die("bad signal");
            pid_t fg = tcgetpgrp(master);
            if (fg <= 0 || kill(-fg, signo) != 0) {
                if (kill(child, signo) != 0) die("kill");
            }
        }
        else if (line[0] == 'r' && line[1] == ' ') {
            char *rest = NULL;
            long cols = strtol(line + 2, &rest, 10);
            long rows = rest ? strtol(rest, NULL, 10) : 0;
            if (cols <= 0 || rows <= 0 || rows > GRID_ROWS) die("bad resize");
            memset(&ws, 0, sizeof(ws));
            ws.ws_col = (unsigned short)cols;
            ws.ws_row = (unsigned short)rows;
            if (ioctl(master, TIOCSWINSZ, &ws) != 0) die("resize");
        }
        else if (line[0] == 'x' && line[1] == ' ') g_expect_exit = (int)strtol(line + 2, NULL, 10);
        else die("bad script line");
        drain(master, 10);
    }
    free(line);
    fclose(script);

    uint64_t start = now_ms();
    while (now_ms() - start < 10000u) {
        int status = 0;
        drain(master, 10);
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            drain(master, 50);
            fflush(stdout);
            close(master);
            int actual = WIFEXITED(status) ? WEXITSTATUS(status)
                       : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
            if (g_expect_exit >= 0) {
                if (actual == g_expect_exit) return 0;
                fprintf(stderr, "pty_driver: child exit %d, expected %d\n", actual, g_expect_exit);
                return 1;
            }
            return actual;
        }
    }
    fprintf(stderr, "pty_driver: child did not exit\n");
    kill(child, SIGKILL);
    return 2;
}
