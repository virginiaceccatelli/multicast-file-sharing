#ifndef __MULTICAST_H__
#define __MULTICAST_H__

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>

typedef struct _mcast_t
{
    struct sockaddr_in addr; // destination multicast group address
    struct sockaddr_in my_addr; // local address (for receiving)
    unsigned int addrlen;
    unsigned int my_addrlen;
    int sock; // socket file descriptor
    struct ip_mreq mreq; // multicast group membership info
    struct pollfd fds[2]; // for polling (non-blocking check)
    int nfds; // number of fds used
} mcast_t;

mcast_t *multicast_init(char *mcast_addr, int sport, int rport);
int multicast_send(mcast_t *m, void *msg, int msglen);
void multicast_setup_recv(mcast_t *m);
int multicast_receive(mcast_t *m, void *buf, int bufsize);
int multicast_check_receive(mcast_t *m);
void multicast_destroy(mcast_t *m);

#endif