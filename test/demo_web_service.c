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

static int create_and_bind (char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, sfd;

    memset (&hints, 0, sizeof (struct addrinfo));
    hints.ai_family = AF_INET;       /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */
    hints.ai_flags = AI_PASSIVE;     /* All interfaces */

    s = getaddrinfo (NULL, port, &hints, &result);
    if (s != 0)
    {
        fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        sfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1)
            continue;

        int reuse = 1;
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
            perror("setsockopt(SO_REUSEADDR) failed");

#ifdef SO_REUSEPORT
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse)) < 0) 
            perror("setsockopt(SO_REUSEPORT) failed");
#endif

        s = bind (sfd, rp->ai_addr, rp->ai_addrlen);
        if (s == 0)
        {
            /* We managed to bind successfully! */
            break;
        }

        close (sfd);
    }

    if (rp == NULL)
    {
        fprintf (stderr, "Could not bind\n");
        return -1;
    }

    freeaddrinfo (result);

    return sfd;
}

int main()
{
    int sfd, s;
    int efd;
    struct epoll_event event;
    struct epoll_event *events;
    static int visit_counter = 0;

    sfd = create_and_bind ("9000");
    if (sfd == -1)
        abort ();

    s = make_socket_non_blocking (sfd);
    if (s == -1)
        abort ();

    s = listen (sfd, SOMAXCONN);
    if (s == -1)
    {
        perror ("listen");
        abort ();
    }

    efd = epoll_create1 (0);
    if (efd == -1)
    {
        perror ("epoll_create");
        abort ();
    }

    event.data.fd = sfd;
    event.events = EPOLLIN;
    s = epoll_ctl (efd, EPOLL_CTL_ADD, sfd, &event);
    if (s == -1)
    {
        perror ("epoll_ctl");
        abort ();
    }

    /* Buffer where events are returned */
    events = calloc (MAXEVENTS, sizeof event);

    /* The event loop */
    while (1)
    {
        int n, i;

        n = epoll_wait (efd, events, MAXEVENTS, -1);
        for (i = 0; i < n; i++)
        {
            if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP))
            {
                /* An error has occured on this fd, or the socket is not
                   ready for reading (why were we notified then?) */
                //fprintf (stderr, "epoll error\n");
                close (events[i].data.fd);
            }

            else if (sfd == events[i].data.fd)
            {
                /* We have a notification on the listening socket, which
                   means one or more incoming connections. */
                struct sockaddr in_addr;
                socklen_t in_len;
                int infd;
                char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

                in_len = sizeof in_addr;
                infd = accept (sfd, &in_addr, &in_len);
                if (infd == -1)
                {
                    if ((errno == EAGAIN) ||
                            (errno == EWOULDBLOCK))
                    {
                        /* We have processed all incoming
                           connections. */
                    }
                    else
                    {
                        perror ("accept");
                        abort();
                    }
                }

                s = getnameinfo (&in_addr, in_len,
                        hbuf, sizeof hbuf,
                        sbuf, sizeof sbuf,
                        NI_NUMERICHOST | NI_NUMERICSERV);
                if (s == 0)
                {
                    //printf("Accepted connection on descriptor %d "
                    //        "(host=%s, port=%s)\n", infd, hbuf, sbuf);
                }

                /* Make the incoming socket non-blocking and add it to the
                   list of fds to monitor. */
                s = make_socket_non_blocking (infd);
                if (s == -1)
                    abort ();

                event.data.fd = infd;
                event.events = EPOLLIN | EPOLLET;
                s = epoll_ctl (efd, EPOLL_CTL_ADD, infd, &event);
                if (s == -1)
                {
                    perror ("epoll_ctl");
                    abort ();
                }
            }
            else if (events[i].events & EPOLLIN)
            {
                ssize_t count;
                char buf[4096];

                count = read (events[i].data.fd, buf, sizeof buf);
                if (count == -1)
                {
                    /* If errno == EAGAIN, that means we have read all
                       data. So go back to the main loop. */
                    if (errno != EAGAIN)
                    {
                        perror ("read");
                        abort();
                    }
                }
                else if (count == 0)
                {
                    /* End of file. The remote has closed the
                       connection. */
                    //printf ("Closed connection on descriptor %d\n",
                    //        events[i].data.fd);

                    /* Closing the descriptor will make epoll remove it
                       from the set of descriptors which are monitored. */
                    close (events[i].data.fd);
                }
                else {
                    char findstr[] = "GET / HTTP/1.1";
                    if (strstr(buf, findstr) == NULL) {
                        close (events[i].data.fd);
                    }
                    else {
                        char buf[4096] = "HTTP/1.1 200 OK\r\n"
                            "Server: nginx/1.12.2\r\n"
                            "Content-Type: text/html\r\n"
                            "Connection: close\r\n"
                            //"Content-Length: 200\r\n"
                            "\r\n"
                            "<html><body><h1>Sample web page, visit count ";
                        char counter_str[40] = {0};
                        sprintf(counter_str, "%d\n", ++visit_counter);
                        char buf2[40] =
                            "</h1></body></html>\r\n";
                        write (events[i].data.fd, buf, strlen(buf));
                        write (events[i].data.fd, counter_str, strlen(counter_str));
                        write (events[i].data.fd, buf2, strlen(buf2));
                        //printf ("received %d bytes from fd %d\n", count, events[i].data.fd);
                        shutdown (events[i].data.fd, SHUT_RDWR);
                    }
                }
            }
        }
    }

    free (events);

    close (sfd);

    return EXIT_SUCCESS;
}
