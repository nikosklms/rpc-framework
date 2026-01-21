#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <sys/file.h>
#include <stdarg.h>  
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>      // For IFF_LOOPBACK
#include <time.h>

#define SERVER_PORT 12345
#define QUEUE_SIZE 100
#define MAX_PAYLOAD 256
#define MAX_CLIENTS 100
#define PERSIST_FILE "reqids.db"
#define NUM_WORKERS 4
#define DEFAULT_PORT 12345
#define MAX_REQUESTS 5
#define HEARTBEAT_INTERVAL 5 // seconds
#define MAX_SERVICES 1000

// Represents an incoming request.
typedef struct {
    int reqid;
    int svcid;          
    int payload_len;
    char payload[MAX_PAYLOAD];
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    int heartbeat_ack;
} request_t;

// Represents a mapping from reqid to the client address.
typedef struct {
    int reqid;
    struct sockaddr_in client_addr;
    socklen_t addr_len;
} client_map_t;

// Represents a response to be sent.
typedef struct {
    int reqid;         
    char response[MAX_PAYLOAD];
    int response_len;
    struct sockaddr_in client_addr;
    socklen_t addr_len;
} response_t;

// Data structure for persisted processed request IDs.
typedef struct {
    int reqid;
} processed_req_t;

void init();
int register_service(int svcid);
int unregister_service(int svcid);
int getRequest(int svcid, void *buf, int *len);
int sendReply(int reqid, void *buf, int len);
void log_printf(const char *format, ...); // voithitiko gia prints

#endif
