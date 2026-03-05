#include "net_utils.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>

#define FILE_NAME "strings.txt"

void handle_put(char *msg) {
    FILE *f = fopen(FILE_NAME, "a");
    if (!f) return;

    fprintf(f, "%s", msg + 4); // +4 nes "PUT " nesiskaito
    fclose(f);
}

void handle_get(int client) {
    FILE *f = fopen(FILE_NAME, "r");
    char buffer[BUFFER_SIZE];

    if (!f) return;

    while (fgets(buffer, BUFFER_SIZE, f)) {
        send_all(client, buffer, strlen(buffer));
    }

    fclose(f);
}

int main(int argc, char **argv) {

    short port = (short)strtol(argv[1],NULL,10);
    int server = create_server_socket(port);

    while (1) {
        int client = accept(server,NULL,NULL);

        char buffer[BUFFER_SIZE];

        while (read_line(client, buffer, BUFFER_SIZE) > 0) {

            if (strncmp(buffer,"PUT ",4) == 0)
                handle_put(buffer);

            else if (strncmp(buffer,"GET",3) == 0)
                handle_get(client);
        }

        close(client);
    }
}