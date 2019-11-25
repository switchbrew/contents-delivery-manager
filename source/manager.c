#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <getopt.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

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

struct content_transfer_state {
    FILE *f;
};

// TODO: Improve this.
Result handler_meta_load(void* userdata, const char* filepath, void** outbuf_ptr, size_t *out_filesize) {
    Result rc=0;
    const char *tmpdir = "tmpdir";
    char tmpstr[PATH_MAX+257];
    char dirpath[PATH_MAX];
    memset(tmpstr, 0, sizeof(tmpstr));
    memset(dirpath, 0, sizeof(dirpath));
    snprintf(dirpath, sizeof(dirpath)-1, "%s/section0", tmpdir);
    snprintf(tmpstr, sizeof(tmpstr)-1, "hactool --section0dir=%s \"%s\" > %s/hactool_out 2>&1", dirpath, filepath, tmpdir); // TODO: proper tmpdir

#ifdef _WIN32
    _mkdir(tmpdir);
#else
    mkdir(tmpdir, 0777);
#endif

    if (system(tmpstr) != 0) return MAKERESULT(Module_Nim, NimError_BadInput);

    rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);

    DIR* dir;
    struct dirent* dp;
    dir = opendir(dirpath);
    if (!dir) return rc;

    while ((dp = readdir(dir))) {
        if (dp->d_name[0]=='.')
            continue;

        bool entrytype=0;

        memset(tmpstr, 0, sizeof(tmpstr));
        snprintf(tmpstr, sizeof(tmpstr)-1, "%s%s%s", dirpath, "/", dp->d_name);

        struct stat tmpstat;
        if(stat(tmpstr, &tmpstat)==-1)
            continue;

        entrytype = (tmpstat.st_mode & S_IFMT) != S_IFREG;

        if (entrytype) continue;

        if (strncmp(&dp->d_name[strlen(dp->d_name)-5], ".cnmt", 5)!=0) continue;

        *out_filesize = tmpstat.st_size;
        *outbuf_ptr = malloc(tmpstat.st_size);
        if (*outbuf_ptr == NULL) {
            rc = MAKERESULT(Module_Nim, NimError_BadInput);
            break;
        }

        FILE *f = fopen(tmpstr, "rb");
        if (!f) {
            free(*outbuf_ptr);
            *outbuf_ptr = NULL;
            rc = MAKERESULT(Module_Nim, NimError_BadInput);
            break;
        }
        if (fread(*outbuf_ptr, 1, tmpstat.st_size, f) != tmpstat.st_size) rc = MAKERESULT(Module_Nim, NimError_BadInput);
        fclose(f);

        unlink(tmpstr);

        rc = 0;
        break;
    }

    closedir(dir);
    return rc;
}

Result handler_meta_record(void* userdata, NcmPackagedContentInfo* record, const NcmContentMetaKey* content_meta_key) {
    Result rc=0;
    struct DeliveryContentEntry *entry = NULL;

    rc = deliveryManagerGetContentEntry((DeliveryManager*)userdata, &entry, content_meta_key, NULL);
    if (R_SUCCEEDED(rc)) memcpy(record, &entry->content_info, sizeof(NcmPackagedContentInfo));
    return rc;
}

Result content_transfer_init(struct DeliveryGetContentDataTransferState* state, s64* content_size) {
    Result rc=0;
    struct content_transfer_state *user_state = (struct content_transfer_state*)state->userdata;
    struct DeliveryContentEntry *entry = NULL;

    if (!state->manager->server) printf("content_size: 0x%lx\n", *content_size);
    else {
        rc = deliveryManagerGetContentEntry(state->manager, &entry, NULL, &state->arg->content_id);
        if (R_SUCCEEDED(rc)) {
            user_state->f = fopen(entry->filepath, "rb");
            if (user_state->f == NULL) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        }
        if (R_SUCCEEDED(rc)) *content_size = entry->filesize;
    }

    return rc;
}

void content_transfer_exit(struct DeliveryGetContentDataTransferState* state) {
    struct content_transfer_state *user_state = (struct content_transfer_state*)state->userdata;
    if (user_state->f) {
        fclose(user_state->f);
        user_state->f = NULL;
    }
}

Result content_transfer(struct DeliveryGetContentDataTransferState* state, void* buffer, u64 size, s64 offset) {
    Result rc=0;
    struct content_transfer_state *user_state = (struct content_transfer_state*)state->userdata;
    printf("transfer: 0x%lx, 0x%lx\n", size, offset);
    if (state->manager->server) {
        if (fseek(user_state->f, offset, SEEK_SET)==-1) rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        if (fread(buffer, 1, size, user_state->f) != size) rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
    }
    else {
        printf("data: ");
        for (u64 i=0; i<size; i++) printf("%02X", ((u8*)buffer)[i]);
        printf("\n");
    }
    return rc;
}

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
    puts("--depth,   -e   Sysupdate data dir scanning depth, the default is 3.");
    puts("\n");
}


//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
    int ret=0;
    Result rc=0;
    DeliveryManager manager={0};
    struct content_transfer_state transfer_state={0};
    char *address = NULL;
    char *datadir = NULL;
    char *endarg = NULL;
    static int server=1;
    u16 port=DELIVERY_PORT_DEFAULT;
    s32 depth=3;

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
            {"depth",   required_argument, 0,       'e'},
            {0, 0, 0, 0}
        };

        /* getopt_long stores the option index here. */
        int option_index = 0, c;

        c = getopt_long (argc, argv, "hsca:p:d:e:", long_options, &option_index);

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
        case 'e':
            errno = 0;
            depth = strtoul(optarg, &endarg, 0);
            if (endarg == optarg) errno = EINVAL;
            if (errno != 0) {
                perror("--depth");
                return 1;
            }
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

    if (server && datadir==NULL) {
        fprintf(stderr,"datadir is required.\n");
        return 1;
    }

    rc = deliveryManagerCreate(&manager, server, &nxaddr, port);
    if (R_FAILED(rc)) printf("deliveryManagerCreate() failed: 0x%x\n", rc);
    if (R_SUCCEEDED(rc)) {
        deliveryManagerSetHandlerGetMetaContentRecord(&manager, handler_meta_record, &manager);
        deliveryManagerSetHandlersGetContent(&manager, &transfer_state, content_transfer_init, content_transfer_exit, content_transfer);
        if (server) {
            rc = deliveryManagerScanDataDir(&manager, datadir, depth, handler_meta_load, NULL);
            if (R_FAILED(rc)) printf("deliveryManagerScanDataDir() failed: 0x%x\n", rc);

            if (R_SUCCEEDED(rc)) {
                rc = deliveryManagerRequestRun(&manager);
                if (R_FAILED(rc)) printf("deliveryManagerRequestRun() failed: 0x%x\n", rc);
            }

            if (R_SUCCEEDED(rc)) printf("Server started.\n");

            if (R_SUCCEEDED(rc)) {
                // We could use deliveryManagerGetProgress() to print the progress, but don't bother - would also have to handle waiting for the task to finish differently, since deliveryManagerGetResult() blocks until it's done.
                rc = deliveryManagerGetResult(&manager);
                printf("deliveryManagerGetResult(): 0x%x\n", rc);
            }
        }
        else {
            printf("Connected to server.\n");

            rc = deliveryManagerClientRequestExit(&manager);
            printf("deliveryManagerClientRequestExit(): 0x%x\n", rc);
        }

        deliveryManagerClose(&manager);
    }
    if (R_FAILED(rc)) ret = 1;

#ifdef __WIN32__
    WSACleanup ();
#endif
    return ret;
}

