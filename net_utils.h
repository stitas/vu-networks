#ifndef NET_UTILS_H
#define NET_UTILS_H

#define BUFFER_SIZE 1024

int create_server_socket(short port);
int connect_to_server(const char *ip, short port);
int send_all(int sock, const char *buf, int len);
int read_line(int sock, char *buf, int max);

#endif