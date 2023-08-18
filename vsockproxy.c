#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <sys/select.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <arpa/inet.h>

#define MAX_EVENTS 64
#define RECV_BUF_SIZE (1024 * 1024)
#define STAT_INTERVAL 1.0

struct client {
    int sock;
    int cid;
    int port;
    int rx;
    int connected;
    struct client *sibling;
    char *buf;
    int buf_size;
    uint64_t rxbytes;
    uint64_t txbytes;
    double rxbegin;
    double txbegin;
};

double timespec_now()
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (ts.tv_sec * 1000000000ull + ts.tv_nsec) / 1000000000.0;
}

void close_connections(struct client *data)
{
    if (data == NULL)
        return;

    printf("Connection %d %s cid %d on port %d closed\n", data->sock, data->rx ? "from" : "to", data->cid, data->port);
    close(data->sock);
    if (data->sibling) {
        data->sibling->sibling = NULL;
        close_connections(data->sibling);
    }
    free(data->buf);
    data->buf = NULL;
    free(data);
    data = NULL;
}

void start_receiving(struct client *data, int epfd)
{
    struct epoll_event rx_ev;
    rx_ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    rx_ev.data.ptr = data;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, data->sock, &rx_ev) == -1) {
        printf("Failed to add to epoll: %s\n", strerror(errno));
        close_connections(data);
        return;
    }

    struct epoll_event tx_ev;
    tx_ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    tx_ev.data.ptr = data->sibling;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, data->sibling->sock, &tx_ev) == -1) {
        printf("Failed to add to epoll: %s\n", strerror(errno));
        close_connections(data->sibling);
        return;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: vsockproxy <local_port> <remote_cid> <remote_port>\n");
        exit(1);
    }

    // Disable stdout buffering so that journald can show live data
    setvbuf(stdout, NULL, _IONBF, 0);

    int local_port = atoi(argv[1]);
    int remote_cid = atoi(argv[2]);
    int remote_port = atoi(argv[3]);
    printf("vsockproxy started (local port %d, remote cid %d, remote port %d)\n", local_port, remote_cid, remote_port);

    int listen_sock = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (listen_sock == -1) {
        printf("Failed to create socket: %s\n", strerror(errno));
        exit(1);
    }

    struct sockaddr_vm addr = {};
    addr.svm_family = AF_VSOCK;
    addr.svm_port = local_port;
    addr.svm_cid = VMADDR_CID_ANY;
    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        printf("Failed to bind: %s\n", strerror(errno));
        exit(1);
    }

    if (fcntl(listen_sock, F_SETFL, fcntl(listen_sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
		printf("Failed to set non block: %s\n", strerror(errno));
        exit(1);
	}

    if (listen(listen_sock, 10) == -1) {
        printf("Failed to listen: %s\n", strerror(errno));
        exit(1);
    }

    int epfd = epoll_create(1);
    if (epfd == -1) {
        printf("Failed to create epoll: %s\n", strerror(errno));
        exit(1);
    }

    struct client *data = malloc(sizeof(struct client));
    data->sock = listen_sock;
    struct epoll_event listen_ev;
	listen_ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
	listen_ev.data.ptr = data;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &listen_ev) == -1) {
		printf("Failed to add to epoll: %s\n", strerror(errno));
        exit(1);
    }

    struct epoll_event events[MAX_EVENTS];
    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            printf("Failed to wait: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            struct client *data = (struct client *)events[i].data.ptr;

			if (data->sock == listen_sock) {
                // New connection
				struct sockaddr_vm clientaddr;
                socklen_t socklen = sizeof(clientaddr);
				int rx_sock = accept(listen_sock, (struct sockaddr *)&clientaddr, &socklen);
                if (rx_sock == -1) {
                    printf("Failed to accept a new connection: %s\n", strerror(errno));
                    break;
                }

                printf("Accepted connection %d from cid %d on port %d \n", rx_sock, clientaddr.svm_cid, ntohs(clientaddr.svm_port));
				if (fcntl(rx_sock, F_SETFL, fcntl(rx_sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
                    printf("Failed to set non block: %s\n", strerror(errno));
                    close(rx_sock);
                    break;
                }

                struct client *rx_data = malloc(sizeof(struct client));
                memset(rx_data, 0, sizeof(struct client));
                rx_data->sock = rx_sock;
                rx_data->cid = clientaddr.svm_cid;
                rx_data->port = ntohs(clientaddr.svm_port);
                rx_data->rx = 1;
                rx_data->connected = 1;
                rx_data->buf = malloc(RECV_BUF_SIZE);
                rx_data->rxbegin = timespec_now();
                rx_data->txbegin = timespec_now();

                // Connect to uplink
                int tx_sock = socket(AF_VSOCK, SOCK_STREAM, 0);
                if (tx_sock == -1) {
                    printf("Failed to create remote socket: %s\n", strerror(errno));
                    close_connections(rx_data);
                    continue;
                }

                if (fcntl(tx_sock, F_SETFL, fcntl(tx_sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
                    printf("Failed to set non block: %s\n", strerror(errno));
                    close(tx_sock);
                    close_connections(rx_data);
                    continue;
                }

                struct sockaddr_vm remoteaddr = {};
                remoteaddr.svm_family = AF_VSOCK;
                remoteaddr.svm_port = remote_port;
                remoteaddr.svm_cid = remote_cid;
                int res = connect(tx_sock, (struct sockaddr *) &remoteaddr, sizeof(remoteaddr));
                if (res < 0 && errno != EINPROGRESS) {
                    printf("Failed to connect to cid %d on port %d: %s\n", remote_cid, remote_port, strerror(errno));
                    close(tx_sock);
                    close_connections(rx_data);
                    continue;
                }

                struct client *tx_data = malloc(sizeof(struct client));
                memset(tx_data, 0, sizeof(struct client));
                tx_data->sock = tx_sock;
                tx_data->cid = remote_cid;
                tx_data->port = remote_port;
                tx_data->rx = 0;
                tx_data->buf = malloc(RECV_BUF_SIZE);
                tx_data->sibling = rx_data;
                tx_data->rxbegin = timespec_now();
                tx_data->txbegin = timespec_now();
                rx_data->sibling = tx_data;

                if (res == 0) {
                    printf("Connection %d to cid %d on port %d has been established\n", tx_sock, remote_cid, remote_port);
                    tx_data->connected = 1;
                    start_receiving(rx_data, epfd);
                }
                else {
                    printf("Connection %d to cid %d on port %d is in progress\n", tx_sock, remote_cid, remote_port);

                    // Connection status will be reported in EPOLLOUT
                    struct epoll_event tx_ev;
                    tx_ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                    tx_ev.data.ptr = tx_data;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tx_sock, &tx_ev) == -1) {
                        printf("Failed to add to epoll: %s\n", strerror(errno));
                        close_connections(rx_data);
                        continue;
                    }
                }
			}
            else {
                if (events[i].events & EPOLLIN) {
				    while (1) {
                        int res = read(data->sock, data->buf + data->buf_size, RECV_BUF_SIZE - data->buf_size);
                        if (res == 0) {
                            //printf("End of file: %d\n", data->sock);
                            break;
                        }
                        else if (res < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                printf("Read failed: %s\n", strerror(errno));
                                close_connections(data);
                            }
                            break;
                        }
                        else {
                            data->buf_size += res;
                            int res = send(data->sibling->sock, data->buf, data->buf_size, 0);
                            if (res <= 0) {
                                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                    printf("Failed to send from epollin (%d): %s\n", errno, strerror(errno));
                                    close_connections(data);
                                    break;
                                }
                            }
                            else {
                                data->buf_size -= res;
                                memmove(data->buf, data->buf + res, data->buf_size);
                                data->sibling->txbytes += res;
                            }

                            // Calculate statistics
                            data->rxbytes += res;
                            double rxtime_spent = timespec_now() - data->rxbegin;
                            double txtime_spent = timespec_now() - data->txbegin;
                            if (rxtime_spent >= STAT_INTERVAL) {
                                printf("Connection %d RX %ld bytes in %.2f seconds, transfer rate %.2f MB/s, data size %d\n", data->sock, data->rxbytes, rxtime_spent, data->rxbytes / rxtime_spent / 1000000, res);
                                printf("Connection %d TX %ld bytes in %.2f seconds, transfer rate %.2f MB/s, data size %d\n", data->sock, data->txbytes, txtime_spent, data->txbytes / txtime_spent / 1000000, res);
                                data->rxbytes = 0;
                                data->rxbegin = timespec_now();
                                data->txbytes = 0;
                                data->txbegin = timespec_now();
                            }
                        }
                    }
                }
                if (events[i].events & EPOLLOUT) {
                    if (!data->connected) {
                        int res;
                        socklen_t res_len = sizeof(res);
                        if (getsockopt(data->sock, SOL_SOCKET, SO_ERROR, &res, &res_len) < 0) {
                            printf("Failed to get connection status: %s\n", strerror(errno));
                            close_connections(data);
                            continue;
                        }

                        if (res != 0) {
                            printf("Failed to connect to cid %d on port %d: %s\n", remote_cid, remote_port, strerror(errno));
                            close_connections(data);
                            continue;
                        }

                        printf("Connection %d to cid %d on port %d has been established\n", data->sock, remote_cid, remote_port);
                        data->connected = 1;
                        if (epoll_ctl(epfd, EPOLL_CTL_DEL, data->sock, NULL) == -1) {
                            printf("Failed to delete from epoll: %s\n", strerror(errno));
                            close_connections(data);
                        }
                        else
                            start_receiving(data, epfd);
                    }
                    else {
                        while (data->sibling->buf_size > 0) {
                            int res = send(data->sock, data->sibling->buf, data->sibling->buf_size, 0);
                            if (res <= 0) {
                                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                    printf("Failed to send from epollout (%d): %s\n", errno, strerror(errno));
                                    close_connections(data);
                                }
                                break;
                            }
                            else {
                                data->sibling->buf_size -= res;
                                memmove(data->sibling->buf, data->sibling->buf + res, data->sibling->buf_size);
                            }
                        }
                    }
                }
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    //printf("Сonnection %d closed\n", data->sock);
                    close_connections(data);
                }
            }
        }
    }
    close(epfd);
    printf("vsockproxy finished\n");
    return 0;
}
