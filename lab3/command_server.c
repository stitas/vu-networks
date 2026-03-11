#include "net_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

static void forward_to_storage(int storage, int client, char *cmd) {
    send_all(storage, cmd, strlen(cmd));

    if (strncmp(cmd, "GET", 3) == 0) {
        char header[BUFFER_SIZE];

        if (read_line(storage, header, BUFFER_SIZE) <= 0) {
            return;
        }

        long file_size = 0;
        if (sscanf(header, "SIZE %ld\n", &file_size) != 1 || file_size < 0) {
            return;
        }

        char buffer[BUFFER_SIZE];
        long remaining = file_size;

        while (remaining > 0) {
            int chunk = (remaining < BUFFER_SIZE) ? (int)remaining : BUFFER_SIZE;
            int n = recv(storage, buffer, chunk, 0);

            // Error
            if (n <= 0) {
                return; 
            }

            send_all(client, buffer, n);
            remaining -= n;
        }
    }
}

int main(int argc, char **argv) {
    short port1 = (short)strtol(argv[1], NULL, 10);
    short port2 = port1 + 2;

    int server = create_server_socket(port1);

    while (1) {
        int client = accept(server, NULL, NULL);
        int storage = connect_to_server("::1", port2);
        char buffer[BUFFER_SIZE];

        while (read_line(client, buffer, BUFFER_SIZE) > 0) {
            forward_to_storage(storage, client, buffer);
        }

        close(storage);
        close(client);
    }

    close(server);
    return 0;
}