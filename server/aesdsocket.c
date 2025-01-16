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

#define BLOCK_SIZE 4096

#define DATA_FILE "/var/tmp/aesdsocketdata"

typedef enum
{
    CLEAN_FD = 1,
    CLEAN_SERVER = 2,
    CLEAN_RES = 4,
    CLEAN_BUFFER = 8
} cleanupflags_t;

static int fd;
struct addrinfo *res = NULL;
static int server_conn;
static int conn_fd;
static char *rx_buffer = NULL;
static int cleanup_state = 0;

void cleanup(int exit_code)
{
    if (cleanup_state & CLEAN_BUFFER)
    {
        free(rx_buffer);
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
        close(fd);
        remove(DATA_FILE);
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
    fd = open(DATA_FILE, O_RDWR | O_TRUNC | O_CREAT, 0644);
    if (fd < 0)
    {
        exit_error("Failed to open logfile");
    }
    cleanup_state |= CLEAN_FD;
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

    while (1)
    {
        rx_buffer = realloc(rx_buffer, BLOCK_SIZE);
        if (rx_buffer == NULL)
        {
            exit_error("malloc fail");
        }
        cleanup_state |= CLEAN_BUFFER;

        struct sockaddr_in client;
        socklen_t socklen = sizeof(client);
        conn_fd = accept(server_conn, (struct sockaddr *)&client, &socklen);
        if (conn_fd == -1)
        {
            exit_error("Failed to accept");
        }

        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client.sin_addr));

        int packet_len = 0;
        int next_size = BLOCK_SIZE;

        while (1)
        {

            int num_rx = recv(conn_fd, &rx_buffer[packet_len], next_size, 0);
            if (num_rx == -1)
            {
                close(conn_fd);
                exit_error("Client connection failed");
            }
            int new_packet_len = packet_len + num_rx;

            if (rx_buffer[new_packet_len - 1] == '\n')
            {
                // END OF PACKET
                packet_len = new_packet_len;
                break;
            }
            if (num_rx < next_size)
            {
                rx_buffer[new_packet_len] = 0;
                char *eop = strchr(&rx_buffer[packet_len], '\n');
                if (eop == NULL)
                {
                    next_size -= num_rx;
                    packet_len = new_packet_len;
                }
                else
                {
                    // END OF PACKET
                    packet_len = eop - rx_buffer + 1;
                    break;
                }
            }
            else
            {
                // alloc more space for next block
                packet_len = new_packet_len;
                next_size = BLOCK_SIZE;
                rx_buffer = realloc(rx_buffer, packet_len + BLOCK_SIZE);
                if (rx_buffer == NULL)
                {
                    close(conn_fd);
                    exit_error("malloc fail");
                }
            }
        }

        lseek(fd, 0, SEEK_END);
        int written = write(fd, rx_buffer, packet_len);
        if (written == -1)
        {
            exit_error("failed to write to file");
            close(conn_fd);
        }

        off_t offset = 0;
        size_t len = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        ssize_t socksent = sendfile(conn_fd, fd, &offset, len);
        if (socksent == -1)
        {
            close(conn_fd);
            exit_error("sendfile fail");
        }

        close(conn_fd);
        syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(client.sin_addr));
    }

    return -1;
}
