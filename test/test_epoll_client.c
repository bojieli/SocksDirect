#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#define MAXEVENTS 64

static int make_socket_non_blocking (int sfd)
{
    int flags, s;

    flags = fcntl (sfd, F_GETFL, 0);
    if (flags == -1)
    {
        perror ("fcntl");
        return -1;
    }

    flags |= O_NONBLOCK;
    s = fcntl (sfd, F_SETFL, flags);
    if (s == -1)
    {
        perror ("fcntl");
        return -1;
    }

    return 0;
}

void send_messages_to_fd(int sfd)
{
    printf ("Connection established for fd %d!\n", sfd);
    char buf[512] = {0};
    const int messages = 10;

    for (int j = 0; j < messages; j ++)
    {
        buf[0] = j;
        if (write (sfd, buf, sizeof buf) < 0)
        {
            if (errno != EAGAIN)
            {
                perror ("write");
                abort();
            }
            break;
        }
    }
}

int main(int argc, char **argv)
{
    int efd;
    struct epoll_event event;
    struct epoll_event *events = calloc (MAXEVENTS, sizeof event);
    struct sockaddr_in serv_addr;

    if (argc != 2) {
        printf("Usage: %s <num_conn>\n", argv[0]);
        return 1;
    }
    int num_conn = atoi(argv[1]);

    efd = epoll_create1 (0);
    if (efd == -1)
    {
        perror ("epoll_create");
        abort ();
    }

    for (int i=0; i<num_conn; i++) {
        int sfd;
        if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        {
            printf("Error : Could not create socket\n");
            abort();
        } 

        memset(&serv_addr, 0, sizeof(serv_addr)); 
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(8080); 

        if (inet_pton (AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0)
        {
            printf("Error : inet_pton error occured\n");
            abort();
        } 

        if (make_socket_non_blocking (sfd) < 0)
        {
            printf("Error : non block\n");
            abort();
        }

        event.data.fd = sfd;
        event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        if (epoll_ctl (efd, EPOLL_CTL_ADD, sfd, &event) < 0)
        {
            perror ("epoll_ctl");
            abort ();
        }

        if (connect(sfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        {
            // expected: EINPROGRESS because we are non blocking
            if (errno != EINPROGRESS) {
                printf("Error : Connect Failed, errno %d\n", errno);
                abort();
            }
        } 
    }

    /* The event loop */
    while (num_conn > 0) {
        int n = epoll_wait (efd, events, MAXEVENTS, -1);
        printf ("epoll_wait returned %d events\n", n);
        for (int i = 0; i < n; i++)
        {
            if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP))
            {
                fprintf (stderr, "epoll error %d\n", events[i].data.fd);
                close (events[i].data.fd);
                num_conn --;
            }
            else if (events[i].events & EPOLLOUT)
            {
                send_messages_to_fd(events[i].data.fd);
                close (events[i].data.fd);
                printf ("fd %d finish\n", events[i].data.fd);
                num_conn --;
            }
            else if (events[i].events & EPOLLIN)
            {
                fprintf (stderr, "EPOLLIN not expected on fd %d\n", events[i].data.fd);
            }
        }
    }

    return 0;
}
