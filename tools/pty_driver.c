#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

static void die(const char *what) {
    fprintf(stderr, "pty_driver: %s: %s\n", what, strerror(errno));
    exit(2);
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

static void wait_for(int master, const char *text) {
    for (int i = 0; i < 500; i++) {
        if (g_seen && strstr(g_seen, text)) return;
        drain(master, 10);
    }
    fprintf(stderr, "pty_driver: timed out waiting for '%s'\n", text);
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
        else if (line[0] == 'w' && line[1] == ' ') wait_for(master, line + 2);
        else die("bad script line");
        drain(master, 10);
    }
    free(line);
    fclose(script);

    for (int i = 0; i < 500; i++) {
        int status = 0;
        drain(master, 10);
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            drain(master, 50);
            fflush(stdout);
            close(master);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }
    fprintf(stderr, "pty_driver: child did not exit\n");
    kill(child, SIGKILL);
    return 2;
}
