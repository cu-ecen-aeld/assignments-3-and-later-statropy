#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <syslog.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include "queue.h"

#define BLOCK_SIZE 4096

#define DATA_FILE "/var/tmp/aesdsocketdata"

typedef enum
{
    CLEAN_FD = 1,
    CLEAN_SERVER = 2,
    CLEAN_RES = 4,
    CLEAN_TIMER = 8,
} cleanupflags_t;

struct list_data_s
{
    struct sockaddr_in client;
    pthread_t thread;
    int conn_fd;
    int result;
    char *rx_buffer;
    SLIST_ENTRY(list_data_s)
    entry;
};

struct locked_file_s
{
    int fd;
    pthread_mutex_t mutex;
};

static struct locked_file_s logfile;
struct addrinfo *res = NULL;
static int server_conn;
static int cleanup_state = 0;
SLIST_HEAD(slisthead, list_data_s)
head;
timer_t periodic_timer;

void cleanup(int exit_code)
{
    if (cleanup_state & CLEAN_TIMER)
    {
        timer_delete(periodic_timer);
    }

    if (cleanup_state & CLEAN_RES)
    {
        freeaddrinfo(res);
    }

    if (cleanup_state & CLEAN_SERVER)
    {
        close(server_conn);
    }

    if (cleanup_state & CLEAN_FD)
    {
        close(logfile.fd);
        remove(DATA_FILE);
    }

    struct list_data_s *dat = NULL;
    while (!SLIST_EMPTY(&head))
    {
        dat = SLIST_FIRST(&head);
        if (dat->thread)
        {
            pthread_cancel(dat->thread);
            pthread_join(dat->thread, NULL);
        }
        SLIST_REMOVE_HEAD(&head, entry);
        free(dat);
    }

    closelog();
    exit(exit_code);
}

void exit_error(const char *message)
{
    syslog(LOG_ERR, "%s: %s", message, strerror(errno));
    cleanup(-1);
}

void signal_handler(int signum)
{
    syslog(LOG_INFO, "Caught signal, exiting");
    cleanup(0);
}

void open_data_file(void)
{
    logfile.fd = open(DATA_FILE, O_RDWR | O_TRUNC | O_CREAT, 0644);
    if (logfile.fd < 0)
    {
        exit_error("Failed to open logfile");
    }
    cleanup_state |= CLEAN_FD;
    pthread_mutex_init(&logfile.mutex, NULL);
}

void open_server(void)
{
    server_conn = socket(PF_INET, SOCK_STREAM, 0);
    if (server_conn < 0)
    {
        exit_error("Failed to create server socket %s");
    }
    cleanup_state |= CLEAN_SERVER;

    int reuse = 1;
    if (setsockopt(server_conn, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0)
    {
        exit_error("Could not set socket option");
    }
}

void bind_server(void)
{
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, "9000", &hints, &res) != 0)
    {
        exit_error("Failed to get addr info");
    }
    cleanup_state |= CLEAN_RES;

    if (bind(server_conn, res->ai_addr, sizeof(struct sockaddr)) != 0)
    {
        exit_error("Could not bind");
    }
}

void start_daemon(void)
{
    pid_t cpid = fork();
    if (cpid == -1)
    {
        exit_error("Could not fork deamon");
    }

    if (cpid != 0)
    {
        // parent exits
        exit(0);
    }

    if (setsid() == -1)
    {
        exit_error("Could not create new session/group");
    }

    if (chdir("/") == -1)
    {
        exit_error("Could change to root path");
    }

    open("/dev/null", O_RDWR);
    if (dup(0) == -1)
    {
        exit_error("Could not redirect stdout");
    }
    if (dup(0) == -1)
    {
        exit_error("Could not redirect stderr");
    }
}

void cleanup_thread(struct list_data_s *dat, int result_code)
{
    close(dat->conn_fd);
    free(dat->rx_buffer);
    dat->result = result_code;
    pthread_exit(NULL);
}

void thread_error(struct list_data_s *dat, const char *message)
{
    syslog(LOG_ERR, "(thread %s) %s: %s", inet_ntoa(dat->client.sin_addr), message, strerror(errno));
    cleanup_thread(dat, -1);
}

