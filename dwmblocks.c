#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>

#define CMDOUTLENGTH                    50
#define STATUSLENGTH                    256

#define NILL                            INT_MIN
#define LOCKFILE                        "/tmp/dwmblocks.pid"

#define DELIMITERLENGTH                 sizeof delimiter

typedef struct {
        char *const pathu;
        char *const pathc;
        const int interval;
        const int signal;
        char curcmdout[CMDOUTLENGTH + 1];
        char prvcmdout[CMDOUTLENGTH + 1];
} Block;

#include "blocks.h"

static void buttonhandler(int sig, siginfo_t *info, void *ucontext);
static void cleanup();
static void setroot();
static void setupsignals();
static void sighandler(int sig, siginfo_t *si, void *ucontext);
static void statusloop();
static void termhandler(int sig);
static void updateblock(Block *block, int sigval);
static int updatestatus();
static void writepid();

static char statustext[STATUSLENGTH + DELIMITERLENGTH];
static Display *dpy;
static sigset_t blocksigmask;

void
buttonhandler(int sig, siginfo_t *info, void *ucontext)
{
        sig = info->si_value.sival_int >> 8;
        for (Block *block = blocks; block->pathu; block++)
                if (block->signal == sig)
                        switch (fork()) {
                                case -1:
                                        perror("buttonhandler - fork");
                                        break;
                                case 0:
                                {
                                        char button[] = { '0' + (info->si_value.sival_int & 0xff), '\0' };
                                        char *arg[] = { block->pathc, button, NULL };

                                        close(ConnectionNumber(dpy));
                                        setsid();
                                        execv(arg[0], arg);
                                        perror("buttonhandler - child - execv");
                                        _exit(127);
                                }
                        }
}

void
cleanup()
{
        unlink(LOCKFILE);
        XStoreName(dpy, DefaultRootWindow(dpy), "");
        XCloseDisplay(dpy);
}

void
setroot()
{
        if (updatestatus()) {
                XStoreName(dpy, DefaultRootWindow(dpy), statustext);
                XSync(dpy, False);
        }
}

