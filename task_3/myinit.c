#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>

#define MAX_PROCS 64
#define MAX_LINE 1024
#define MAX_ARGS 32
#define LOG_FILE "/tmp/myinit.log"
#define PID_FILE "/tmp/myinit.pid"

typedef struct {
    char cmdline[MAX_LINE];
    char *argv[MAX_ARGS + 1];
    char stdin_file[MAX_LINE];
    char stdout_file[MAX_LINE];
    pid_t pid;
} ProcEntry;

static ProcEntry procs[MAX_PROCS];
static int nprocs = 0;
static char config_path[MAX_LINE];
static int log_fd = -1;

static volatile sig_atomic_t got_sighup = 0;
static volatile sig_atomic_t got_sigterm = 0;

static void log_write(const char *fmt, ...) {
    if (log_fd < 0) return;

    time_t now = time(NULL);
    char tbuf[32];
    struct tm tinfo;
    localtime_r(&now, &tinfo);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tinfo);

    char prefix[64];
    int plen = snprintf(prefix, sizeof(prefix), "[%s] ", tbuf);
    write(log_fd, prefix, plen);

    char buf[MAX_LINE + 256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len > 0) {
        write(log_fd, buf, len);
        if (buf[len - 1] != '\n')
            write(log_fd, "\n", 1);
    }
}

static void sighup_handler(int sig) { (void)sig; got_sighup = 1; }
static void sigterm_handler(int sig) { (void)sig; got_sigterm = 1; }

static void free_proc_entry(ProcEntry *pe) {
    for (int i = 0; i <= MAX_ARGS; i++) {
        if (pe->argv[i]) {
            free(pe->argv[i]);
            pe->argv[i] = NULL;
        }
    }
}

