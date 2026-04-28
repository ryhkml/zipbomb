#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 32000
#define DEFAULT_WORKERS 4
#define DEFAULT_SEND_TIMEOUT 5
#define BACKLOG 10
#define CHUNK_SIZE 4096
#define BOMB_FILE "data.gzip"
#define MAX_WORKERS 128

typedef struct {
    const char *host;
    const char *bomb_path;
    uint16_t port;
    int workers;
    int send_timeout;
} server_config_t;

static volatile sig_atomic_t running = true;

void handle_signal(int signal);
void handle_connection(int client_fd, const char *bomb_path);
ssize_t write_all(int fd, const void *buf, size_t count);
void print_usage(const char *name);

static bool parse_long_arg(const char *value, long min, long max, const char *name, long *out);
static char *resolve_payload_path(const char *path);
static bool validate_payload_file(const char *path);
static bool configure_signal_handlers(void);
static bool set_send_timeout(int fd, int seconds);
static int create_server_socket(const server_config_t *config);
static void worker_loop(int server_fd, const server_config_t *config);
static pid_t spawn_worker(int server_fd, const server_config_t *config);
static int find_worker_slot(const pid_t *pids, int workers, pid_t pid);
static void terminate_workers(pid_t *pids, int workers);
static void wait_for_workers(pid_t *pids, int workers);

int main(int argc, char **argv) {
    const char *payload_arg = BOMB_FILE;
    server_config_t config = {
        .host = DEFAULT_HOST,
        .bomb_path = NULL,
        .port = DEFAULT_PORT,
        .workers = DEFAULT_WORKERS,
        .send_timeout = DEFAULT_SEND_TIMEOUT,
    };

    struct option long_options[] = {
        {"host",         required_argument, NULL, 'h'},
        {"port",         required_argument, NULL, 'p'},
        {"file",         required_argument, NULL, 'f'},
        {"workers",      required_argument, NULL, 'w'},
        {"send-timeout", required_argument, NULL, 't'},
        {0,              0,                 0,    0  }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:f:w:t:", long_options, NULL)) != -1) {
        long parsed;

        switch (opt) {
            case 'h':
                config.host = optarg;
                break;
            case 'p':
                if (!parse_long_arg(optarg, 4000, 65535, "port", &parsed)) return EXIT_FAILURE;
                config.port = (uint16_t)parsed;
                break;
            case 'f':
                payload_arg = optarg;
                break;
            case 'w':
                if (!parse_long_arg(optarg, 1, MAX_WORKERS, "workers", &parsed)) return EXIT_FAILURE;
                config.workers = (int)parsed;
                break;
            case 't':
                if (!parse_long_arg(optarg, 1, 300, "send timeout", &parsed)) return EXIT_FAILURE;
                config.send_timeout = (int)parsed;
                break;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    char *resolved_payload_path = resolve_payload_path(payload_arg);
    if (resolved_payload_path == NULL) return EXIT_FAILURE;
    config.bomb_path = resolved_payload_path;

    if (!validate_payload_file(config.bomb_path)) {
        free(resolved_payload_path);
        return EXIT_FAILURE;
    }

    if (!configure_signal_handlers()) {
        free(resolved_payload_path);
        return EXIT_FAILURE;
    }

    int server_fd = create_server_socket(&config);
    if (server_fd == -1) {
        free(resolved_payload_path);
        return EXIT_FAILURE;
    }

    pid_t *pids = calloc((size_t)config.workers, sizeof(*pids));
    if (pids == NULL) {
        perror("Failed to allocate worker table");
        close(server_fd);
        free(resolved_payload_path);
        return EXIT_FAILURE;
    }

    printf("Server listening on http://%s:%d with %d workers\n", config.host, config.port, config.workers);
    fflush(NULL);

    int live_workers = 0;
    for (int i = 0; i < config.workers; i++) {
        pids[i] = spawn_worker(server_fd, &config);
        if (pids[i] == -1) {
            terminate_workers(pids, config.workers);
            wait_for_workers(pids, config.workers);
            close(server_fd);
            free(pids);
            free(resolved_payload_path);
            return EXIT_FAILURE;
        }
        live_workers++;
    }

    while (running && live_workers > 0) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid == -1) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            perror("Failed waiting for worker");
            continue;
        }

        int slot = find_worker_slot(pids, config.workers, pid);
        if (slot == -1) continue;

        pids[slot] = 0;
        live_workers--;

        if (!running) continue;

        pid_t replacement = spawn_worker(server_fd, &config);
        if (replacement == -1) {
            fprintf(stderr, "Worker %ld exited and could not be replaced\n", (long)pid);
            continue;
        }

        pids[slot] = replacement;
        live_workers++;
        fprintf(stderr, "Worker %ld exited; spawned replacement %ld\n", (long)pid, (long)replacement);
    }

    terminate_workers(pids, config.workers);
    wait_for_workers(pids, config.workers);

    close(server_fd);
    free(pids);
    free(resolved_payload_path);
    return EXIT_SUCCESS;
}

void handle_signal(int signal) {
    (void)signal;
    running = false;
}

