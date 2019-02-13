#include <lib/types.h>
#include <lib/lock.h>
#include <lib/event.h>
#include <lib/klib.h>
#include <lib/alloc.h>
#include <sys/panic.h>
#include <fd/fd.h>
#include <sys/urm.h>

// Macros from mlibc: options/posix/include/sys/wait.h
#define WSIGNALLED 0x400
#define WEXITED 0x200

#define USER_REQUEST_EXECVE 1
#define USER_REQUEST_EXIT 2

struct userspace_request_t {
    int type;
    void *opaque_data;
};

static size_t userspace_request_i = 0;
static struct userspace_request_t *userspace_requests = 0;
static lock_t userspace_request_lock = new_lock;
static event_t userspace_event;

static void userspace_send_request(int type, void *opaque_data) {
    spinlock_acquire(&userspace_request_lock);

    userspace_request_i++;
    userspace_requests = krealloc(userspace_requests,
        sizeof(struct userspace_request_t) * userspace_request_i);

    struct userspace_request_t *userspace_request =
                    &userspace_requests[userspace_request_i - 1];

    userspace_request->type = type;
    userspace_request->opaque_data = opaque_data;

    event_trigger(&userspace_event);

    spinlock_release(&userspace_request_lock);
}

struct execve_request_t {
    pid_t pid;
    char *filename;
    char **argv;
    char **envp;
    lock_t *err_lock;
    int *err;
};

void execve_send_request(pid_t pid, const char *filename, const char **argv, const char **envp,
                    lock_t **err_lock, int **err) {
    struct execve_request_t *execve_request = kalloc(sizeof(struct execve_request_t));

    execve_request->pid = pid;

    execve_request->filename = kalloc(kstrlen(filename) + 1);
    kstrcpy(execve_request->filename, filename);

    size_t argv_i;
    for (argv_i = 0; argv[argv_i]; argv_i++);
    argv_i++;
    execve_request->argv = kalloc(sizeof(char *) * argv_i);
    for (size_t i = 0; ; i++) {
        if (!argv[i]) {
            execve_request->argv[i] = 0;
            break;
        }
        execve_request->argv[i] = kalloc(kstrlen(argv[i]) + 1);
        kstrcpy(execve_request->argv[i], argv[i]);
    }

    size_t envp_i;
    for (envp_i = 0; envp[envp_i]; envp_i++);
    envp_i++;
    execve_request->envp = kalloc(sizeof(char *) * envp_i);
    for (size_t i = 0; ; i++) {
        if (!envp[i]) {
            execve_request->envp[i] = 0;
            break;
        }
        execve_request->envp[i] = kalloc(kstrlen(envp[i]) + 1);
        kstrcpy(execve_request->envp[i], envp[i]);
    }

    execve_request->err_lock = kalloc(sizeof(lock_t));
    *(execve_request->err_lock) = new_lock;
    execve_request->err = kalloc(sizeof(int));
    *(execve_request->err) = 0;

    *err_lock = execve_request->err_lock;
    *err = execve_request->err;

    userspace_send_request(USER_REQUEST_EXECVE, execve_request);
}

static void execve_receive_request(struct execve_request_t *execve_request) {
    int ret = exec(
        execve_request->pid,
        execve_request->filename,
        (const char **)execve_request->argv,
        (const char **)execve_request->envp
    );

    /* free request mem */
    kfree(execve_request->filename);

    for (size_t i = 0; ; i++) {
        if (!execve_request->argv[i])
            break;
        kfree(execve_request->argv[i]);
    }
    kfree(execve_request->argv);

    for (size_t i = 0; ; i++) {
        if (!execve_request->envp[i])
            break;
        kfree(execve_request->envp[i]);
    }
    kfree(execve_request->envp);

    if (ret) {
        spinlock_acquire(execve_request->err_lock);
        *(execve_request->err) = 1;
        spinlock_release(execve_request->err_lock);
    } else {
        kfree((void *)execve_request->err_lock);
        kfree(execve_request->err);
    }

    kfree(execve_request);
}

struct exit_request_t {
    pid_t pid;
    int signalled;
    int exit_code;
};

void exit_send_request(pid_t pid, int exit_code, int signalled) {
    struct exit_request_t *exit_request = kalloc(sizeof(struct exit_request_t));

    exit_request->pid = pid;
    exit_request->exit_code = exit_code;
    exit_request->signalled = signalled;

    userspace_send_request(USER_REQUEST_EXIT, exit_request);
}

static void exit_receive_request(struct exit_request_t *exit_request) {
    struct process_t *process = process_table[exit_request->pid];

    if (!process->ppid)
        panic("Going nowhere without my init!", 0, 0);

    /* Kill all associated threads */
    for (size_t i = 0; i < MAX_THREADS; i++)
        task_tkill(exit_request->pid, i);

    /* Close all file handles */
    for (size_t i = 0; i < MAX_FILE_HANDLES; i++) {
        if (process->file_handles[i] == -1)
            continue;
        close(process->file_handles[i]);
    }
    kfree(process->file_handles);

    if (process->child_events)
        kfree(process->child_events);

    free_address_space(process->pagemap);

    if (process->active_perfmon)
        perfmon_unref(process->active_perfmon);

    struct child_event_t child_event;

    child_event.pid = exit_request->pid;
    child_event.status = 0;
    child_event.status |= (uint8_t)(exit_request->exit_code & 0xff);
    child_event.status |= WEXITED;

    if (exit_request->signalled)
        child_event.status |= WSIGNALLED;

    task_send_child_event(process->ppid, &child_event);

    kfree(exit_request);
}

void userspace_request_monitor(void *arg) {
    (void)arg;

    kprint(KPRN_INFO, "urm: Userspace request monitor launched.");

    /* main event loop */
    for (;;) {
        spinlock_acquire(&userspace_request_lock);
        if (userspace_request_i) {
            switch (userspace_requests[0].type) {
                case USER_REQUEST_EXECVE:
                    kprint(KPRN_INFO, "urm: execve request received");
                    execve_receive_request(userspace_requests[0].opaque_data);
                    break;
                case USER_REQUEST_EXIT:
                    kprint(KPRN_INFO, "urm: exit request received");
                    exit_receive_request(userspace_requests[0].opaque_data);
                    break;
                default:
                    kprint(KPRN_ERR, "urm: Invalid request received");
                    break;
            }
            userspace_request_i--;
            for (size_t i = 0; i < userspace_request_i; i++)
                userspace_requests[i] = userspace_requests[i + 1];
            userspace_requests = krealloc(userspace_requests,
                sizeof(struct userspace_request_t) * userspace_request_i);
        }
        spinlock_release(&userspace_request_lock);
        event_await(&userspace_event);
    }
}
