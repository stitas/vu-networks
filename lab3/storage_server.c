#include "net_utils.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>

#define FILE_NAME "strings.txt"

static void handle_put(char *msg) {
    FILE *f = fopen(FILE_NAME, "ab");
    if (!f) {
        return;
    }

    fwrite(msg + 4, 1, strlen(msg + 4), f); // +4 because skip "PUT"
    fclose(f);
}

static void handle_get(int client) {
    FILE *f = fopen(FILE_NAME, "rb");
    char buffer[BUFFER_SIZE];

    if (!f) {
        send_all(client, "SIZE 0\n", 7);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        send_all(client, "SIZE 0\n", 7);
        return;
    }

    char header[64];
    int header_len = snprintf(header, sizeof(header), "SIZE %ld\n", size);
    send_all(client, header, header_len);

    while (1) {
        size_t n = fread(buffer, 1, sizeof(buffer), f);
        if (n == 0) {
            break;
        }

        send_all(client, buffer, (int)n);
    }

    fclose(f);
}

int main(int argc, char **argv) {
    short port = (short)strtol(argv[1], NULL, 10);
    int server = create_server_socket(port);

    while (1) {
        int client = accept(server, NULL, NULL);
        char buffer[BUFFER_SIZE];

        while (read_line(client, buffer, BUFFER_SIZE) > 0) {
            if (strncmp(buffer, "PUT ", 4) == 0) {
                handle_put(buffer);
            } else if (strncmp(buffer, "GET", 3) == 0) {
                handle_get(client);
            }
        }

        close(client);
    }

    close(server);
    return 0;
}