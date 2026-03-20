/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_PTT_SOCKET "/tmp/udpptt.sock"

static int send_command(const char *socket_path, const char *cmd) {
    int fd = -1;
    struct sockaddr_un addr;
    ssize_t n;

    if (!socket_path || !socket_path[0] || !cmd || !cmd[0]) {
        fprintf(stderr, "invalid socket path or command\n");
        return 1;
    }

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", socket_path);
        return 1;
    }

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(AF_UNIX)");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    n = sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr *)&addr, sizeof(addr));
    if (n < 0) {
        perror("sendto(AF_UNIX)");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--socket PATH] (--ptt_down | --ptt_up | --toggle)\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *socket_path = DEFAULT_PTT_SOCKET;
    const char *cmd = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for --socket\n");
                usage(argv[0]);
                return 1;
            }
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--ptt_down") == 0) {
            cmd = "DOWN";
        } else if (strcmp(argv[i], "--ptt_up") == 0) {
            cmd = "UP";
        } else if (strcmp(argv[i], "--toggle") == 0) {
            cmd = "TOGGLE";
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!cmd) {
        usage(argv[0]);
        return 1;
    }

    return send_command(socket_path, cmd);
}
