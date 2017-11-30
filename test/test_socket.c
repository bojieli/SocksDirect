//
// Created by ctyi on 11/28/17.
//
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    struct sockaddr_in serveraddr;
    serveraddr.sin_port = htons(80);
    serveraddr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &serveraddr.sin_addr);
    printf("%d\n", connect(2147483645, (struct sockaddr *)&serveraddr, sizeof(serveraddr)));
    return 0;
}
