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
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 32000
#define BACKLOG 10
#define CHUNK_SIZE 4096
#define BOMB_FILE "data.gzip"

volatile sig_atomic_t running = true;

void handle_connection(int client_fd);
void handle_sigact(int signal);
ssize_t write_all(int fd, const void *buf, size_t count);
void print_usage(const char *name);

int main(int argc, char **argv) {
    uint16_t port = DEFAULT_PORT;
    const char *host = DEFAULT_HOST;

    struct option long_options[] = {
        {"host", required_argument, NULL, 'h'},
        {"port", required_argument, NULL, 'p'},
        {0,      0,                 0,    0  }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                host = optarg;
                break;
            case 'p': {
                char *endptr;
                errno = 0;
                long p = strtol(optarg, &endptr, 10);
                if (errno != 0 || *endptr != '\0' || p < 4000 || p > 65535) {
                    fprintf(stderr, "Error: invalid port %s\n", optarg);
                    return EXIT_FAILURE;
                }
                port = (uint16_t)p;
                break;
            }
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigact;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("Failed to set signal handlers");
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Failed to create socket");
        return EXIT_FAILURE;
    }

    int opt_val = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val)) == -1) {
        perror("Failed to set setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) <= 0) {
        fprintf(stderr, "Error: invalid host %s\n", host);
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("Failed to bind socket");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, BACKLOG) == -1) {
        perror("Failed to listen on socket");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on http://%s:%d\n", host, port);

    while (running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR) continue;
            perror("Failed to accept connection");
            continue;
        }

        if (!running) {
            close(client_fd);
            break;
        }

        handle_connection(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}

void handle_sigact(int signal) {
    (void)signal;
    running = false;
}

void handle_connection(int client_fd) {
    int bomb_fd = open(BOMB_FILE, O_RDONLY);
    if (bomb_fd == -1) {
        perror("Failed to open bomb file");
        const char *err_500 = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
        write_all(client_fd, err_500, strlen(err_500));
        return;
    }

    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Content-Encoding: gzip\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "X-Readme: Cry motherfucker\r\n"
        "\r\n";

    if (write_all(client_fd, header, strlen(header)) == -1) {
        close(bomb_fd);
        return;
    }

    printf("[BOMB DEPLOYED]\n");

    char buffer[CHUNK_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(bomb_fd, buffer, sizeof(buffer))) > 0) {
        if (write_all(client_fd, buffer, bytes_read) == -1) break;
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
            perror("Failed to write");
            return -1;
        }
        bytes_written += result;
    }

    return bytes_written;
}

void print_usage(const char *name) { fprintf(stderr, "Usage: %s [--host <addr>] [--port <4000-65535>]\n", name); }
