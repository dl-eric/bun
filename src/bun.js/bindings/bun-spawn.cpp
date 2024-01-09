#include "root.h"

#if OS(LINUX)

#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/fcntl.h>
#include <sys/signal.h>
#include <sys/syscall.h>
#include <sys/resource.h>

static int close_range(unsigned int first)
{
    return syscall(__NR_close_range, first, ~0U, 0);
}

extern char** environ;

enum FileActionType : uint8_t {
    None,
    Close,
    Dup2,
    Open,
};

typedef struct bun_spawn_request_file_action_t {
    FileActionType type;
    const char* path;
    int fds[2];
    int flags;
    int mode;
} bun_spawn_request_file_action_t;

typedef struct bun_spawn_file_action_list_t {
    const bun_spawn_request_file_action_t* ptr;
    size_t len;
} bun_spawn_file_action_list_t;

typedef struct bun_spawn_request_t {
    const char* chdir;
    bool detached;
    bun_spawn_file_action_list_t actions;
} bun_spawn_request_t;

extern "C" ssize_t posix_spawn_bun(
    int* pid,
    const bun_spawn_request_t* request,
    char* const argv[],
    char* const envp[])
{
    volatile int status = 0;
    sigset_t blockall, oldmask;
    int res = 0, cs = 0, e = errno;
    sigfillset(&blockall);
    sigprocmask(SIG_SETMASK, &blockall, &oldmask);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
    const char* path = argv[0];
    pid_t child = vfork();

    const auto parentFailed = [&]() -> ssize_t {
        sigprocmask(SIG_SETMASK, &oldmask, 0);
        pthread_setcancelstate(cs, 0);
        errno = e;
        return res;
    };

    const auto childFailed = [&]() -> ssize_t {
        res = errno;
        status = res;
        close_range(0);
        _exit(127);

        // should never be reached
        return -1;
    };

    const auto startChild = [&]() -> ssize_t {
        sigset_t childmask = oldmask;

        // Reset signals
        struct sigaction sa = { 0 };
        sa.sa_handler = SIG_DFL;
        for (int i = 0; i < NSIG; i++) {
            sigaction(i, &sa, 0);
        }

        // Make "detached" work
        if (request->detached) {
            setsid();
        }

        int current_max_fd = 0;

        if (request->chdir) {
            if (chdir(request->chdir) != 0) {
                return childFailed();
            }
        }

        const auto& actions = request->actions;

        for (size_t i = 0; i < actions.len; i++) {
            const bun_spawn_request_file_action_t& action = actions.ptr[i];
            switch (action.type) {
            case FileActionType::Close: {
                close(action.fds[0]);
                break;
            }
            case FileActionType::Dup2: {
                // Even if the file descrtiptors are the same, we still need to
                // call dup2() because it will reset the close-on-exec flag.
                if (dup2(action.fds[0], action.fds[1]) == -1) {
                    return childFailed();
                }

                // Only close it if it's not the same as the other fd.
                if (action.fds[0] != action.fds[1]) {
                    close(action.fds[0]);
                }

                current_max_fd = std::max(current_max_fd, action.fds[1]);
                break;
            }
            case FileActionType::Open: {
                int opened = -1;
                opened = open(action.path, action.flags, action.mode);

                if (opened == -1) {
                    return childFailed();
                }

                if (opened != -1) {
                    if (dup2(opened, action.fds[0]) == -1) {
                        close(opened);
                        return childFailed();
                    }
                    current_max_fd = std::max(current_max_fd, action.fds[0]);
                    if (close(opened)) {
                        return childFailed();
                    }
                }

                break;
            }
            default: {
                __builtin_unreachable();
                break;
            }
            }
        }

        sigprocmask(SIG_SETMASK, &childmask, 0);
        if (!envp)
            envp = environ;

        close_range(current_max_fd + 1);
        execve(path, argv, envp);
        _exit(127);

        // should never be reached.
        return -1;
    };

    if (child == 0) {
        return startChild();
    }

    if (child != -1) {
        res = status;

        if (!res) {
            if (pid) {
                *pid = child;
            }
        } else {
            wait4(child, 0, 0, 0);
        }
    } else {
        res = errno;
    }

    sigprocmask(SIG_SETMASK, &oldmask, 0);
    pthread_setcancelstate(cs, 0);

    return res;
}

#endif