static int read_config(void) {
    FILE *f = fopen(config_path, "r");
    if (!f) {
        log_write("ERROR: fopen('%s'): %s", config_path, strerror(errno));
        return -1;
    }

    nprocs = 0;
    char line[MAX_LINE];

    while (fgets(line, sizeof(line), f) != NULL && nprocs < MAX_PROCS) {

        int len = (int)strlen(line);
        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#')
            continue;

        char tmp[MAX_LINE];
        strncpy(tmp, line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *tokens[MAX_ARGS + 3];
        int ntok = 0;
        char *tok = strtok(tmp, " \t");
        while (tok && ntok < MAX_ARGS + 2) {
            tokens[ntok++] = tok;
            tok = strtok(NULL, " \t");
        }

        if (ntok < 3) {
            log_write("WARNING: skipping line (< 3 fields): '%s'", line);
            continue;
        }

        ProcEntry *pe = &procs[nprocs];
        memset(pe, 0, sizeof(*pe));
        strncpy(pe->cmdline, line, sizeof(pe->cmdline) - 1);

        strncpy(pe->stdout_file, tokens[ntok - 1], sizeof(pe->stdout_file) - 1);
        strncpy(pe->stdin_file, tokens[ntok - 2], sizeof(pe->stdin_file) - 1);

        if (tokens[0][0] != '/') {
            log_write("WARNING: program path is not absolute: '%s', skipping",
                      tokens[0]);
            continue;
        }
        if (pe->stdin_file[0] != '/') {
            log_write("WARNING: stdin path is not absolute: '%s', skipping",
                      pe->stdin_file);
            continue;
        }
        if (pe->stdout_file[0] != '/') {
            log_write("WARNING: stdout path is not absolute: '%s', skipping",
                      pe->stdout_file);
            continue;
        }

        int nargs = ntok - 2;
        for (int i = 0; i < nargs && i < MAX_ARGS; i++) {
            pe->argv[i] = strdup(tokens[i]);
            if (!pe->argv[i]) {
                log_write("ERROR: strdup failed");
                free_proc_entry(pe);
                fclose(f);
                return -1;
            }
        }
        pe->argv[nargs < MAX_ARGS ? nargs : MAX_ARGS] = NULL;
        pe->pid = -1;

        nprocs++;
    }

    fclose(f);
    log_write("Config loaded: %d process(es) from '%s'", nprocs, config_path);
    return 0;
}

static pid_t spawn_process(int idx) {
    ProcEntry *pe = &procs[idx];

    pid_t pid = fork();
    if (pid < 0) {
        log_write("ERROR: fork() for proc[%d]: %s", idx, strerror(errno));
        return -1;
    }

    if (pid == 0) {

        int fd_in = open(pe->stdin_file, O_RDONLY);
        if (fd_in < 0)
            fd_in = open("/dev/null", O_RDONLY);
        if (fd_in >= 0) {
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }

        int fd_out = open(pe->stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out < 0)
            fd_out = open("/dev/null", O_WRONLY);
        if (fd_out >= 0) {
            dup2(fd_out, STDOUT_FILENO);
            dup2(fd_out, STDERR_FILENO);
            close(fd_out);
        }

        execv(pe->argv[0], pe->argv);
        _exit(127);
    }

    pe->pid = pid;
    log_write("Started proc[%d] '%s' pid=%d", idx, pe->argv[0], (int)pid);
    return pid;
}

static void handle_sighup(void) {
    log_write("SIGHUP: terminating all child processes");

    for (int i = 0; i < nprocs; i++) {
        if (procs[i].pid > 0) {
            log_write("Terminating proc[%d] '%s' pid=%d",
                      i, procs[i].argv[0], (int)procs[i].pid);
            kill(procs[i].pid, SIGTERM);
        }
    }

    for (int i = 0; i < nprocs; i++) {
        if (procs[i].pid <= 0)
            continue;

        int status;
        pid_t r;
        do {
            r = waitpid(procs[i].pid, &status, 0);
        } while (r < 0 && errno == EINTR);

        if (r == procs[i].pid) {
            if (WIFEXITED(status))
                log_write("proc[%d] pid=%d exited (code=%d) [SIGHUP]",
                          i, (int)procs[i].pid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                log_write("proc[%d] pid=%d killed by signal %d [SIGHUP]",
                          i, (int)procs[i].pid, WTERMSIG(status));
        } else if (r < 0 && errno == ECHILD) {
            log_write("proc[%d] pid=%d was already terminated before SIGHUP",
                      i, (int)procs[i].pid);
        }
        procs[i].pid = -1;
    }

    for (int i = 0; i < nprocs; i++)
        free_proc_entry(&procs[i]);
    nprocs = 0;

    close(log_fd);
    log_fd = open(LOG_FILE, O_CREAT | O_APPEND | O_WRONLY, 0600);

    log_write("SIGHUP: config reloaded, starting new processes");

    if (read_config() == 0) {
        for (int i = 0; i < nprocs; i++)
            spawn_process(i);
    }
}

static void run(void) {
    for (int i = 0; i < nprocs; i++)
        spawn_process(i);

    while (1) {

        if (got_sigterm) {
            log_write("SIGTERM: shutting down myinit");
            for (int i = 0; i < nprocs; i++) {
                if (procs[i].pid > 0) {
                    kill(procs[i].pid, SIGTERM);
                    waitpid(procs[i].pid, NULL, 0);
                    procs[i].pid = -1;
                }
            }
            log_write("myinit exited");
            unlink(PID_FILE);
            exit(0);
        }

        if (got_sighup) {
            got_sighup = 0;
            handle_sighup();
            continue;
        }

        if (nprocs == 0) {
            pause();
            continue;
        }

        int status;
        pid_t cpid = waitpid(-1, &status, 0);

        if (cpid < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ECHILD) {
                sleep(1);
                continue;
            }
            continue;
        }

        for (int i = 0; i < nprocs; i++) {
            if (procs[i].pid != cpid)
                continue;

            if (WIFEXITED(status))
                log_write("proc[%d] '%s' pid=%d exited (code=%d), restart",
                          i, procs[i].argv[0], (int)cpid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                log_write("proc[%d] '%s' pid=%d killed by signal %d, restart",
                          i, procs[i].argv[0], (int)cpid, WTERMSIG(status));
            else
                log_write("proc[%d] '%s' pid=%d stopped, restart",
                          i, procs[i].argv[0], (int)cpid);

            procs[i].pid = -1;
            spawn_process(i);
            break;
        }
    }
}

static void daemonize(void) {

    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    if (getppid() != 1) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid != 0) exit(0);

        if (setsid() < 0) { perror("setsid"); exit(1); }
    }

    struct rlimit flim;
    getrlimit(RLIMIT_NOFILE, &flim);
    long maxfd = (flim.rlim_max == RLIM_INFINITY) ? 4096L
                                                   : (long)flim.rlim_max;
    for (long fd = 0; fd < maxfd; fd++)
        close((int)fd);

    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) _exit(1);
    if (devnull != STDIN_FILENO) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
    }
    dup2(STDIN_FILENO, STDOUT_FILENO);
    dup2(STDIN_FILENO, STDERR_FILENO);

    if (chdir("/") != 0) _exit(1);

    log_fd = open(LOG_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) _exit(1);

    log_write("myinit started, config='%s', pid=%d",
              config_path, (int)getpid());

    int pidf = open(PID_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (pidf >= 0) {
        char pidbuf[32];
        int n = snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
        write(pidf, pidbuf, n);
        close(pidf);
    }
}

int main(int argc, char **argv) {
    char *config_file = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
        case 'c':
            config_file = optarg;
            break;
        default:
            fprintf(stderr, "Usage: %s -c <config_file>\n", argv[0]);
            return 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Usage: %s -c <config_file>\n", argv[0]);
        return 1;
    }

    if (config_file[0] == '/') {
        strncpy(config_path, config_file, sizeof(config_path) - 1);
    } else {
        char cwd[MAX_LINE / 2];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return 1;
        }
        strncpy(config_path, cwd, sizeof(config_path) / 2 - 1);
        strncat(config_path, "/", sizeof(config_path) - strlen(config_path) - 1);
        strncat(config_path, config_file, sizeof(config_path) - strlen(config_path) - 1);
    }
    config_path[sizeof(config_path) - 1] = '\0';

    daemonize();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sa.sa_handler = sighup_handler;
    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        log_write("ERROR: sigaction(SIGHUP): %s", strerror(errno));
        return 1;
    }

    sa.sa_handler = sigterm_handler;
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        log_write("ERROR: sigaction(SIGTERM): %s", strerror(errno));
        return 1;
    }

    if (read_config() != 0) {
        log_write("FATAL: failed to read config");
        return 1;
    }

    run();
    return 0;
}