void handle_connection(int client_fd, const char *bomb_path) {
    int bomb_fd = open(bomb_path, O_RDONLY);
    if (bomb_fd == -1) {
        perror("Failed to open bomb file");
        const char *err_500 = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
        write_all(client_fd, err_500, strlen(err_500));
        return;
    }

    struct stat file_stat;
    if (fstat(bomb_fd, &file_stat) == -1) {
        perror("Failed to stat bomb file");
        close(bomb_fd);
        return;
    }

    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                              "Content-Encoding: gzip\r\n"
                              "Content-Type: application/octet-stream\r\n"
                              "Content-Length: %jd\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              (intmax_t)file_stat.st_size);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        fprintf(stderr, "Failed to build response header\n");
        close(bomb_fd);
        return;
    }

    if (write_all(client_fd, header, (size_t)header_len) == -1) {
        close(bomb_fd);
        return;
    }

    printf("[BOMB DEPLOYED]\n");

    char buffer[CHUNK_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(bomb_fd, buffer, sizeof(buffer))) > 0) {
        if (write_all(client_fd, buffer, (size_t)bytes_read) == -1) break;
    }

    if (bytes_read == -1) {
        perror("Failed to read from file");
    }

    close(bomb_fd);
}

ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t bytes_written = 0;
    const char *p = buf;

    while (bytes_written < count) {
        ssize_t result = write(fd, p + bytes_written, count - bytes_written);
        if (result == -1) {
            if (errno == EINTR) continue;
            if (errno == EPIPE || errno == ECONNRESET || errno == EAGAIN || errno == EWOULDBLOCK) return -1;
            perror("Failed to write");
            return -1;
        }
        if (result == 0) {
            errno = EPIPE;
            return -1;
        }
        bytes_written += (size_t)result;
    }

    return (ssize_t)bytes_written;
}

void print_usage(const char *name) {
    fprintf(stderr,
            "Usage: %s [--host <addr>] [--port <4000-65535>] [--file <path>] "
            "[--workers <1-%d>] [--send-timeout <1-300>]\n",
            name, MAX_WORKERS);
}

static bool parse_long_arg(const char *value, long min, long max, const char *name, long *out) {
    char *endptr;

    errno = 0;
    long parsed = strtol(value, &endptr, 10);
    if (errno != 0 || value[0] == '\0' || *endptr != '\0' || parsed < min || parsed > max) {
        fprintf(stderr, "Error: invalid %s %s\n", name, value);
        return false;
    }

    *out = parsed;
    return true;
}

static char *resolve_payload_path(const char *path) {
    char *resolved = realpath(path, NULL);
    if (resolved == NULL) {
        fprintf(stderr, "Error: failed to resolve payload path %s: %s\n", path, strerror(errno));
    }
    return resolved;
}

static bool validate_payload_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Error: failed to open payload %s: %s\n", path, strerror(errno));
        return false;
    }

    struct stat file_stat;
    if (fstat(fd, &file_stat) == -1) {
        fprintf(stderr, "Error: failed to stat payload %s: %s\n", path, strerror(errno));
        close(fd);
        return false;
    }

    close(fd);

    if (!S_ISREG(file_stat.st_mode)) {
        fprintf(stderr, "Error: payload is not a regular file: %s\n", path);
        return false;
    }

    if (file_stat.st_size <= 0) {
        fprintf(stderr, "Error: payload is empty: %s\n", path);
        return false;
    }

    return true;
}

static bool configure_signal_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("Failed to set signal handlers");
        return false;
    }

    struct sigaction ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, NULL) == -1) {
        perror("Failed to ignore SIGPIPE");
        return false;
    }

    return true;
}

static bool set_send_timeout(int fd, int seconds) {
    struct timeval timeout = {
        .tv_sec = seconds,
        .tv_usec = 0,
    };

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("Failed to set send timeout");
        return false;
    }

    return true;
}

static int create_server_socket(const server_config_t *config) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Failed to create socket");
        return -1;
    }

    int opt_val = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val)) == -1) {
        perror("Failed to set setsockopt");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(config->port);
    if (inet_pton(AF_INET, config->host, &address.sin_addr) <= 0) {
        fprintf(stderr, "Error: invalid host %s\n", config->host);
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("Failed to bind socket");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, BACKLOG) == -1) {
        perror("Failed to listen on socket");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

static void worker_loop(int server_fd, const server_config_t *config) {
    while (running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR) continue;
            if (errno == ECONNABORTED) continue;
            perror("Failed to accept connection");
            continue;
        }

        if (!set_send_timeout(client_fd, config->send_timeout)) {
            close(client_fd);
            continue;
        }

        handle_connection(client_fd, config->bomb_path);
        close(client_fd);
    }
}

static pid_t spawn_worker(int server_fd, const server_config_t *config) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("Failed to fork worker");
        return -1;
    }

    if (pid == 0) {
        worker_loop(server_fd, config);
        close(server_fd);
        _exit(EXIT_SUCCESS);
    }

    return pid;
}

static int find_worker_slot(const pid_t *pids, int workers, pid_t pid) {
    for (int i = 0; i < workers; i++) {
        if (pids[i] == pid) return i;
    }

    return -1;
}

static void terminate_workers(pid_t *pids, int workers) {
    running = false;

    for (int i = 0; i < workers; i++) {
        if (pids[i] > 0) kill(pids[i], SIGTERM);
    }
}

static void wait_for_workers(pid_t *pids, int workers) {
    for (int i = 0; i < workers; i++) {
        while (pids[i] > 0) {
            pid_t result = waitpid(pids[i], NULL, 0);
            if (result == -1) {
                if (errno == EINTR) continue;
                if (errno == ECHILD) break;
                perror("Failed waiting for worker shutdown");
                break;
            }
            break;
        }
        pids[i] = 0;
    }
}
