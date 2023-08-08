
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

#define RECV_BUF_SIZE (1024 * 1024)
#define STAT_INTERVAL 1.0

double timespec_now()
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (ts.tv_sec * 1000000000ull + ts.tv_nsec) / 1000000000.0;
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

    signal(SIGCHLD, SIG_IGN);

    int sock = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (sock == -1) {
        printf("Failed to create socket: %s\n", strerror(errno));
        exit(1);
    }

    struct sockaddr_vm addr = {};
    addr.svm_family = AF_VSOCK;
    addr.svm_port = local_port;
    addr.svm_cid = VMADDR_CID_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        printf("Failed to bind: %s\n", strerror(errno));
        exit(1);
    }

    if (listen(sock, 10) == -1) {
        printf("Failed to listen: %s\n", strerror(errno));
        exit(1);
    }

    while (1) {
        int clientsock = accept(sock, NULL, NULL);
        if (clientsock == -1) {
            printf("Failed to accept: %s\n", strerror(errno));
            exit(1);
        }

        pid_t pid = fork();
        if (pid == -1) {
            printf("Failed to fork: %s\n", strerror(errno));
            exit(1);
        }
        else if (pid == 0) {
            printf("Accepted connection (id %d)\n", getpid());

            int remotesock = socket(AF_VSOCK, SOCK_STREAM, 0);
            if (remotesock == -1) {
                printf("Failed to create remote socket (id %d): %s\n", getpid(), strerror(errno));
                exit(1);
            }

            struct sockaddr_vm remoteaddr = {};
	        remoteaddr.svm_family = AF_VSOCK;
	        remoteaddr.svm_port = remote_port;
	        remoteaddr.svm_cid = remote_cid;

            if (connect(remotesock, (struct sockaddr *) &remoteaddr, sizeof(remoteaddr)) == -1) {
                printf("Failed to connect to remote socket (cid %d port %d): %s\n", remote_cid, remote_port, strerror(errno));
                exit(1);
            }

            uint64_t rxbytes = 0, txbytes = 0;
            double rxbegin = timespec_now();
            double txbegin = timespec_now();
            char *recvbuf = malloc(RECV_BUF_SIZE);

            while(1) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(clientsock, &rfds);
                FD_SET(remotesock, &rfds);

                struct timeval tv;
                tv.tv_sec = 5;
                tv.tv_usec = 0;

                int ret = select((clientsock > remotesock ? clientsock : remotesock) + 1, &rfds, NULL, NULL, &tv);
                if (ret < 0) {
                    printf("select failed (id %d): %s\n", getpid(), strerror(errno));
                    break;
                }
                else if (ret) {
                    if (FD_ISSET(clientsock, &rfds)) {
                        ret = recv(clientsock, recvbuf, RECV_BUF_SIZE, 0);
                        if(ret < 0) {
                            printf("recv failed (id %d): %s\n", getpid(), strerror(errno));
                            break;
                        }
                        else if (ret > 0) {
                            if (send(remotesock, recvbuf, ret, 0) != ret) {
                                printf("send failed (id %d): %s\n", getpid(), strerror(errno));
                                break;
                            }
                            // Calculate statistics
                            rxbytes += ret;
                            double rxtime_spent = timespec_now() - rxbegin;
                            if (rxtime_spent >= STAT_INTERVAL) {
                                printf("RX %ld bytes in %.2f seconds, transfer rate %.2f MB/s, data size %d\n", rxbytes, rxtime_spent, rxbytes / rxtime_spent / 1000000, ret);
                                rxbytes = 0;
                                rxbegin = timespec_now();
                            }
                        }
                    }
                    if (FD_ISSET(remotesock, &rfds)) {
                        ret = recv(remotesock, recvbuf, RECV_BUF_SIZE, 0);
                        if(ret < 0) {
                            printf("recv failed (id %d): %s\n", getpid(), strerror(errno));
                            break;
                        }
                        else if(ret > 0) {
                            if (send(clientsock, recvbuf, ret, 0) != ret) {
                                printf("send failed (id %d): %s\n", getpid(), strerror(errno));
                                break;
                            }
                            // Calculate statistics
                            txbytes += ret;
                            double txtime_spent = timespec_now() - txbegin;
                            if (txtime_spent >= STAT_INTERVAL) {
                                printf("TX %ld bytes in %.2f seconds, transfer rate %.2f MB/s, data size %d\n", txbytes, txtime_spent, txbytes / txtime_spent / 1000000, ret);
                                txbytes = 0;
                                txbegin = timespec_now();
                            }
                        }
                    }
                }
            }

            free(recvbuf);
            exit(0);
        }

        close(clientsock);
    }

    printf("vsockproxy finished\n");
    return 0;
}