void *conn_handler(void *arg)
{
    struct list_data_s *dat = (struct list_data_s *)arg;

    int packet_len = 0;
    int next_size = BLOCK_SIZE;

    dat->rx_buffer = malloc(BLOCK_SIZE);
    if (dat->rx_buffer == NULL)
    {
        thread_error(dat, "malloc fail");
    }

    while (1)
    {

        int num_rx = recv(dat->conn_fd, &dat->rx_buffer[packet_len], next_size, 0);
        if (num_rx == -1)
        {
            close(dat->conn_fd);
            thread_error(dat, "Client connection failed");
        }
        int new_packet_len = packet_len + num_rx;

        if (dat->rx_buffer[new_packet_len - 1] == '\n')
        {
            // END OF PACKET
            packet_len = new_packet_len;
            break;
        }
        if (num_rx < next_size)
        {
            dat->rx_buffer[new_packet_len] = 0;
            char *eop = strchr(&dat->rx_buffer[packet_len], '\n');
            if (eop == NULL)
            {
                next_size -= num_rx;
                packet_len = new_packet_len;
            }
            else
            {
                // END OF PACKET
                packet_len = eop - dat->rx_buffer + 1;
                break;
            }
        }
        else
        {
            // alloc more space for next block
            packet_len = new_packet_len;
            next_size = BLOCK_SIZE;
            syslog(LOG_INFO, "realloc() %d", packet_len + BLOCK_SIZE);
            char *rptr = realloc(dat->rx_buffer, packet_len + BLOCK_SIZE);
            if (rptr == NULL)
            {
                close(dat->conn_fd);
                thread_error(dat, "malloc fail");
            }
            dat->rx_buffer = rptr;
        }
    }

    if (pthread_mutex_lock(&logfile.mutex) != 0)
    {
        thread_error(dat, "Could not get fs lock");
    }
    lseek(logfile.fd, 0, SEEK_END);
    int written = write(logfile.fd, dat->rx_buffer, packet_len);
    if (written == -1)
    {
        close(dat->conn_fd);
        pthread_mutex_unlock(&logfile.mutex);
        thread_error(dat, "failed to write to file");
    }

    off_t offset = 0;
    size_t len = lseek(logfile.fd, 0, SEEK_END);
    lseek(logfile.fd, 0, SEEK_SET);
    ssize_t socksent = sendfile(dat->conn_fd, logfile.fd, &offset, len);
    if (socksent == -1)
    {
        close(dat->conn_fd);
        pthread_mutex_unlock(&logfile.mutex);
        thread_error(dat, "sendfile fail");
    }

    if (pthread_mutex_unlock(&logfile.mutex) != 0)
    {
        close(dat->conn_fd);
        thread_error(dat, "failed to unlock logfile mutex");
    }

    syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(dat->client.sin_addr));

    cleanup_thread(dat, 1);
    return NULL;
}

void timer_handler(sigval_t v)
{
    struct locked_file_s *f = (struct locked_file_s *)v.sival_ptr;
    char tsbuffer[64];

    time_t t = time(NULL);
    struct tm *tms = localtime(&t);
    size_t len = strftime(tsbuffer, sizeof(tsbuffer) - 1, "%a, %d %b %Y %T %z", tms);
    tsbuffer[len] = '\n';
    pthread_mutex_lock(&f->mutex);
    lseek(f->fd, 0, SEEK_END);
    write(f->fd, "timestamp:", 10);
    write(f->fd, tsbuffer, len + 1);
    pthread_mutex_unlock(&f->mutex);
}

int main(int argc, char *argv[])
{
    openlog(NULL, 0, LOG_USER);

    if (argc > 2 || ((argc == 2) && strcmp(argv[1], "-d")))
    {
        syslog(LOG_ERR, "Invalid arguments");
        cleanup(-1);
    }

    if (signal(SIGINT, signal_handler) == SIG_ERR)
    {
        syslog(LOG_ERR, "Failed to register SIGINT handler");
        cleanup(-1);
    }

    if (signal(SIGTERM, signal_handler) == SIG_ERR)
    {
        syslog(LOG_ERR, "Failed to register SIGTERM handler");
        cleanup(-1);
    }

    open_data_file();

    open_server();

    bind_server();

    if (argc == 2)
    {
        start_daemon();
    }

    if (listen(server_conn, 10) != 0)
    {
        exit_error("Failed to listen");
    }

    SLIST_INIT(&head);

    struct sigevent sigev;
    memset(&sigev, 0, sizeof(sigev));
    sigev.sigev_notify = SIGEV_THREAD;
    sigev.sigev_value.sival_ptr = &logfile;
    sigev.sigev_notify_function = timer_handler;
    timer_create(CLOCK_REALTIME, &sigev, &periodic_timer);

    struct itimerspec its;
    its.it_interval.tv_sec = 10;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;

    timer_settime(periodic_timer, 0, &its, NULL);

    while (1)
    {
        struct list_data_s *dat = (struct list_data_s *)malloc(sizeof(struct list_data_s));
        if (dat == NULL)
        {
            exit_error("No thread memory available");
        }
        memset(dat, 0, sizeof(struct list_data_s));

        SLIST_INSERT_HEAD(&head, dat, entry);

        socklen_t socklen = sizeof(dat->client);
        dat->conn_fd = accept(server_conn, (struct sockaddr *)&dat->client, &socklen);
        if (dat->conn_fd == -1)
        {
            exit_error("Failed to accept");
        }

        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(dat->client.sin_addr));

        if (pthread_create(&dat->thread, NULL, conn_handler, dat) != 0)
        {
            exit_error("Could not create thread");
        }

        struct list_data_s *tmp = NULL;
        SLIST_FOREACH_SAFE(dat, &head, entry, tmp)
        {
            if (dat->result)
            {
                if (pthread_join(dat->thread, NULL) != 0)
                {
                    exit_error("Thread join failed");
                }
                SLIST_REMOVE(&head, dat, list_data_s, entry);
                free(dat);
            }
        }
    }
    exit_error("Execution reached end of function");
}