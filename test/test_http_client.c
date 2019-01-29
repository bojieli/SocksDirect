# include <stdio.h>
# include <stdlib.h>
# include <sys/socket.h>
# include <arpa/inet.h>
# include <netinet/in.h>
# include <netdb.h>
# include <string.h>
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
#include <pthread.h>
#include <netinet/tcp.h>

/*
 * (c) 2011 dermesser
 * This piece of software is licensed with GNU GPL 3.
 *
 */

const char* help_string = "Usage: simple-http [-h] [-4|-6] [-p PORT] [-o OUTPUT_FILE] [-b BATCH_SIZE] [-n THREAD_NUM] <SERVER-IP> <TESTSIZE-BYTES>\n";

void errExit(const char* str, char p)
{
	if ( p != 0 )
	{
		perror(str);
	} else
	{
		if (write(2,str,strlen(str)) == -1)
            exit(2);
	}
	exit(1);
}

int testsize;
int n_threads;
int batch_size;

struct worker_args
{
    struct addrinfo *server_info;
    int id;
} thread_args[32];

struct stat_t
{
    long counter;
    double totol_time;
    long last_counter;
} stat[32];
pthread_t threads[32];
struct addrinfo *result, hints;

void * worker_rd(void* args);
void * worker(void * args)
{
    char request[512];
    char response[8192];
    int fd;
    int id;
    struct addrinfo *result;
    result = ((struct worker_args *)args)->server_info;
    id = ((struct worker_args *)args)->id;
    //printf("ID: %d\n", id);
    fd = socket(result->ai_family,SOCK_STREAM,0);
    if ( fd < 0 )
        errExit("socket()",1);
    int flag = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag) );
    if ( connect(fd,result->ai_addr,result->ai_addrlen) == -1)
        errExit("connect",1);

    int flags, s;

    flags = fcntl (fd, F_GETFL, 0);
    if (flags == -1)
    {
        perror ("fcntl");
        return NULL;
    }

    flags |= O_NONBLOCK;
    s = fcntl (fd, F_SETFL, flags);
    if (s == -1)
    {
        perror ("fcntl");
        return NULL;
    }


    while (1)
    {
        for(int i=0;i<batch_size;++i)
        {
            struct timespec startt;
            clock_gettime(CLOCK_MONOTONIC, &startt);
            sprintf(request, "GET /%d/%ld/%ld HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "Connection: keep-alive\r\n\r\n", testsize, startt.tv_sec, startt.tv_nsec);
            int req_len = (int)strlen(request);
            int curr_ptr = 0;
            while (curr_ptr < req_len)
            {
                int ret = (int)send(fd, request+curr_ptr, req_len, 0);
                if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return NULL;
                curr_ptr += ret;
            }
        }

        /* Now we handling the response*/
        char templaterepstr[] = "\r\n\r\n";
        char* startptr = response;
        for (int i=0; i< batch_size; ++i)
        {
            int curr_ptr = 0;
            char* parse_ptr;
            while (1)
            {
                int ret = (int)recv(fd, startptr+curr_ptr, 8192, 0);
                if (ret < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        continue;
                    else
                    {
                        printf("Worker get return failed %d\n", errno);
                        return NULL;
                    }
                }
                if (ret == 0)
                {
                    printf("Connection closed on cnt %d\n", stat[id].counter);
                    shutdown(fd, SHUT_RDWR);
                    return NULL;
                }
                curr_ptr += ret;
                response[curr_ptr] = '\0';
                parse_ptr = strstr(startptr, templaterepstr);
                /* header fully read */
                if (parse_ptr != NULL)
                    break;
            }
            /* Now we need to know where is the response body*/
            int body_ptr = parse_ptr - response + 4;
            /* We have not fully get the body, wait for the body */
            if (curr_ptr - body_ptr < testsize)
            {
                int len_to_read = testsize - (curr_ptr  - body_ptr);
                while (len_to_read > 0)
                {
                    int ret = (int)recv(fd, startptr+curr_ptr, len_to_read, 0);
                    if (ret < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            continue;
                        else
                        {
                            printf("Worker get return failed\n");
                            return NULL;
                        }
                    }
                    len_to_read -= ret;
                    curr_ptr += ret;
                }
            }
            /* Now we know we already read the full bddy */
            struct timespec end_t, start_t;
            clock_gettime(CLOCK_MONOTONIC, &end_t);
            char * tv_sec_delimiter_ptr;
            start_t.tv_sec = strtoul(&response[body_ptr], &tv_sec_delimiter_ptr, 0);
            if (*tv_sec_delimiter_ptr == '\0')
            {
                errExit("Failed to read nanosecond", 0);
            }
            start_t.tv_nsec = strtol(tv_sec_delimiter_ptr+1, NULL, 0);
            double duration; /* us */
            duration = (end_t.tv_sec - start_t.tv_sec)*1e6 + (end_t.tv_nsec - start_t.tv_nsec)*1e-3;
            if (duration < 0)
            {
                printf("Wrong duration %.3lf\n", duration);
                return NULL;
            }
            ++stat[id].counter;
            stat[id].totol_time += duration;

            /* If we got more than one response I just copy it back to the front */
            memcpy(response, parse_ptr + 4 + testsize, response + curr_ptr - (parse_ptr + 4 + testsize));
            startptr = response;
        }

    }
}


