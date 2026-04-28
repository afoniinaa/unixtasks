#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define LOCK_SUFFIX ".lck"
#define STAT_FILE   "stats.txt"

volatile sig_atomic_t running = 1;

char lockfile[256];
char datafile[256];

int has_lock  = 0;
int lock_count = 0;

void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, buf + total, len - total);
        if (n <= 0) return 0;
        total += (size_t)n;
    }
    return 1;
}

int create_lock(void) {
    int fd = open(lockfile, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd == -1) return 0;

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (len < 0 || !write_all(fd, buf, (size_t)len)) {
        close(fd);
        unlink(lockfile);
        return 0;
    }
    close(fd);
    has_lock = 1;
    return 1;
}

int check_our_lock(void) {
    int fd = open(lockfile, O_RDONLY);
    if (fd == -1) return 0;

    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    return atoi(buf) == getpid();
}

void remove_lock(void) {
    if (has_lock && check_our_lock()) {
        unlink(lockfile);
    }
    has_lock = 0;
}

void save_stats(void) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "PID %d: %d\n", getpid(), lock_count);
    if (len <= 0) return;

    int fd = open(STAT_FILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd == -1) return;
    (void)write_all(fd, buf, (size_t)len);
    close(fd);
}

void do_file_work(void) {
    int fd = open(datafile, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd != -1) {
        char wbuf[64];
        int len = snprintf(wbuf, sizeof(wbuf), "PID %d lock %d\n",
                           getpid(), lock_count);
        if (len > 0) (void)write_all(fd, wbuf, (size_t)len);
        close(fd);
    }

    fd = open(datafile, O_RDONLY);
    if (fd != -1) {
        char rbuf[256];
        ssize_t n;
        while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
            ;
        close(fd);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }

    snprintf(datafile, sizeof(datafile), "%s", argv[1]);
    snprintf(lockfile, sizeof(lockfile), "%s%s", argv[1], LOCK_SUFFIX);

    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    srand((unsigned)(getpid() ^ time(NULL)));

    while (running) {
        while (!create_lock() && running) {
            usleep(10000 + rand() % 10000);
        }
        if (!running) break;

        lock_count++;
        printf("[PID %d] lock acquired (count=%d)\n", getpid(), lock_count);
        fflush(stdout);

        do_file_work();

        sleep(1);

        if (check_our_lock()) {
            remove_lock();
            printf("[PID %d] lock released\n", getpid());
            fflush(stdout);
        } else {
            fprintf(stderr, "[PID %d] ERROR: lock ownership lost\n", getpid());
            break;
        }

        usleep(10000 + rand() % 10000);
    }

    remove_lock();
    save_stats();
    return 0;
}