/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <ncursesw/curses.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define HISTORY 500
#define TEXT 4096
#define QUEUE 32
static char history[HISTORY][TEXT + 80], queue[QUEUE][TEXT];
static int lines, head, count, scrollback, logfd = -1;
static volatile sig_atomic_t stopping;
static pid_t watcher, gate, tts;
static char stage[PATH_MAX], wav[PATH_MAX], destination[PATH_MAX];
static const char *input, *output;
static const char *voice = "af_heart", *language = "a", *kokoro = "kokoro-offline";

static void signal_stop(int sig) { (void)sig; stopping = 1; }
static void die(const char *what) { perror(what); exit(1); }
static void path(char *dst, const char *a, const char *b) {
    if (snprintf(dst, PATH_MAX, "%s/%s", a, b) >= PATH_MAX) {
        errno = ENAMETOOLONG; die(a);
    }
}
static void directory(const char *name) {
    struct stat st;
    if (mkdir(name, 0700) && errno != EEXIST) die(name);
    if (stat(name, &st)) die(name);
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; die(name); }
}
static void add(const char *kind, const char *fmt, ...) {
    char text[TEXT], stamp[16];
    va_list args; va_start(args, fmt); vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    /* Do not interpret control characters from helper output as terminal commands. */
    for (char *p = text; *p; ++p)
        if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    if (lines == HISTORY) {
        memmove(history, history + 1, sizeof(history) - sizeof(history[0])); --lines;
    }
    snprintf(history[lines++], sizeof(history[0]), "[%s] %s %s", stamp, kind, text);
}
/* Each helper has its own process group; it cannot read the terminal. */
static pid_t spawn(char *const args[]) {
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0 || dup2(fd, STDIN_FILENO) < 0 ||
            dup2(logfd, STDOUT_FILENO) < 0 || dup2(logfd, STDERR_FILENO) < 0) _exit(126);
        if (fd > STDERR_FILENO) close(fd);
        execvp(args[0], args);
        dprintf(STDERR_FILENO, "Cannot execute %s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    if (pid > 0) setpgid(pid, pid);
    return pid;
}
static void stop_child(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            /* A shell can exit before its current helper. */
            kill(-pid, SIGKILL); return;
        }
        usleep(50000);
    }
    kill(-pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}
