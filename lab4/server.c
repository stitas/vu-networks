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
#include <ctype.h>

#define MAX_NAME_LEN 50
#define MAX_MSG_LEN 1024
#define BACKLOG 20
#define MAIL_LINE_MAX 2048

typedef struct Client {
    int sock;
    char name[MAX_NAME_LEN];
    struct Client *next;
} Client;

static Client *clients = NULL;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *mail_host = NULL;
static const char *mail_port = NULL;

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

/* Sends all bytes */
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

/* Sends one line with '\n' */
int send_line(int sock, const char *text) {
    char buffer[MAX_MSG_LEN + MAX_NAME_LEN + 128];
    int written = snprintf(buffer, sizeof(buffer), "%s\n", text);

    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return -1;
    }

    return send_all(sock, buffer, (size_t)written);
}

/* Sends text in format understood by the Java client */
int send_chat_text(int sock, const char *text) {
    char buffer[MAX_MSG_LEN + MAX_NAME_LEN + 128];
    int written = snprintf(buffer, sizeof(buffer), "PRANESIMAS %s", text);

    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return -1;
    }

    return send_line(sock, buffer);
}

/* Username rules:
   - non-empty
   - no spaces
   - no ';'
   - cannot start with '@' or '#'
*/
int is_valid_name(const char *name) {
    size_t len = strlen(name);
    size_t i;

    if (len == 0 || len >= MAX_NAME_LEN) {
        return 0;
    }

    if (name[0] == '@' || name[0] == '#') {
        return 0;
    }

    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!isprint(ch) || isspace(ch) || ch == ';') {
            return 0;
        }
    }

    return 1;
}

Client *find_client_by_name_locked(const char *name) {
    Client *curr = clients;

    while (curr != NULL) {
        if (strcmp(curr->name, name) == 0) {
            return curr;
        }
        curr = curr->next;
    }

    return NULL;
}

int name_exists(const char *name) {
    return find_client_by_name_locked(name) != NULL;
}

void add_client(Client *client) {
    client->next = clients;
    clients = client;
}

void remove_client(Client *client) {
    Client **curr = &clients;

    while (*curr != NULL) {
        if (*curr == client) {
            *curr = client->next;
            return;
        }
        curr = &((*curr)->next);
    }
}

void broadcast_message(const char *text) {
    char full[MAX_MSG_LEN + MAX_NAME_LEN + 128];
    snprintf(full, sizeof(full), "PRANESIMAS %s", text);

    pthread_mutex_lock(&clients_mutex);

    Client *curr = clients;
    while (curr != NULL) {
        send_line(curr->sock, full);
        curr = curr->next;
    }

    pthread_mutex_unlock(&clients_mutex);
}

/* ---------------- Mail server communication ---------------- */

int connect_to_mail_server(void) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    int sock = -1;
    int gai;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    gai = getaddrinfo(mail_host, mail_port, &hints, &result);
    if (gai != 0) {
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            continue;
        }

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(result);
    return sock;
}

int store_offline_message(const char *from, const char *to, const char *message) {
    char command[MAIL_LINE_MAX];
    char response[MAIL_LINE_MAX];
    int sock = connect_to_mail_server();

    if (sock < 0) {
        return -1;
    }

    if (snprintf(command, sizeof(command), "STORE %s;%s;%s", from, to, message) >= (int)sizeof(command)) {
        close(sock);
        return -1;
    }

    if (send_line(sock, command) < 0) {
        close(sock);
        return -1;
    }

    if (read_line(sock, response, sizeof(response)) <= 0) {
        close(sock);
        return -1;
    }

    close(sock);
    return strcmp(response, "OK") == 0 ? 0 : -1;
}

int fetch_offline_messages_for_user(Client *client) {
    char command[MAIL_LINE_MAX];
    char response[MAIL_LINE_MAX];
    int found = 0;
    int sock = connect_to_mail_server();

    if (sock < 0) {
        send_chat_text(client->sock, "Nepavyko pasiekti zinuciu serverio.");
        return -1;
    }

    snprintf(command, sizeof(command), "FETCH %s", client->name);

    if (send_line(sock, command) < 0) {
        close(sock);
        send_chat_text(client->sock, "Nepavyko nuskaityti zinuciu.");
        return -1;
    }

    while (1) {
        ssize_t n = read_line(sock, response, sizeof(response));
        if (n <= 0) {
            close(sock);
            send_chat_text(client->sock, "Zinuciu serveris nutrauke rysi.");
            return -1;
        }

        if (strcmp(response, "END") == 0) {
            break;
        }

        if (strncmp(response, "MAIL ", 5) == 0) {
            char *payload = response + 5;
            char *sep1 = strchr(payload, ';');
            char *sep2 = sep1 ? strchr(sep1 + 1, ';') : NULL;

            if (sep1 == NULL || sep2 == NULL) {
                continue;
            }

            *sep1 = '\0';
            *sep2 = '\0';
            found = 1;

            char out[MAX_MSG_LEN + MAX_NAME_LEN + 128];
            snprintf(out, sizeof(out), "[mail] %s -> %s", payload, sep2 + 1);
            send_chat_text(client->sock, out);
        }
    }

    close(sock);

    if (!found) {
        send_chat_text(client->sock, "Nera issaugotu zinuciu.");
    }

    return 0;
}