void
setupsignals()
{
        struct sigaction sa;

        /* populate blocksigmask */
        sigemptyset(&blocksigmask);
        sigaddset(&blocksigmask, SIGHUP);
        sigaddset(&blocksigmask, SIGINT);
        sigaddset(&blocksigmask, SIGTERM);
        for (Block *block = blocks; block->pathu; block++)
                if (block->signal > 0)
                        sigaddset(&blocksigmask, SIGRTMIN + block->signal);

        /* setup signal handlers */
        /* to handle HUP, INT and TERM */
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = termhandler;
        sigaction(SIGHUP, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        /* to ignore unused realtime signals */
        // sa.sa_flags = SA_RESTART;
        // sigemptyset(&sa.sa_mask);
        sa.sa_handler = SIG_IGN;
        for (int i = SIGRTMIN + 1; i <= SIGRTMAX; i++)
                sigaction(i, &sa, NULL);

        /* to prevent forked children from becoming zombies */
        sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
        // sigemptyset(&sa.sa_mask);
        sa.sa_handler = SIG_DFL;
        sigaction(SIGCHLD, &sa, NULL);

        /* to handle signals generated by dwm on click events */
        sa.sa_flags = SA_RESTART | SA_SIGINFO;
        // sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = buttonhandler;
        sigaction(SIGRTMIN, &sa, NULL);

        /* to handle update signals for individual blocks */
        sa.sa_flags |= SA_NODEFER;
        sa.sa_mask = blocksigmask;
        sa.sa_sigaction = sighandler;
        for (Block *block = blocks; block->pathu; block++)
                if (block->signal > 0)
                        sigaction(SIGRTMIN + block->signal, &sa, NULL);
}

void
sighandler(int sig, siginfo_t *info, void *ucontext)
{
        sig -= SIGRTMIN;
        for (Block *block = blocks; block->pathu; block++)
                if (block->signal == sig)
                        updateblock(block, info->si_value.sival_int);
        setroot();
}

void
statusloop()
{
        int i;
        struct timespec t;

        /* first run */
        sigprocmask(SIG_BLOCK, &blocksigmask, NULL);
        for (Block *block = blocks; block->pathu; block++)
                if (block->interval >= 0)
                        updateblock(block, NILL);
        setroot();
        sigprocmask(SIG_UNBLOCK, &blocksigmask, NULL);
        t.tv_sec = INTERVALs, t.tv_nsec = INTERVALn;
        while (nanosleep(&t, &t) == -1)
                if (errno != EINTR) {
                        perror("statusloop - nanosleep");
                        exit(1);
                }
        i = 1;
        /* main loop */
        for (;; i++) {
                sigprocmask(SIG_BLOCK, &blocksigmask, NULL);
                for (Block *block = blocks; block->pathu; block++)
                        if (block->interval > 0 && i % block->interval == 0)
                                updateblock(block, NILL);
                setroot();
                sigprocmask(SIG_UNBLOCK, &blocksigmask, NULL);
                t.tv_sec = INTERVALs, t.tv_nsec = INTERVALn;
                while (nanosleep(&t, &t) == -1);
        }
}

void
termhandler(int sig)
{
        cleanup();
        exit(0);
}

void
updateblock(Block *block, int sigval)
{
        int fd[2];

        if (pipe(fd) == -1) {
                perror("updateblock - pipe");
                exit(1);
        }
        switch (fork()) {
                case -1:
                        perror("updateblock - fork");
                        exit(1);
                case 0:
                        close(ConnectionNumber(dpy));
                        close(fd[0]);
                        if (fd[1] != STDOUT_FILENO) {
                                if (dup2(fd[1], STDOUT_FILENO) != STDOUT_FILENO) {
                                        perror("updateblock - child - dup2");
                                        exit(1);
                                }
                                close(fd[1]);
                        }
                        if (sigval == NILL) {
                                char *arg[] = { block->pathu, NULL };

                                execv(arg[0], arg);
                        } else {
                                char buf[12];
                                char *arg[] = { block->pathu, buf, NULL };

                                snprintf(buf, sizeof buf, "%d", sigval);
                                execv(arg[0], arg);
                        }
                        perror("updateblock - child - execv");
                        _exit(127);
                default:
                {
                        size_t trd = 0;
                        ssize_t rd;

                        close(fd[1]);
                        do
                                rd = read(fd[0], block->curcmdout + trd, CMDOUTLENGTH - trd);
                        while (rd > 0 && (trd += rd) < CMDOUTLENGTH);
                        if (rd == -1) {
                                perror("updateblock - read");
                                exit(1);
                        }
                        close(fd[0]);
                }
        }
}

/* returns whether block outputs have changed and updates statustext if they have */
int
updatestatus()
{
        char *s = statustext;
        char *c, *p;
        Block *block = blocks;

        /* checking half of the function */
        for (; block->pathu; block++) {
                c = block->curcmdout, p = block->prvcmdout;
                for (; *c == *p && *c != '\n' && *c != '\0'; c++, p++);
                s += c - block->curcmdout;
                if (*c != *p)
                        goto update;
                if (c == block->curcmdout)
                        continue;
                if (block->pathc /* && block->signal */)
                        s++;
                s += DELIMITERLENGTH;
        }
        return 0;

        /* updating half of the function */
        for (; block->pathu; block++) {
                c = block->curcmdout, p = block->prvcmdout;
update:
                for (; *p = *c, *c != '\n' && *c != '\0'; c++, p++)
                        *(s++) = *c;
                if (c == block->curcmdout)
                        continue;
                if (block->pathc /* && block->signal */)
                        *(s++) = block->signal;
                memcpy(s, delimiter, DELIMITERLENGTH);
                s += DELIMITERLENGTH;
        }
        if (s != statustext)
                s -= DELIMITERLENGTH;
        *s = '\0';
        return 1;
}

void
writepid()
{
        int fd;
        struct flock fl;

        if ((fd = open(LOCKFILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
                perror("writepid - open");
                exit(1);
        }
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;
        if (fcntl(fd, F_SETLK, &fl) == -1) {
                if (errno == EACCES || errno == EAGAIN) {
                        fputs("Error: another instance of dwmblocks is already running.\n", stderr);
                        exit(2);
                }
                perror("writepid - fcntl");
                exit(1);
        }
        if (ftruncate(fd, 0) == -1) {
                perror("writepid - ftruncate");
                exit(1);
        }
        if (dprintf(fd, "%ld", (long)getpid()) < 0) {
                perror("writepid - dprintf");
                exit(1);
        }
}

int
main(int argc, char *argv[])
{
        if (!(dpy = XOpenDisplay(NULL))) {
                fputs("Error: could not open display.\n", stderr);
                return 1;
        }
        writepid();
        setupsignals();
        statusloop();
        cleanup();
        return 0;
}