int main (int argc, char** argv)
{
	int srvfd, rwerr = 42, outfile, ai_family = AF_UNSPEC;
	char *request, buf[16], port[6],c;

	memset(port,0,6);

	if ( argc < 3 )
		errExit(help_string,0);
	
	strncpy(port,"80",2);

	while ( (c = getopt(argc,argv,"p:ho:n:b:46")) >= 0 )
	{
		switch (c)
		{
			case 'h' :
				errExit(help_string,0);
			case 'p' :
				strncpy(port,optarg,5);
				break;
			case 'o' :
				outfile = open(optarg,O_WRONLY|O_CREAT,0644);
				close(1);
				dup2(outfile,1);
				break;
			case '4' :
				ai_family = AF_INET;
				break;
			case '6' :
				ai_family = AF_INET6;
				break;
		    case 'n':
		        n_threads = atoi(optarg);
		        break;
		    case 'b':
		        batch_size = atoi(optarg);
		        break;

		}
	}

	testsize = atoi(argv[optind + 1]);
	if (testsize <= 0)
	    errExit("Invalid testsize", 0);
    if (batch_size <= 0)
        errExit("Invalid batch size", 0);
    if (n_threads <= 0)
        errExit("Invalid thread number", 0);

	memset(&hints,0,sizeof(struct addrinfo));

	hints.ai_family = ai_family;
	hints.ai_socktype = SOCK_STREAM;

	if ( 0 != getaddrinfo(argv[optind],port,&hints,&result))
		errExit("getaddrinfo",1);

	for (int i=0;i<n_threads;++i)
    {
	    thread_args[i].id = i;
	    thread_args[i].server_info = result;
	    stat[i].totol_time = 0;
	    stat[i].counter = 0;
        stat[i].last_counter = 0;
	    pthread_create(&threads[i], NULL, worker, &thread_args[i]);
    }

    while (1)
    {
        sleep(1);
        for (int i=0;i<n_threads;++i)
        {
            printf("%.3lf %d %.3lf %d ; ", stat[i].totol_time / stat[i].counter, stat[i].counter, 1e6 / (stat[i].counter - stat[i].last_counter), stat[i].counter - stat[i].last_counter);
            stat[i].last_counter = stat[i].counter;
        }
        printf("\n");
    }
	// Now we have an established connection.
	/*
	// XXX: Change the length if request string is modified!!!
	request = calloc(53+strlen(argv[optind+1])+strlen(argv[optind]),1);

	sprintf(request,"GET %s HTTP/1.0\nHost: %s\nUser-agent: simple-http client\n\n",argv[optind+1],argv[optind]);

	if (write(srvfd,request,strlen(request)) == -1)
        errExit("write to socket",1);
	
	shutdown(srvfd,SHUT_WR);

	while ( rwerr > 0 )
	{
		rwerr = read(srvfd,buf,16);
		if (write(1,buf,rwerr) == -1)
            errExit("write response to file",1);
	}
	
	close(srvfd);*/

	return 0;

}