int admin_delete_mail(const char *user_name, int *deleted_count) {
    char command[MAIL_LINE_MAX];
    char response[MAIL_LINE_MAX];
    int sock = connect_to_mail_server();

    if (deleted_count != NULL) {
        *deleted_count = 0;
    }

    if (sock < 0) {
        return -1;
    }

    snprintf(command, sizeof(command), "DELETE %s", user_name);

    if (send_line(sock, command) < 0) {
        close(sock);
        return -1;
    }

    if (read_line(sock, response, sizeof(response)) <= 0) {
        close(sock);
        return -1;
    }

    close(sock);

    if (strncmp(response, "OK ", 3) != 0) {
        return -1;
    }

    if (deleted_count != NULL) {
        *deleted_count = atoi(response + 3);
    }

    store_offline_message("admin", user_name, "Zinutes del saugumo buvo istrintos");

    return 0;
}

/* ---------------- Chat logic ---------------- */

void handle_private_message(Client *sender, const char *input) {
    const char *space = strchr(input, ' ');
    char recipient[MAX_NAME_LEN];
    const char *message;
    size_t name_len;

    if (space == NULL || input[1] == '\0') {
        send_chat_text(sender->sock, "Privacios zinutes formatas: @vardas zinute");
        return;
    }

    name_len = (size_t)(space - (input + 1));
    if (name_len == 0 || name_len >= sizeof(recipient)) {
        send_chat_text(sender->sock, "Netinkamas gavejo vardas.");
        return;
    }

    memcpy(recipient, input + 1, name_len);
    recipient[name_len] = '\0';

    message = space + 1;
    if (*message == '\0') {
        send_chat_text(sender->sock, "Privacios zinutes formatas: @vardas zinute");
        return;
    }

    pthread_mutex_lock(&clients_mutex);

    Client *target = find_client_by_name_locked(recipient);
    if (target != NULL) {
        char to_target[MAX_MSG_LEN + MAX_NAME_LEN + 128];
        char to_sender[MAX_MSG_LEN + MAX_NAME_LEN + 128];

        snprintf(to_target, sizeof(to_target), "[privatu] %s: %s", sender->name, message);
        send_chat_text(target->sock, to_target);

        snprintf(to_sender, sizeof(to_sender), "[privatu -> %s] %s", recipient, message);
        send_chat_text(sender->sock, to_sender);

        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    pthread_mutex_unlock(&clients_mutex);

    if (store_offline_message(sender->name, recipient, message) == 0) {
        char ack[MAX_MSG_LEN + MAX_NAME_LEN + 128];
        snprintf(ack, sizeof(ack), "%s neprisijunges. Zinute issaugota pasto serveryje.", recipient);
        send_chat_text(sender->sock, ack);
    } else {
        send_chat_text(sender->sock, "Gavejas neprisijunges, bet nepavyko issaugoti zinutes.");
    }
}

void handle_command_or_chat(Client *client, const char *buffer) {
    if (strcmp(buffer, "#mail") == 0) {
        fetch_offline_messages_for_user(client);
        return;
    }

    if (strncmp(buffer, "#del ", 5) == 0) {
        if (strcmp(client->name, "admin") != 0) {
            send_chat_text(client->sock, "Tik admin gali naudoti #del komanda.");
            return;
        }

        const char *user_name = buffer + 5;
        int deleted_count = 0;

        if (*user_name == '\0') {
            send_chat_text(client->sock, "Naudojimas: #del vartotojas");
            return;
        }

        if (admin_delete_mail(user_name, &deleted_count) == 0) {
            char msg[MAX_MSG_LEN + MAX_NAME_LEN + 128];
            snprintf(msg, sizeof(msg), "Istrinta %d zinuciu(-os) vartotojui %s.", deleted_count, user_name);
            send_chat_text(client->sock, msg);
        } else {
            send_chat_text(client->sock, "Nepavyko istrinti zinuciu is pasto serverio.");
        }

        return;
    }

    if (buffer[0] == '@') {
        handle_private_message(client, buffer);
        return;
    }

    char msg[MAX_MSG_LEN + MAX_NAME_LEN + 64];
    snprintf(msg, sizeof(msg), "%s: %s", client->name, buffer);
    broadcast_message(msg);
}

void *handle_client(void *arg) {
    int sock = *(int *)arg;
    free(arg);

    char buffer[MAX_MSG_LEN];

    Client *client = malloc(sizeof(Client));
    if (client == NULL) {
        close(sock);
        return NULL;
    }

    client->sock = sock;
    client->name[0] = '\0';
    client->next = NULL;

    /* Ask for name until we get a unique valid one */
    while (1) {
        if (send_line(sock, "ATSIUSKVARDA") < 0) {
            close(sock);
            free(client);
            return NULL;
        }

        ssize_t n = read_line(sock, buffer, sizeof(buffer));
        if (n <= 0) {
            close(sock);
            free(client);
            return NULL;
        }

        if (!is_valid_name(buffer)) {
            send_chat_text(sock, "Vardas turi buti be tarpu ir kabliataskiu.");
            continue;
        }

        pthread_mutex_lock(&clients_mutex);

        if (!name_exists(buffer)) {
            strncpy(client->name, buffer, MAX_NAME_LEN - 1);
            client->name[MAX_NAME_LEN - 1] = '\0';
            add_client(client);
            pthread_mutex_unlock(&clients_mutex);
            break;
        }

        pthread_mutex_unlock(&clients_mutex);
        send_chat_text(sock, "Toks vardas jau naudojamas.");
    }

    if (send_line(sock, "VARDASOK") < 0) {
        pthread_mutex_lock(&clients_mutex);
        remove_client(client);
        pthread_mutex_unlock(&clients_mutex);
        close(sock);
        free(client);
        return NULL;
    }

    if (strcmp(client->name, "admin") == 0) {
        send_chat_text(client->sock, "Admin komanda: #del vartotojas");
    }
    send_chat_text(client->sock, "Privati zinute: @vardas tekstas | Pastas: #mail");

    char join_msg[MAX_MSG_LEN];
    snprintf(join_msg, sizeof(join_msg), "%s prisijunge.", client->name);
    broadcast_message(join_msg);

    while (1) {
        ssize_t n = read_line(sock, buffer, sizeof(buffer));

        if (n <= 0) {
            break;
        }

        if (buffer[0] == '\0') {
            continue;
        }

        handle_command_or_chat(client, buffer);
    }

    pthread_mutex_lock(&clients_mutex);
    remove_client(client);
    pthread_mutex_unlock(&clients_mutex);

    char leave_msg[MAX_MSG_LEN];
    snprintf(leave_msg, sizeof(leave_msg), "%s atsijunge.", client->name);
    broadcast_message(leave_msg);

    close(sock);
    free(client);
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

    char host[INET6_ADDRSTRLEN];
    char serv[16];

    if (getnameinfo((struct sockaddr *)&client_addr, client_len,
                    host, sizeof(host),
                    serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        printf("Naujas klientas is %s:%s\n", host, serv);
    }

    int *pclient = malloc(sizeof(int));
    if (pclient == NULL) {
        close(client_sock);
        return;
    }

    *pclient = client_sock;

    pthread_t tid;
    if (pthread_create(&tid, NULL, handle_client, pclient) != 0) {
        perror("pthread_create");
        close(client_sock);
        free(pclient);
        return;
    }

    pthread_detach(tid);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Naudojimas: %s <chat_port> <mail_host> <mail_port>\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    mail_host = argv[2];
    mail_port = argv[3];

    int ipv4_sock = create_listener(argv[1], AF_INET);
    int ipv6_sock = create_listener(argv[1], AF_INET6);

    if (ipv4_sock < 0 && ipv6_sock < 0) {
        fprintf(stderr, "Nepavyko paleisti nei IPv4, nei IPv6 chat serverio.\n");
        return 1;
    }

    if (ipv4_sock >= 0) {
        printf("Chat serveris klauso per IPv4 porta %s\n", argv[1]);
    }
    if (ipv6_sock >= 0) {
        printf("Chat serveris klauso per IPv6 porta %s\n", argv[1]);
    }
    printf("Naudojamas pasto serveris %s:%s\n", mail_host, mail_port);

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

    return 0;
}