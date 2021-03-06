#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

#ifndef __WIN32__

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#else

#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>

typedef int socklen_t;
typedef uint32_t in_addr_t;


#define mkdir(dir, mode) _mkdir(dir)

#endif // __WIN32__

#include "../common/delivery.h"

struct content_transfer_state {
    FILE *f;
};

Result handler_meta_load(void* userdata, struct DeliveryContentEntry *entry, const char* filepath, void** outbuf_ptr, size_t *out_filesize) {
    Result rc=0;
    char *tmpdir = (char*)userdata;
    char tmpstr[PATH_MAX+PATH_MAX+40];
    char dirpath[PATH_MAX];
    char redir_path[PATH_MAX];
    memset(tmpstr, 0, sizeof(tmpstr));
    memset(dirpath, 0, sizeof(dirpath));
    memset(redir_path, 0, sizeof(redir_path));
    snprintf(dirpath, sizeof(dirpath)-1, "%s/section0", tmpdir);
    snprintf(redir_path, sizeof(redir_path)-1, "%s/hactool_out", tmpdir);
    snprintf(tmpstr, sizeof(tmpstr)-1, "hactool \"--section0dir=%s\" \"%s\" > \"%s\" 2>&1", dirpath, filepath, redir_path);

    mkdir(tmpdir, 0777);

    if (system(tmpstr) != 0) return MAKERESULT(Module_Nim, NimError_BadInput);

    rc = deliveryManagerLoadMetaFromFs(dirpath, outbuf_ptr, out_filesize, true);
    rmdir(dirpath);
    unlink(redir_path);
    rmdir(tmpdir);
    return rc;
}

Result handler_meta_packaged_content_info(void* userdata, NcmPackagedContentInfo* meta_content_info, const NcmContentMetaKey* content_meta_key) {
    Result rc=0;
    struct DeliveryContentEntry *entry = NULL;

    rc = deliveryManagerGetContentEntry((DeliveryManager*)userdata, &entry, content_meta_key, NULL);
    if (R_SUCCEEDED(rc)) memcpy(meta_content_info, &entry->content_info, sizeof(NcmPackagedContentInfo));
    return rc;
}

// The client handling in these are for testing, proper client handlers would write to a file / whatever.
Result content_transfer_init(struct DeliveryGetContentDataTransferState* state, s64* content_size) {
    Result rc=0;
    struct content_transfer_state *user_state = (struct content_transfer_state*)state->userdata;
    struct DeliveryContentEntry *entry = NULL;

    if (!state->manager->server) printf("content_size: 0x%"PRIx64"\n", *content_size);
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

    if (state->manager->server) {
        if (fseek(user_state->f, offset, SEEK_SET)==-1) rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        if (R_SUCCEEDED(rc)) {
            if (fread(buffer, 1, size, user_state->f) != size) rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        }
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
    puts("--log,     -l   Enable logging to the specified file, or if path not specified stdout.");
    puts("--server,  -s   Run as a server (default).");
    puts("--client,  -c   Run as a client.");
    puts("--address, -a   Hostname or IPv4 address to bind/connect to. With server the default is 0.0.0.0.");
    puts("--port,    -p   Port, the default is 55556.");
    puts("--datadir, -d   Sysupdate data dir path. Required with server-mode.");
    puts("--depth,   -e   Sysupdate data dir scanning depth, the default is 3.");
    puts("--tmpdir,  -t   Temporary directory path used during datadir scanning, this will be automatically deleted when usage is finished. The default is 'tmpdir'.");
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
    FILE *log_file = NULL;
    const char *log_filepath = NULL;
    char *tmpdir_path = "tmpdir";

    printf("contents_delivery_manager v%s\n", VERSION);

    if (argc < 2) {
        showHelp();
        return 1;
    }

    while(1) {
        static struct option long_options[] = {
            {"help",    no_argument,       0,       'h'},
            {"log",     optional_argument, 0,       'l'},
            {"server",  no_argument,       &server,  1 },
            {"client",  no_argument,       &server,  0 },
            {"address", required_argument, 0,       'a'},
            {"port",    required_argument, 0,       'p'},
            {"datadir", required_argument, 0,       'd'},
            {"depth",   required_argument, 0,       'e'},
            {"tmpdir",  required_argument, 0,       't'},
            {0, 0, 0, 0}
        };

        /* getopt_long stores the option index here. */
        int option_index = 0, c;

        c = getopt_long (argc, argv, "hl::sca:p:d:e:t:", long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
        break;

        switch(c) {

        case 'h':
            showHelp();
            break;
        case 'l':
            log_filepath = optarg;
            if (log_filepath==NULL) log_file = stdout;
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
        case 't':
            tmpdir_path = optarg;
            break;
        }
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
        fprintf(stderr, "Invalid address.\n");
        ret = 1;
    }

    if (ret==0 && server && datadir==NULL) {
        fprintf(stderr, "datadir is required.\n");
        ret = 1;
    }

    if (ret==0 && log_filepath) {
        log_file = fopen(log_filepath, "w");
        if (log_file==NULL) {
            fprintf(stderr, "Failed to open log_filepath.\n");
            ret = 1;
        }
    }

    if (ret==0) {
        rc = deliveryManagerCreate(&manager, server, &nxaddr, port);
        if (R_FAILED(rc)) printf("deliveryManagerCreate() failed: 0x%x\n", rc);
        if (R_SUCCEEDED(rc)) {
            if (log_file) deliveryManagerSetLogFile(&manager, log_file);
            deliveryManagerSetHandlerGetMetaPackagedContentInfo(&manager, handler_meta_packaged_content_info, &manager);
            deliveryManagerSetHandlersGetContent(&manager, &transfer_state, content_transfer_init, content_transfer_exit, content_transfer);
            if (server) {
                printf("Scanning datadir...\n");
                rc = deliveryManagerScanDataDir(&manager, datadir, depth, handler_meta_load, tmpdir_path);
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

                // This is for testing, an actual client would use the various deliveryManagerClient*() funcs.

                rc = deliveryManagerClientRequestExit(&manager);
                printf("deliveryManagerClientRequestExit(): 0x%x\n", rc);
            }

            deliveryManagerClose(&manager);
        }
        if (R_FAILED(rc)) ret = 1;
    }

    if (log_file && log_filepath) fclose(log_file);

#ifdef __WIN32__
    WSACleanup ();
#endif
    return ret;
}

