/***
 *
 *  examples/xenomai/posix/eth_p_all.c
 *
 *  ETH_P_ALL receiver - listens for all incoming packets and dumps them
 *
 *  Copyright (C) 2006 Jan Kiszka <jan.kiszka@web.de>
 *
 *  RTnet - real-time networking example
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/ether.h>

char buffer[10*1024];
int sock;


void catch_signal(int sig)
{
    close(sock);
}


int main(int argc, char *argv[])
{
    struct sched_param param = { .sched_priority = 1 };
    ssize_t len;
    struct sockaddr_ll addr;
    struct ether_header *eth = (struct ether_header *)buffer;


    signal(SIGTERM, catch_signal);
    signal(SIGINT, catch_signal);
    signal(SIGHUP, catch_signal);
    mlockall(MCL_CURRENT|MCL_FUTURE);

    if ((sock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) < 0) {
        perror("socket cannot be created");
        return 1;
    }

    if (argc > 1) {
        struct ifreq ifr;

        strncpy(ifr.ifr_name, argv[1], IFNAMSIZ);
        if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
            perror("cannot get interface index");
            close(sock);
            return 1;
        }

        addr.sll_family   = AF_PACKET;
        addr.sll_protocol = ETH_P_ALL;
        addr.sll_ifindex  = ifr.ifr_ifindex;

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("cannot bind to local ip/port");
            close(sock);
            return 1;
        }
    }

    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    while (1) {
        len = recv(sock, buffer, sizeof(buffer), 0);
        if (len < 0)
            break;

        printf("from: %s type: %04x length=%zd\n",
               ether_ntoa((struct ether_addr *)eth->ether_shost),
               ntohs(eth->ether_type), len);
    }

    /* This call also leaves primary mode, required for socket cleanup. */
    printf("shutting down\n");

    /* Note: The following loop is no longer required since Xenomai 2.4. */
    while ((close(sock) < 0) && (errno == EAGAIN)) {
        printf("socket busy - waiting...\n");
        sleep(1);
    }

    return 0;
}
