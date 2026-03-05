#include "net_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int create_server_socket(short port) {
    int server_fd;
    struct sockaddr_in6 addr;

    if ((server_fd = socket(AF_INET6, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) == -1) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

int connect_to_server(const char *ip, short port) {
    int sock;
    struct sockaddr_in6 addr;

    if ((sock = socket(AF_INET6, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&addr,0,sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    inet_pton(AF_INET6, ip, &addr.sin6_addr);

    if (connect(sock,(struct sockaddr*)&addr,sizeof(addr)) == -1) {
        perror("connect");
        close(sock);
        exit(EXIT_FAILURE);
    }

    return sock;
}

int send_all(int sock, const char *buf, int len) {
    int total = 0;

    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }

    return total;
}

int read_line(int sock, char *buf, int max) {
    int i = 0;
    char c;

    while (i < max-1) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) break;

        buf[i++] = c;
        if (c == '\n') break;
    }

    buf[i] = '\0';
    return i;
}