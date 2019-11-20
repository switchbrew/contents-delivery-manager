#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <getopt.h>
#include <errno.h>

#ifndef __WIN32__
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
typedef uint32_t in_addr_t;
#endif

#include "../common/delivery.h"

//---------------------------------------------------------------------------------
void showHelp() {
//---------------------------------------------------------------------------------
    puts("Usage: contents_delivery_manager [options]\n");
    puts("--help,    -h   Display this information.");
    puts("--server,  -s   Run as a server (default).");
    puts("--client,  -c   Run as a client.");
    puts("--address, -a   Hostname or IPv4 address to bind/connect to. With server the default is 0.0.0.0.");
    puts("--port,    -p   Port, the default is 55556.");
    puts("--datadir, -d   Sysupdate data dir path.");
    puts("\n");
}


//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
    int res=0;
    DeliveryManager manager={0};
    char *address = NULL;
    char *datadir = NULL;
    char *endarg = NULL;
    static int server=1;
    u16 port=DELIVERY_PORT_DEFAULT;

    if (argc < 2) {
        showHelp();
        return 1;
    }

    while(1) {
        static struct option long_options[] = {
            {"help",    no_argument,       0,       'h'},
            {"server",  no_argument,       &server,  1 },
            {"client",  no_argument,       &server,  0 },
            {"address", required_argument, 0,       'a'},
            {"port",    required_argument, 0,       'p'},
            {"datadir", required_argument, 0,       'd'},
            {0, 0, 0, 0}
        };

        /* getopt_long stores the option index here. */
        int option_index = 0, c;

        c = getopt_long (argc, argv, "hsca:p:d:", long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
        break;

        switch(c) {

        case 'h':
            showHelp();
            break;
        case 'a':
            address = optarg;
            break;
        case 'p':
            errno = 0;
            port = strtoul(optarg, &endarg, 0);
            if (endarg == optarg) errno = EINVAL;
            if (errno != 0) {
                perror("--port");
                return 1;
            }
            break;
        case 'd':
            datadir = optarg;
            break;
        }

    }

    if (datadir== NULL) {
        showHelp();
        return 1;
    }

#ifdef __WIN32__
    WSADATA wsa_data;
    if (WSAStartup (MAKEWORD(2,2), &wsa_data)) {
        printf ("WSAStartup failed\n");
        return 1;
    }
#endif

    struct in_addr nxaddr;
    nxaddr.s_addr  =  INADDR_NONE;

    if (address) {
        struct addrinfo *info;
        if (getaddrinfo(address, NULL, NULL, &info) == 0) {
            nxaddr = ((struct sockaddr_in*)info->ai_addr)->sin_addr;
            freeaddrinfo(info);
        }
    }
    else if (server)
        nxaddr.s_addr = htonl(INADDR_ANY);

    if (nxaddr.s_addr == INADDR_NONE) {
        fprintf(stderr,"Invalid address.\n");
        return 1;
    }

    res = deliveryManagerCreate(&manager, server, &nxaddr, port);
    if (res!=0) printf("deliveryManagerCreate() failed: %d\n", res);
    if (res==0) {
        deliveryManagerClose(&manager);
    }

#ifdef __WIN32__
    WSACleanup ();
#endif
    return res;
}

