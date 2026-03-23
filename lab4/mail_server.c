#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BACKLOG 20
#define MAX_LINE 2048

typedef struct {
    const char *storage_file;
    pthread_mutex_t file_mutex;
} MailStore;

static MailStore store;

/* Reads one line from socket, discarding '\r' and stopping at '\n' */
ssize_t read_line(int sock, char *buffer, size_t size) {
    size_t i = 0;
    char c;

    if (size == 0) {
        return -1;
    }

    while (1) {
        ssize_t n = recv(sock, &c, 1, 0);

        if (n == 0) {
            if (i == 0) {
                return 0;
            }
            break;
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            break;
        }

        if (i < size - 1) {
            buffer[i++] = c;
        }
    }

    buffer[i] = '\0';
    return (ssize_t)i;
}

int send_all(int sock, const char *data, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}

int send_line(int sock, const char *text) {
    char buffer[MAX_LINE + 2];
    int written = snprintf(buffer, sizeof(buffer), "%s\n", text);

    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return -1;
    }

    return send_all(sock, buffer, (size_t)written);
}

void trim_newline(char *line) {
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

/* Checks whether a stored line belongs to recipient user_name.
   Stored format: from;to;message
*/
int recipient_matches(const char *line, const char *user_name) {
    const char *sep1 = strchr(line, ';');
    const char *sep2 = sep1 ? strchr(sep1 + 1, ';') : NULL;
    size_t user_len = strlen(user_name);

    if (sep1 == NULL || sep2 == NULL) {
        return 0;
    }

    if ((size_t)(sep2 - (sep1 + 1)) != user_len) {
        return 0;
    }

    return strncmp(sep1 + 1, user_name, user_len) == 0;
}

/* Append one offline message line to the storage file */
int store_message_line(const char *payload) {
    FILE *fp;

    pthread_mutex_lock(&store.file_mutex);

    fp = fopen(store.storage_file, "a");
    if (fp == NULL) {
        pthread_mutex_unlock(&store.file_mutex);
        return -1;
    }

    fprintf(fp, "%s\n", payload);
    fclose(fp);

    pthread_mutex_unlock(&store.file_mutex);
    return 0;
}

/* Send all messages for user_name to the requester and delete them */
int fetch_messages_for_user(int sock, const char *user_name) {
    FILE *in = NULL;
    FILE *out = NULL;
    char line[MAX_LINE];
    char temp_path[512];

    snprintf(temp_path, sizeof(temp_path), "%s.tmp", store.storage_file);

    pthread_mutex_lock(&store.file_mutex);

    in = fopen(store.storage_file, "r");
    if (in == NULL) {
        out = fopen(store.storage_file, "a");
        if (out != NULL) {
            fclose(out);
        }
        pthread_mutex_unlock(&store.file_mutex);
        send_line(sock, "END");
        return 0;
    }

    out = fopen(temp_path, "w");
    if (out == NULL) {
        fclose(in);
        pthread_mutex_unlock(&store.file_mutex);
        send_line(sock, "ERR");
        return -1;
    }

    while (fgets(line, sizeof(line), in) != NULL) {
        trim_newline(line);

        if (recipient_matches(line, user_name)) {
            char response[MAX_LINE + 10];
            snprintf(response, sizeof(response), "MAIL %s", line);

            if (send_line(sock, response) < 0) {
                fclose(in);
                fclose(out);
                remove(temp_path);
                pthread_mutex_unlock(&store.file_mutex);
                return -1;
            }
        } else {
            fprintf(out, "%s\n", line);
        }
    }

    fclose(in);
    fclose(out);

    if (rename(temp_path, store.storage_file) != 0) {
        remove(temp_path);
        pthread_mutex_unlock(&store.file_mutex);
        send_line(sock, "ERR");
        return -1;
    }

    pthread_mutex_unlock(&store.file_mutex);

    send_line(sock, "END");
    return 0;
}

/* Delete all stored messages whose recipient is user_name */
int delete_messages_for_user(const char *user_name, int *deleted_count) {
    FILE *in = NULL;
    FILE *out = NULL;
    char line[MAX_LINE];
    char temp_path[512];
    int deleted = 0;

    snprintf(temp_path, sizeof(temp_path), "%s.tmp", store.storage_file);

    pthread_mutex_lock(&store.file_mutex);

    in = fopen(store.storage_file, "r");
    if (in == NULL) {
        out = fopen(store.storage_file, "a");
        if (out != NULL) {
            fclose(out);
        }
        pthread_mutex_unlock(&store.file_mutex);

        if (deleted_count != NULL) {
            *deleted_count = 0;
        }
        return 0;
    }

    out = fopen(temp_path, "w");
    if (out == NULL) {
        fclose(in);
        pthread_mutex_unlock(&store.file_mutex);
        return -1;
    }

    while (fgets(line, sizeof(line), in) != NULL) {
        trim_newline(line);

        if (recipient_matches(line, user_name)) {
            deleted++;
        } else {
            fprintf(out, "%s\n", line);
        }
    }

    fclose(in);
    fclose(out);

    if (rename(temp_path, store.storage_file) != 0) {
        remove(temp_path);
        pthread_mutex_unlock(&store.file_mutex);
        return -1;
    }

    pthread_mutex_unlock(&store.file_mutex);

    if (deleted_count != NULL) {
        *deleted_count = deleted;
    }

    return 0;
}

void *handle_mail_client(void *arg) {
    int sock = *(int *)arg;
    free(arg);

    char buffer[MAX_LINE];
    ssize_t n = read_line(sock, buffer, sizeof(buffer));

    if (n <= 0) {
        close(sock);
        return NULL;
    }

    if (strncmp(buffer, "STORE ", 6) == 0) {
        if (store_message_line(buffer + 6) == 0) {
            send_line(sock, "OK");
        } else {
            send_line(sock, "ERR");
        }
    } else if (strncmp(buffer, "FETCH ", 6) == 0) {
        if (fetch_messages_for_user(sock, buffer + 6) != 0) {
            send_line(sock, "ERR");
        }
    } else if (strncmp(buffer, "DELETE ", 7) == 0) {
        int deleted = 0;

        if (delete_messages_for_user(buffer + 7, &deleted) == 0) {
            char response[64];
            snprintf(response, sizeof(response), "OK %d", deleted);
            send_line(sock, response);
        } else {
            send_line(sock, "ERR");
        }
    } else {
        send_line(sock, "ERR");
    }

    close(sock);
    return NULL;
}

/* Creates one listening socket: either IPv4 or IPv6 */
int create_listener(const char *port, int family) {
    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *rp;
    int sock = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int gai = getaddrinfo(NULL, port, &hints, &result);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            continue;
        }

        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (rp->ai_family == AF_INET6) {
            int v6only = 1;
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }

        if (bind(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            if (listen(sock, BACKLOG) == 0) {
                break;
            }
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(result);
    return sock;
}

void accept_and_spawn(int listen_sock) {
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
    if (client_sock < 0) {
        perror("accept");
        return;
    }

    int *pclient = malloc(sizeof(int));
    if (pclient == NULL) {
        close(client_sock);
        return;
    }

    *pclient = client_sock;

    pthread_t tid;
    if (pthread_create(&tid, NULL, handle_mail_client, pclient) != 0) {
        perror("pthread_create");
        close(client_sock);
        free(pclient);
        return;
    }

    pthread_detach(tid);
}

int main(int argc, char *argv[]) {
    const char *port;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Naudojimas: %s <mail_port> [storage_file]\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    port = argv[1];
    store.storage_file = (argc == 3) ? argv[2] : "offline_mail.txt";
    pthread_mutex_init(&store.file_mutex, NULL);

    int ipv4_sock = create_listener(port, AF_INET);
    int ipv6_sock = create_listener(port, AF_INET6);

    if (ipv4_sock < 0 && ipv6_sock < 0) {
        fprintf(stderr, "Nepavyko paleisti nei IPv4, nei IPv6 pasto serverio.\n");
        return 1;
    }

    printf("Pasto serveris saugo: %s\n", store.storage_file);
    if (ipv4_sock >= 0) {
        printf("Pasto serveris klauso per IPv4 porta %s\n", port);
    }
    if (ipv6_sock >= 0) {
        printf("Pasto serveris klauso per IPv6 porta %s\n", port);
    }

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);

        int maxfd = -1;

        if (ipv4_sock >= 0) {
            FD_SET(ipv4_sock, &readfds);
            if (ipv4_sock > maxfd) {
                maxfd = ipv4_sock;
            }
        }

        if (ipv6_sock >= 0) {
            FD_SET(ipv6_sock, &readfds);
            if (ipv6_sock > maxfd) {
                maxfd = ipv6_sock;
            }
        }

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (ipv4_sock >= 0 && FD_ISSET(ipv4_sock, &readfds)) {
            accept_and_spawn(ipv4_sock);
        }

        if (ipv6_sock >= 0 && FD_ISSET(ipv6_sock, &readfds)) {
            accept_and_spawn(ipv6_sock);
        }
    }

    if (ipv4_sock >= 0) close(ipv4_sock);
    if (ipv6_sock >= 0) close(ipv6_sock);

    pthread_mutex_destroy(&store.file_mutex);
    return 0;
}