static void cleanup_stage(void) {
    if (*wav) unlink(wav);
    if (*stage) rmdir(stage);
    *wav = *stage = 0;
}
static void start_tts(void) {
    path(stage, input, ".operator-XXXXXX");
    if (!mkdtemp(stage)) { add("ERROR", "Cannot create staging directory: %s", strerror(errno)); *stage = 0; return; }
    path(wav, stage, "speech.wav");
    char name[128];
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    snprintf(name, sizeof(name), "speech-%020lld-%09ld-%s.wav", (long long)now.tv_sec,
             now.tv_nsec, strrchr(stage, '/') + 1 + strlen(".operator-"));
    path(destination, input, name);
    char *args[] = {(char *)kokoro, "--text", queue[head], "--voice", (char *)voice,
                    "--language", (char *)language, "--output-file", wav, NULL};
    tts = spawn(args);
    if (tts < 0) { tts = 0; add("ERROR", "Cannot start Kokoro: %s", strerror(errno)); cleanup_stage(); }
}
static bool valid_wav(void) {
    unsigned char header[12]; struct stat st;
    int fd = open(wav, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 44 &&
              read(fd, header, sizeof(header)) == sizeof(header) &&
              !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVE", 4);
    close(fd); return ok;
}
static void tick_tts(void) {
    if (!tts && count) {
        start_tts();
        if (!tts) { head = (head + 1) % QUEUE; --count; }
    }
    if (!tts) return;
    int status;
    if (waitpid(tts, &status, WNOHANG) != tts) return;
    tts = 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && valid_wav()) {
        /* Atomic publication, without replacing any existing queue entry. */
        if (link(wav, destination)) add("ERROR", "Cannot publish WAV: %s", strerror(errno));
        else add("TX ready", "%s", queue[head]);
    } else add("ERROR", "Kokoro failed; message was not queued for the gate (see helpers.log): %s", queue[head]);
    cleanup_stage(); head = (head + 1) % QUEUE; --count;
}
static void tail(FILE *file) {
    static char pending[TEXT]; static size_t used;
    clearerr(file);
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n') { pending[used] = 0; add("RX", "%s", pending); used = 0; }
        else if (used < sizeof(pending) - 1) pending[used++] = (char)ch;
    }
}
static void draw(const wchar_t *edit) {
    erase();
    if (LINES < 6 || COLS < 20) { mvaddstr(0, 0, "Resize terminal"); refresh(); return; }
    mvprintw(0, 0, "operator | RX %s | TX queue %d | %s", watcher > 0 ? "on" : "FAILED", count,
               gate > 0 ? "gate running" : "external gate");
    mvhline(1, 0, ACS_HLINE, COLS);
    int height = LINES - 5;
    int rows = height + 1;
    for (int i = 0; i < lines; ++i)
        rows += (int)strlen(history[i]) / (COLS - 1) + 2;
    /* ncurses uses short coordinates on some builds. */
    if (rows > 30000) rows = 30000;
    WINDOW *pad = newpad(rows, COLS - 1);
    if (pad) {
        scrollok(pad, TRUE);
        for (int i = 0; i < lines; ++i) { waddstr(pad, history[i]); waddch(pad, '\n'); }
        int bottom = getcury(pad), top = bottom - height;
        if (top < 0) top = 0;
        if (scrollback > top) scrollback = top;
        wnoutrefresh(stdscr);
        pnoutrefresh(pad, top - scrollback, 0, 2, 0, LINES - 4, COLS - 2);
        delwin(pad);
    }
    mvhline(LINES - 3, 0, ACS_HLINE, COLS);
    mvaddnstr(LINES - 2, 0, "Enter: send | PgUp/PgDn: history | Ctrl-U: clear | Ctrl-Q: quit", COLS - 1);
    mvaddstr(LINES - 1, 0, "> ");
    size_t start = wcslen(edit); int width = 0;
    while (start) {
        int w = wcwidth(edit[start - 1]); if (w < 0) w = 1;
        if (width + w > COLS - 4) break;
        width += w; --start;
    }
    addwstr(edit + start); wnoutrefresh(stdscr); doupdate();
}
/* Literal KEY=VALUE files: no shell evaluation or variable expansion. */
static void load_env(const char *filename, bool required) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        if (!required && errno == ENOENT) return;
        die("Cannot open environment file");
    }
    char *line = NULL; size_t capacity = 0; ssize_t size; unsigned number = 0;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        ++number;
        while (size && (line[size - 1] == '\n' || line[size - 1] == '\r')) line[--size] = 0;
        if (!size || line[0] == '#') continue;
        char *equal = strchr(line, '=');
        if (!equal) goto invalid;
        *equal = 0;
        if (strncmp(line, "OPERATOR_", 9) && strcmp(line, "WHISPER_BIN") && strcmp(line, "WHISPER_MODEL"))
            goto invalid;
        if (!strcmp(line, "OPERATOR_ENV_FILE")) goto invalid;
        for (char *p = line; *p; ++p)
            if (!(*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) goto invalid;
        if (setenv(line, equal + 1, 0)) die("setenv");
        continue;
invalid:
        fprintf(stderr, "Invalid environment entry at line %u (expected literal KEY=VALUE).\n", number);
        exit(2);
    }
    if (ferror(file)) die("Read environment file");
    free(line); fclose(file);
}
static const char *setting(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value ? value : fallback;
}
static void require_setting(const char *name, const char *value) {
    if (!value || !*value) {
        fprintf(stderr, "Set %s in operator.env or the environment.\n", name); exit(2);
    }
}
static void usage(const char *name) {
    printf("Usage: %s [--start-gate] [--env-file FILE]\n"
           "  [--input-dir DIR] [--output-dir DIR] [--voice VOICE] [--language LANGUAGE]\n"
           "  [--kokoro PROGRAM] [--state-file FILE] [--gate PROGRAM]\n"
           "Loads operator.env beside the executable (or OPERATOR_ENV_FILE).\n"
           "Default: attach to a separately running gate through the configured directories.\n"
           "--start-gate launches the gate using the configured connection settings.\n"
           "See operator.env.example for settings; command-line options override the environment,\n"
           "which overrides the file.\n", name);
}
int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    bool launch = false;
    char executable[PATH_MAX], watch[PATH_MAX], gatepath[PATH_MAX], envpath[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (n < 0 || n == sizeof(executable) - 1) die("executable path");
    executable[n] = 0; *strrchr(executable, '/') = 0;
    path(watch, executable, "watch-whisper.sh"); path(gatepath, executable, "../ptt_wav_gate");
    path(envpath, executable, "operator.env");
    const char *envfile = setting("OPERATOR_ENV_FILE", envpath);
    bool explicit_env = getenv("OPERATOR_ENV_FILE") != NULL;
    /* Select the file before reading any defaults; --help needs no configuration. */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--start-gate")) continue;
        if (i + 1 == argc) { usage(argv[0]); return 2; }
        if (!strcmp(argv[i], "--env-file")) { envfile = argv[i + 1]; explicit_env = true; }
        ++i;
    }
    load_env(envfile, explicit_env);
    if ((explicit_env || access(envfile, F_OK) == 0) &&
        setenv("OPERATOR_ENV_FILE", envfile, 1)) die("setenv");
    input = setting("OPERATOR_INPUT_DIR", NULL);
    output = setting("OPERATOR_OUTPUT_DIR", NULL);
    voice = setting("OPERATOR_VOICE", voice);
    language = setting("OPERATOR_LANGUAGE", language);
    kokoro = setting("OPERATOR_KOKORO", kokoro);
    const char *statefile = setting("OPERATOR_STATE_FILE", NULL);
    const char *gatebin = setting("OPERATOR_GATE", gatepath);
    const char *server = setting("OPERATOR_SERVER", NULL);
    const char *txid = setting("OPERATOR_TXID", NULL);
    const char *key = setting("OPERATOR_KEY", NULL);
    const char *preamble = setting("OPERATOR_PREAMBLE_ID", NULL);
    const char *jitter = setting("OPERATOR_JITTER_MS", "200");
    const char *fec = setting("OPERATOR_FEC_LOSS_PERCENT", "10");
    const char *timeout_ms = setting("OPERATOR_RX_STATE_TIMEOUT_MS", "1000");
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--start-gate")) { launch = true; continue; }
        if (i + 1 == argc) { usage(argv[0]); return 2; }
        const char *option = argv[i], *value = argv[++i];
        if (!strcmp(option, "--env-file")) continue;
        if (!strcmp(option, "--input-dir")) input = value;
        else if (!strcmp(option, "--output-dir")) output = value;
        else if (!strcmp(option, "--voice")) voice = value;
        else if (!strcmp(option, "--language")) language = value;
        else if (!strcmp(option, "--kokoro")) kokoro = value;
        else if (!strcmp(option, "--state-file")) statefile = value;
        else if (!strcmp(option, "--gate")) gatebin = value;
        else { usage(argv[0]); return 2; }
    }
    require_setting("OPERATOR_INPUT_DIR", input);
    require_setting("OPERATOR_OUTPUT_DIR", output);
    require_setting("WHISPER_MODEL", getenv("WHISPER_MODEL"));
    if (launch) {
        require_setting("OPERATOR_SERVER", server);
        require_setting("OPERATOR_TXID", txid);
        require_setting("OPERATOR_KEY", key);
        require_setting("OPERATOR_PREAMBLE_ID", preamble);
        require_setting("OPERATOR_STATE_FILE", statefile);
        char parent[PATH_MAX];
        if (strlen(statefile) >= sizeof(parent)) { errno = ENAMETOOLONG; die("state file"); }
        strcpy(parent, statefile);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) { *slash = 0; directory(parent); }
    }
    directory(input); directory(output);
    char inreal[PATH_MAX], outreal[PATH_MAX];
    if (!realpath(input, inreal) || !realpath(output, outreal)) die("directories");
    input = inreal; output = outreal;
    if (!strcmp(input, output)) { fprintf(stderr, "Input and output must differ.\n"); return 2; }
    char work[PATH_MAX], logfile[PATH_MAX], transcript[PATH_MAX], lockfile[PATH_MAX];
    path(work, output, "operator"); directory(work);
    path(lockfile, work, "lock");
    int lock = open(lockfile, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) die("Another operator may already be running");
    path(logfile, work, "helpers.log");
    logfd = open(logfile, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    if (logfd < 0) die(logfile);
    path(transcript, work, "transcriptions.log");
    int fd = open(transcript, O_CREAT | O_RDONLY | O_CLOEXEC, 0600);
    if (fd < 0) die(transcript);
    FILE *rx = fdopen(fd, "r"); if (!rx) die("fdopen");
    if (access(watch, R_OK)) die(watch);
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "operator requires an interactive terminal.\n"); return 1;
    }
    struct sigaction sa = {.sa_handler = signal_stop}; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    initscr(); raw(); noecho(); keypad(stdscr, TRUE); timeout(100);
    char *watchargs[] = {"bash", watch, (char *)output, transcript, NULL};
    watcher = spawn(watchargs);
    if (watcher < 0) { watcher = 0; add("ERROR", "Cannot launch Whisper watcher"); }
    if (launch) {
        char *args[] = {(char *)gatebin, (char *)server, "--txid", (char *)txid,
            "--input-dir", (char *)input, "--output-dir", (char *)output, "--encrypt",
            "--key", (char *)key, "--jitter-ms", (char *)jitter, "--fec-loss-percent", (char *)fec,
            "--preamble-id", (char *)preamble, "--state-file", (char *)statefile,
            "--rx-state-timeout-ms", (char *)timeout_ms, NULL};
        gate = spawn(args);
        if (gate < 0) { gate = 0; add("ERROR", "Cannot launch gate"); }
    }
    add("INFO", "Enter sends speech; TX ready means a WAV was queued, not delivery confirmation.");
    add("INFO", "Helper diagnostics: %s", logfile);
    wchar_t edit[1024] = {0}; size_t length = 0;
    while (!stopping) {
        tail(rx); tick_tts();
        pid_t *children[] = {&watcher, &gate};
        for (int i = 0; i < 2; ++i) {
            int status; pid_t pid = *children[i];
            if (pid > 0 && waitpid(pid, &status, WNOHANG) == pid) {
                kill(-pid, SIGKILL); *children[i] = 0;
                add("ERROR", "%s exited (status %d); see helpers.log", i ? "Gate" : "Whisper watcher", status);
            }
        }
        draw(edit);
        wint_t key; int result = get_wch(&key);
        if (result == ERR) continue;
        if (result == KEY_CODE_YES) {
            if (key == KEY_PPAGE) scrollback += LINES > 5 ? LINES - 5 : 1;
            else if (key == KEY_NPAGE) { scrollback -= LINES > 5 ? LINES - 5 : 1; if (scrollback < 0) scrollback = 0; }
            else if (key == KEY_BACKSPACE && length) edit[--length] = 0;
            else if (key != KEY_ENTER) continue;
            if (key != KEY_ENTER) continue;
            key = '\n';
        }
        if (key == 17 || key == 4 || key == 3) break;
        if (key == 21) { length = 0; edit[0] = 0; }
        else if ((key == 127 || key == 8) && length) edit[--length] = 0;
        else if (key == '\n' || key == '\r') {
            if (!length) continue;
            if (count == QUEUE) { add("ERROR", "Speech queue full; wait before sending."); continue; }
            char *message = queue[(head + count) % QUEUE];
            size_t bytes = wcstombs(message, edit, TEXT - 1);
            if (bytes == (size_t)-1) { add("ERROR", "Cannot encode message in current locale"); continue; }
            message[bytes] = 0; ++count; add("ME queued", "%s", message);
            length = 0; edit[0] = 0; scrollback = 0;
        } else if (iswprint(key) && length < sizeof(edit) / sizeof(*edit) - 1) {
            edit[length++] = (wchar_t)key; edit[length] = 0;
        }
    }
    endwin(); stop_child(tts); stop_child(watcher); stop_child(gate); cleanup_stage();
    fclose(rx); close(logfd); close(lock);
    if (count) fprintf(stderr, "%d unsynthesized message(s) cancelled on exit.\n", count);
    return 0;
}
