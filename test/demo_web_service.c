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
#include <libmemcached/memcached.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <syscall.h>
#include <signal.h>

#define MAXEVENTS 64

// defined as a macro to make the connection states thread local
#define memcached_init() \
  memcached_server_st *servers = NULL; \
  memcached_st *memc; \
  memcached_return rc; \
 \
  memc = memcached_create(NULL); \
  servers = memcached_server_list_append(servers, "localhost", 11211, &rc); \
  rc = memcached_server_push(memc, servers); \
 \
  if (rc == MEMCACHED_SUCCESS) \
    fprintf(stderr, "PID %d connected to memcached server\n", (pid_t) syscall(SYS_gettid)); \
  else \
    fprintf(stderr, "Couldn't add server: %s\n", memcached_strerror(memc, rc)); \


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

double get_memcached_time(memcached_st *memc, memcached_return *rc)
{
    struct timespec begin_time, end_time;
    clock_gettime(CLOCK_REALTIME, &begin_time);
    for (int i=0; i<2; i++) {
        char *key = "keylat";
        size_t value_length;
        int flags;
        char *retrieved_value = memcached_get(memc, key, strlen(key), &value_length, &flags, rc);
        if (value_length > 0 && retrieved_value)
            free(retrieved_value);
    }
    clock_gettime(CLOCK_REALTIME, &end_time);
    double usec = (1e6 * (end_time.tv_sec - begin_time.tv_sec) + (end_time.tv_nsec - begin_time.tv_nsec) / 1.0e3);
    return usec;
}

struct counter_t {
    unsigned long data;
    char padding[64-8];
};
struct counter_t counter[256];

int check_client_exists()
{
	const char name[] = "demo_http_bench";
    DIR* dir;
    struct dirent* ent;
    char* endptr;
    char buf[512];

    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return 0;
    }

    while((ent = readdir(dir)) != NULL) {
        /* if endptr is not a null character, the directory is not
         * entirely numeric, so ignore it */
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0') {
            continue;
        }

        /* try to open the cmdline file */
        snprintf(buf, sizeof(buf), "/proc/%ld/cmdline", lpid);
        FILE* fp = fopen(buf, "r");

        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                /* check the first token in the file, the program name */
                char* first = strtok(buf, " ");
                if (NULL != strstr(first, name)) {
                    fclose(fp);
                    closedir(dir);
                    return 1;
                }
            }
            fclose(fp);
        }

    }

    closedir(dir);
    return 0;
}

static void * event_loop_thread(void *arg)
{
    size_t nthreads = (size_t) arg;
    counter[nthreads].data = 0;

    while (!check_client_exists())
        usleep(100000);

    while (1) {
        for (int i=0; i<100; i++)
            getppid();
        if (getppid() == 0)
            counter[nthreads].data ++;
        else
            counter[0].data ++;
    }
}

void create_worker_threads(int argc, char ** argv) {
    pthread_t thread_id;
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    int nthreads = 1;
    if (argc == 2)
        nthreads = atoi(argv[1]);
    for (size_t i=0; i<3*nthreads; i++)
        pthread_create(&thread_id, &attr, event_loop_thread, (void *)i);
}

static void * reporter_thread(void *arg)
{
    unsigned long nthreads = atoi((char *)arg);
    unsigned long last_counter = 0;

    while (!check_client_exists())
        usleep(100000);

	memcached_init();
    while (1) {
        usleep(500000);
        unsigned long total_counter = 0;
        for (int i=0; i<3*nthreads; i++)
            total_counter += counter[i].data;
        double latency = get_memcached_time(memc, &rc);
        char *name = (getppid() == 0 ? "ipc" : "linux");
        FILE *fp = fopen("/usr/share/nginx/html/data/live.txt", "w");
        fprintf(fp, "%s-%d %.2lf %.2lf\n", name, nthreads, (total_counter - last_counter) / 2.0e4, latency);
        fclose(fp);
        last_counter = total_counter;
    }
}

void create_reporter_threads(int argc, char **argv) {
    pthread_t thread_id;
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    pthread_create(&thread_id, &attr, reporter_thread, (argc == 2 ? argv[1] : "1"));
}

void signal_callback_handler(int signum) {
    printf("Caught signal SIGPIPE %d\n", signum);
}

int main(int argc, char** argv)
{
    int sfd, s;
    int efd;
    struct epoll_event event;
    struct epoll_event *events;
    static int visit_counter = 0;

    signal(SIGPIPE, signal_callback_handler);

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

    create_worker_threads(argc, argv);
    create_reporter_threads(argc, argv);
	memcached_init();

    printf("Waiting for events...\n");
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
                unsigned long memcached_count = 0;

                struct timespec begin_time;
                clock_gettime(CLOCK_REALTIME, &begin_time);

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
					char long_query[] = "GET /slow_query HTTP/1.";
                    char findstr[] = "GET / HTTP/1.";
                    int found = 0;
					if (strstr(buf, long_query) != NULL) {
                        char *retrieved_value = NULL;
                        char key[] = "testkey";
                        size_t value_length = 0;
                        uint32_t flags;

                        for (int i=0; i<1e5; i++) {
                            retrieved_value = memcached_get(memc, key, strlen(key), &value_length, &flags, &rc);
                            if (value_length > 0 && retrieved_value)
                                free(retrieved_value);
                            memcached_count ++;
                        }
                        found = 1;
					}
                    else if (strstr(buf, findstr) == NULL) {
                        found = 0;
                    }
                    else { // singl query test
                        char *retrieved_value = NULL;
                        char key[] = "testkey";
                        size_t value_length = 0;
                        uint32_t flags;

                        retrieved_value = memcached_get(memc, key, strlen(key), &value_length, &flags, &rc);
                        if (value_length > 0 && retrieved_value)
                            free(retrieved_value);
                        memcached_count ++;

                        found = 1;
                    }

                    if (found) {
                        struct timespec end_time;
                        clock_gettime(CLOCK_REALTIME, &end_time);
                        int msec = (int)(1e3 * (end_time.tv_sec - begin_time.tv_sec) + (end_time.tv_nsec - begin_time.tv_nsec) / 1e6);

                        char buf[4096] = "HTTP/1.1 200 OK\r\n"
                            "Server: nginx/1.12.2\r\n"
                            "Content-Type: text/html\r\n"
                            "Connection: close\r\n"
                            //"Content-Length: 200\r\n"
                            "\r\n"
                            "<html><head><title>Test page</title></head><body><h1>Sample web page</h1><h2>visit count ";
                        char counter_str[256] = {0};
                        sprintf(counter_str, "%d</h2><h2>page loaded in %d ms</h2><h2>accessed %d keys in memcached</h2>", ++visit_counter, msec, memcached_count);
                        char buf2[40] =
                            "</body></html>\r\n";
                        write (events[i].data.fd, buf, strlen(buf));
                        write (events[i].data.fd, counter_str, strlen(counter_str));
                        write (events[i].data.fd, buf2, strlen(buf2));
                        //printf ("received %d bytes from fd %d\n", count, events[i].data.fd);
                    }
                    else {
                        char buf[4096] = "HTTP/1.1 404 Not Found\r\n"
                            "Server: nginx/1.12.2\r\n"
                            "Connection: close\r\n"
                            "\r\n";
                        write (events[i].data.fd, buf, strlen(buf));
                    }

                    shutdown (events[i].data.fd, SHUT_WR);
                }
            }
        }
    }

    free (events);

    close (sfd);

    return EXIT_SUCCESS;
}
