#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define BUFFER_SIZE 1024

int main(int argc, char** argv) {
    const char* host = argv[1];
    const char* port = argv[2];

    struct addrinfo hints, *res, *p;
    int client_fd;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        perror("getaddrinfo failed");
        exit(EXIT_FAILURE);
    }

    // Try each address until we connect
    for (p = res; p != NULL; p = p->ai_next) {
        client_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (client_fd == -1)
            continue;

        if (connect(client_fd, p->ai_addr, p->ai_addrlen) == 0)
            break;

        close(client_fd);
    }

    if (p == NULL) {
        fprintf(stderr, "Failed to connect to server\n");
        freeaddrinfo(res);
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(res);

    printf("Connected to server.\n");
    printf("Send message (type exit to exit)\n");

    char *line = NULL;
    size_t lineSize = 0;

    while (1) {
        ssize_t characters = getline(&line, &lineSize, stdin);
        if (characters <= 0)
            break;

        if (strcmp(line, "exit\n") == 0)
            break;

        if (send(client_fd, line, characters, 0) == -1) {
            perror("Send failed");
            break;
        }

        char buffer[BUFFER_SIZE];
        ssize_t bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received == -1) {
            perror("Receive failed");
            break;
        }

        if (bytes_received == 0) {
            printf("Server closed the connection.\n");
            break;
        }

        buffer[bytes_received] = '\0';
        printf("Server response: %s\n", buffer);
    }

    free(line);
    close(client_fd);

    return 0;
}