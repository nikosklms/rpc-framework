#include "server.h"

// Global Variables

static int sockfd;

// Request queue and its synchronization.
static request_t request_queue[QUEUE_SIZE];
static int queue_head = 0, queue_tail = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

// Client map (to match reqid with client address info) and its mutex.
static client_map_t client_map[MAX_CLIENTS];
static int client_map_size = 0;
static pthread_mutex_t client_map_mutex = PTHREAD_MUTEX_INITIALIZER;

// Response queue and its synchronization.
static response_t response_queue[QUEUE_SIZE];
static int res_head = 0, res_tail = 0;
static pthread_mutex_t res_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t res_cond = PTHREAD_COND_INITIALIZER;

processed_req_t processed_reqs[MAX_REQUESTS];
int processed_count = 0, processed_index = 0;
pthread_mutex_t processed_mutex = PTHREAD_MUTEX_INITIALIZER;

// Open requests queue for heartbeats
static int open_head = 0, open_tail = 0;
static request_t open_requests[QUEUE_SIZE];
static pthread_mutex_t open_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t open_cond = PTHREAD_COND_INITIALIZER;

int services[MAX_SERVICES] = {0};

// log_printf: prints a timestamp and a message; note that the timestamp is in the local time zone.
void log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    // get current time in local timezone (set timezone beforehand if needed)
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    // format time as "[HH:MM:SS]"
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "[%H:%M:%S]", &tm_info);

    // print timestamp + message
    printf("%s ", timestamp);
    vprintf(format, args);

    va_end(args);
}

// load_processed_reqs: loads the persistent file contents into processed_reqs.
// if the file doesn't exist, it creates a new one.
void load_processed_reqs() {
    FILE *fp = fopen(PERSIST_FILE, "rb");
    if (!fp) {
        log_printf("error: could not find file %s, creating new one...\n", PERSIST_FILE);
        // create persist file
        fp = fopen(PERSIST_FILE, "wb");
        if (!fp) {
            log_printf("error: could not create file %s\n", PERSIST_FILE);
            return;
        }
        fclose(fp);
        // reopen in read mode
        fp = fopen(PERSIST_FILE, "rb");
        if (!fp) {
            log_printf("error: could not open file %s after creation\n", PERSIST_FILE);
            return;
        }
    }

    pthread_mutex_lock(&processed_mutex);

    flock(fileno(fp), LOCK_EX);
    log_printf("loading request history...\n");
    
    // load all records
    while (fread(&processed_reqs[processed_count].reqid, sizeof(int), 1, fp)) {
        // even though we don't use timestamp for FIFO, we set it here anyway
        processed_count++;
        if (processed_count >= MAX_REQUESTS) break;
    }
    log_printf("loaded %d records\n", processed_count);
    flock(fileno(fp), LOCK_UN);
    fclose(fp);
    fp = fopen(PERSIST_FILE, "wb");
    // Write only the processed records back to the file (up to processed_count)
    for (int i = 0; i < processed_count; i++) {
        fwrite(&processed_reqs[i].reqid, sizeof(int), 1, fp);
    }
    fclose(fp);
    pthread_mutex_unlock(&processed_mutex);
}

// print_db_contents: prints the contents of the persistent file.
void print_db_contents() {
    FILE *fp = fopen(PERSIST_FILE, "rb");
    if (!fp) {
        log_printf("no file %s exists\n", PERSIST_FILE);
        return;
    }
    
    log_printf("contents of %s: ", PERSIST_FILE);
    int reqid;
    while (fread(&reqid, sizeof(int), 1, fp)) {
        printf("%d ", reqid);
    }
    printf("\n");
    fclose(fp);
}

// is_duplicate: checks if the given reqid has already been processed.
int is_duplicate(int reqid) {
    pthread_mutex_lock(&processed_mutex);
    for (int i = 0; i < processed_count; i++) {
        if (processed_reqs[i].reqid == reqid) {
            pthread_mutex_unlock(&processed_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&processed_mutex);
    return 0;
}

// enqueue_request: enqueues a received request into the request_queue.
int enqueue_request(request_t req) {
    pthread_mutex_lock(&queue_mutex);
    int next_tail = (queue_tail + 1) % QUEUE_SIZE;
    if (next_tail == queue_head) {
        log_printf("request queue full, dropping reqid %d\n", req.reqid);
        pthread_mutex_unlock(&queue_mutex);
        return -1;
    } else {
        request_queue[queue_tail] = req;
        queue_tail = next_tail;
        pthread_cond_signal(&queue_cond);
    }
    pthread_mutex_unlock(&queue_mutex);
    return 0;
}

// enqueue_response: enqueues a response into the response_queue.
int enqueue_response(response_t res) {
    pthread_mutex_lock(&res_mutex);
    int next_tail = (res_tail + 1) % QUEUE_SIZE;
    if (next_tail == res_head) {
        log_printf("response queue full, reqid %d\n", res.reqid);
        pthread_mutex_unlock(&res_mutex);
        return -1;
    } else {
        response_queue[res_tail] = res;
        res_tail = next_tail;
        pthread_cond_signal(&res_cond);
    }
    pthread_mutex_unlock(&res_mutex);
    return 0;
}

// dequeue_response: blocks until a response is available, then returns it.
response_t dequeue_response() {
    pthread_mutex_lock(&res_mutex);
    while (res_head == res_tail) {
        pthread_cond_wait(&res_cond, &res_mutex);
    }
    response_t res = response_queue[res_head];
    res_head = (res_head + 1) % QUEUE_SIZE;
    pthread_mutex_unlock(&res_mutex);
    return res;
}

// is_service_registered: returns 1 if the given service id is registered, 0 otherwise.
int is_service_registered(int svcid) {
    if (services[svcid] == 1) {
        return 1; // service registered
    }
    
    return 0; // service not registered
}

// update_processed_reqs: updates the processed_reqs array with the new req.
// if the array is full, it evicts the oldest record (at index 0) by shifting left.
void update_processed_reqs(request_t req) {
    // record the new reqid in memory and in the persistent file.
    pthread_mutex_lock(&processed_mutex);
    
    if (processed_count >= MAX_REQUESTS) {
        // array is full, so replace the oldest record (at index 0) with the new one
        // shift all entries left to maintain insertion order
        for (int i = 0; i < processed_count - 1; i++) {
            processed_reqs[i] = processed_reqs[i + 1];
        }
        // insert new req at the end
        processed_reqs[processed_count - 1].reqid = req.reqid;
    } else {
        processed_reqs[processed_count].reqid = req.reqid;
        processed_count++;
    }
    

    // re-write persistent file with current reqids
    FILE *fp = fopen(PERSIST_FILE, "wb");
    if (fp) {
        int *reqids = malloc(processed_count * sizeof(int));
        for (int i = 0; i < processed_count; i++) {
            reqids[i] = processed_reqs[i].reqid;
        }
        flock(fileno(fp), LOCK_EX); // lock file
        fwrite(reqids, sizeof(int), processed_count, fp); // write only reqids
        free(reqids);
        flock(fileno(fp), LOCK_UN); // unlock file
        fclose(fp);
    }
    pthread_mutex_unlock(&processed_mutex);

    print_db_contents();
}

// handle_unregistered_service: handles requests for services that are not registered.
void handle_unregistered_service(request_t req) {
    char msg[27] = "Service ";
    // append the service id to the message
    snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "%d", req.svcid);
    strcat(msg, " is not available");

    response_t cancel_response;
    cancel_response.reqid = req.reqid;
    cancel_response.client_addr = req.client_addr;
    memcpy(cancel_response.response, msg, strlen(msg) + 1);
    cancel_response.response_len = strlen(msg) + 1;
    cancel_response.addr_len = req.addr_len;

    pthread_mutex_lock(&open_mutex);
    int next_open_tail = (open_tail + 1) % QUEUE_SIZE;
    if (next_open_tail != open_head) {
        open_requests[open_tail] = req;
        open_tail = next_open_tail;
        pthread_cond_signal(&open_cond); // wake up heartbeat thread
    }
    pthread_mutex_unlock(&open_mutex);

    // enqueue the response.
    while(1) {
        if(enqueue_response(cancel_response) == -1) {
            continue; // if response queue is full, retry until the reply gets q'ed
        }
        else break;
    }
    log_printf("unregistered service handled for reqid %d\n", req.reqid);
}

// handle_cancel_request: handles cancel requests (svcid == -1) by removing the request from queues.
void handle_cancel_request(request_t req) {
    int canceled = 0;
    int i;

    // deletion from request_queue (if exists)
    pthread_mutex_lock(&queue_mutex);
    i = queue_head;
    while (i != queue_tail) {
        if (request_queue[i].reqid == req.reqid) {
            // shift subsequent elements left
            for (int j = i; j != queue_tail; j = (j + 1) % QUEUE_SIZE) {
                request_queue[j] = request_queue[(j + 1) % QUEUE_SIZE];
            }
            queue_tail = (queue_tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
            canceled = 1;
            log_printf("reqid %d removed from queue\n", req.reqid);
            break;
        }
        i = (i + 1) % QUEUE_SIZE;
    }
    pthread_mutex_unlock(&queue_mutex);

    // deletion from response_queue (if exists)
    pthread_mutex_lock(&res_mutex);
    i = res_head;
    while (i != res_tail) {
        if (response_queue[i].reqid == req.reqid) {
            // shift subsequent elements left
            for (int j = i; j != res_tail; j = (j + 1) % QUEUE_SIZE) {
                response_queue[j] = response_queue[(j + 1) % QUEUE_SIZE];
            }
            res_tail = (res_tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
            canceled = 1;
            log_printf("response for reqid %d removed from queue\n", req.reqid);
            break;
        }
        i = (i + 1) % QUEUE_SIZE;
    }
    pthread_mutex_unlock(&res_mutex);

    // deletion for client_map
    pthread_mutex_lock(&client_map_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(client_map[i].reqid == req.reqid) {
            for(int j = i; j < MAX_CLIENTS-1; j++) {
                client_map[j] = client_map[j+1];
            }
            canceled = 1;
            break;
        }
    }
    pthread_mutex_unlock(&client_map_mutex);


    // send ack if found in any queue
    response_t ack_response;
    ack_response.reqid = req.reqid;
    ack_response.client_addr = req.client_addr;
    ack_response.addr_len = req.addr_len;
    if (canceled) {
        log_printf("canceled request %d\n", req.reqid);
        char msg[] = "ack: request canceled.";
        memcpy(ack_response.response, msg, strlen(msg) + 1);
        ack_response.response_len = strlen(msg) + 1;
    } else {
        log_printf("reqid %d not found for cancellation\n", req.reqid);
        char msg[] = "request was not found to be canceled.";
        memcpy(ack_response.response, msg, strlen(msg) + 1);
        ack_response.response_len = strlen(msg) + 1;
    }

    // enqueue the response.
    while(1) {
        if(enqueue_response(ack_response) == -1) {
            continue; // if response queue is full, retry until the reply gets q'ed
        }
        else break;
    }
}

// udp_listener_thread: receives UDP packets and processes them.
void *udp_listener_thread(void *arg) {
    (void)arg; // unused
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[MAX_PAYLOAD + 50]; // extra space for header

    while (1) {
        ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                             (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }
        // expected packet format:
        // [reqid(int)][svcid(int)][payload_len(int)][payload bytes...]
        if (n < 3 * sizeof(int)) {
            log_printf("malformed packet received\n");
            continue;
        }
        request_t req;
        memcpy(&req.reqid, buffer, sizeof(int));
        memcpy(&req.svcid, buffer + sizeof(int), sizeof(int));
        memcpy(&req.payload_len, buffer + 2 * sizeof(int), sizeof(int));
        // convert from network byte order.
        req.reqid = ntohl(req.reqid);
        req.svcid = ntohl(req.svcid);
        req.payload_len = ntohl(req.payload_len);
        if (req.payload_len > MAX_PAYLOAD)
            req.payload_len = MAX_PAYLOAD;
        memcpy(req.payload, buffer + 3 * sizeof(int), req.payload_len);
        req.payload[req.payload_len] = '\0';
        req.client_addr = client_addr;
        req.addr_len = addr_len;

        log_printf("[udp listener] received reqid: %d, svcid: %d, reqlen: %d, reqbuf: %s\n",
                   req.reqid, req.svcid, req.payload_len, req.payload);
        
        // check if it's an ack for a heartbeat
        if(req.reqid < 0) {
            pthread_mutex_lock(&open_mutex);
            int current = open_head;
            while(current != open_tail) {
                request_t tmp = open_requests[current];
                if(tmp.reqid == ((-1)*(req.reqid))) {
                    // acknowledge the ack 
                    log_printf("[udp listener] ack was received for heartbeat for reqid %d\n", req.reqid);
                    open_requests[current].heartbeat_ack--;
                    break;
                }
                current = (current + 1) % QUEUE_SIZE;
            }
            pthread_mutex_unlock(&open_mutex);    
            continue;     
        }

        // check for duplicate requests (&& not cancel requests)
        if (is_duplicate(req.reqid) && req.svcid != -1) {
            log_printf("[udp listener] duplicate reqid %d received; ignoring\n", req.reqid);
            continue;
        }
        
        // update processed reqs (this function encapsulates the insertion and fifo replacement logic)
        if(req.svcid != -1) {
            update_processed_reqs(req);
        }

        // check for service registration
        if (req.svcid != -1 && !is_service_registered(req.svcid)) {
            handle_unregistered_service(req);
            continue;
        }
        
        // handle cancel request
        if (req.svcid == -1) {
            log_printf("[udp listener] cancel request received for reqid %d\n", req.reqid);
            handle_cancel_request(req);
            continue;
        }

        int res = enqueue_request(req);
        if(res == -1) {
            // server is busy
            log_printf("[udp listener] server is busy, req %d was dropped\n", req.reqid);
            continue;
        }

        pthread_mutex_lock(&open_mutex);
        int next_open_tail = (open_tail + 1) % QUEUE_SIZE;
        if (next_open_tail != open_head) {
            open_requests[open_tail] = req;
            open_requests[open_tail].heartbeat_ack = 0; // init 
            open_tail = next_open_tail;
            pthread_cond_signal(&open_cond); // wake up heartbeat thread
        }
        pthread_mutex_unlock(&open_mutex);
    }
    return NULL;
}

// udp_sender_thread: sends responses from the response_queue.
void *udp_sender_thread(void *arg) {
    (void)arg; // unused
    while (1) {
        response_t res = dequeue_response();

        // create response packet (reqid + response_len + response)
        int packet_size = sizeof(int) * 2 + res.response_len;
        char packet[packet_size];

        // convert to network byte order before copying
        int reqid_network = htonl(res.reqid);          // convert reqid
        int response_len_network = htonl(res.response_len); // convert response_len

        // copy data in proper format (network byte order)
        memcpy(packet, &reqid_network, sizeof(int));
        memcpy(packet + sizeof(int), &response_len_network, sizeof(int));
        memcpy(packet + 2 * sizeof(int), res.response, res.response_len);

        // in udp_sender_thread, before sending, remove from open_requests
        pthread_mutex_lock(&open_mutex);
        for (int i = open_head; i != open_tail; i = (i + 1) % QUEUE_SIZE) {
            if (open_requests[i].reqid == res.reqid) {
                // remove from open_requests by shifting left
                for (int j = i; j != open_tail; j = (j + 1) % QUEUE_SIZE) {
                    open_requests[j] = open_requests[(j + 1) % QUEUE_SIZE];
                }
                open_tail = (open_tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
                break;
            }
        }
        pthread_mutex_unlock(&open_mutex);

        // send packet
        ssize_t sent = sendto(sockfd, packet, packet_size, 0,
                              (struct sockaddr *)&res.client_addr, res.addr_len);
        if (sent < 0) {
            perror("sendto failed");
        } else {
            log_printf("[udp sender] sent reply for reqid %d (size %d): %.*s\n",
                       res.reqid, res.response_len, res.response_len, res.response);
            log_printf("===================================\n");
        }
    }
    return NULL;
}

// heartbeat_thread: periodically sends heartbeat messages for open requests.
void* heartbeat_thread(void* arg) {
    (void)arg;
    
    while(1) {
        pthread_mutex_lock(&open_mutex);

        // send heartbeats for all open requests
        int current = open_head;
        while(current != open_tail) {
            request_t req = open_requests[current];
            if(req.heartbeat_ack != 0) {
                log_printf("ack was not received for heartbeat for reqid %d\n", req.reqid);
                // means that the last heartbeat did not receive ack in time
                // so we delete the request and response related to that reqid
                int i;
                // 1. deletion from request_queue (if exists)
                pthread_mutex_lock(&queue_mutex);
                i = queue_head;
                while (i != queue_tail) {
                    if (request_queue[i].reqid == req.reqid) {
                        // shift subsequent elements left
                        for (int j = i; j != queue_tail; j = (j + 1) % QUEUE_SIZE) {
                            request_queue[j] = request_queue[(j + 1) % QUEUE_SIZE];
                        }
                        queue_tail = (queue_tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
                        log_printf("reqid %d removed from queue\n", req.reqid);
                        break;
                    }
                    i = (i + 1) % QUEUE_SIZE;
                }
                pthread_mutex_unlock(&queue_mutex);

                // 2. deletion from response_queue (if exists)
                pthread_mutex_lock(&res_mutex);
                i = res_head;
                while (i != res_tail) {
                    if (response_queue[i].reqid == req.reqid) {
                        // shift subsequent elements left
                        for (int j = i; j != res_tail; j = (j + 1) % QUEUE_SIZE) {
                            response_queue[j] = response_queue[(j + 1) % QUEUE_SIZE];
                        }
                        res_tail = (res_tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
                        log_printf("response for reqid %d removed from queue\n", req.reqid);
                        break;
                    }
                    i = (i + 1) % QUEUE_SIZE;
                }
                pthread_mutex_unlock(&res_mutex);   

                // 3. deletion from open_requests
                // *** it's already lock-safe (check below the while(1) loop)
                for (int i = open_head; i != open_tail; i = (i + 1) % QUEUE_SIZE) {
                    if (open_requests[i].reqid == req.reqid) {
                        // remove from open_requests by shifting left
                        for (int j = i; j != open_tail; j = (j + 1) % QUEUE_SIZE) {
                            open_requests[j] = open_requests[(j + 1) % QUEUE_SIZE];
                        }
                        open_tail = (open_tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
                        break;
                    }
                }

                // 4. deletion for client_map
                pthread_mutex_lock(&client_map_mutex);
                for(int i = 0; i < MAX_CLIENTS; i++) {
                    if(client_map[i].reqid == req.reqid) {
                        for(int j = i; j < MAX_CLIENTS-1; j++) {
                            client_map[j] = client_map[j+1];
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&client_map_mutex);

                continue;
            }
            // create heartbeat packet (negative reqid)
            int hb_reqid = -req.reqid;
            int hb_rsplen = 0;
            int reqid_net = htonl(hb_reqid);
            int rsplen_net = htonl(hb_rsplen);
            char hb_packet[sizeof(int) * 2];
            
            memcpy(hb_packet, &reqid_net, sizeof(int));
            memcpy(hb_packet + sizeof(int), &rsplen_net, sizeof(int));
            
            open_requests[current].heartbeat_ack++;
            sendto(sockfd, hb_packet, sizeof(hb_packet), 0,
                  (struct sockaddr*)&req.client_addr, req.addr_len);
            log_printf("sent heartbeat to client with reqid: %d\n", req.reqid);
            current = (current + 1) % QUEUE_SIZE;
        }
        
        pthread_mutex_unlock(&open_mutex);
        sleep(2); // sleep for 2 seconds
    }
    return NULL;
}

// RESET PROCESSED_REQS AND .db FILE EVERY timeout SECONDS 
void *reset_reqid_log(void *arg) {
    const int timeout = 50000;
    log_printf("Automatic reset was set for every %d seconds\n", timeout);

    while (1) {
        sleep(timeout);
        pthread_mutex_lock(&processed_mutex);

        log_printf("######## RESET START ########\n");
        log_printf("Processed_reqs before reset: ");
        for (int i = 0; i < processed_count; i++) {
            printf("%d ", processed_reqs[i].reqid);
        }
        printf("\n");
        processed_count = 0;

        log_printf("Processed_reqs after reset: ");
        for (int i = 0; i < processed_count; i++) {
            printf("%d ", processed_reqs[i].reqid);
        }
        printf("\n");
        log_printf("######## RESET END ########\n");

        FILE *fp = fopen(PERSIST_FILE, "wb");
        if (fp) {
            

            int *reqids = malloc(processed_count * sizeof(int));
            for (int i = 0; i < processed_count; i++) {
                reqids[i] = processed_reqs[i].reqid;
            }
            flock(fileno(fp), LOCK_EX); 

            fwrite(reqids, sizeof(int), processed_count, fp);
            free(reqids);
            flock(fileno(fp), LOCK_UN); 
            fclose(fp);
        }
        pthread_mutex_unlock(&processed_mutex);
    }
    return NULL;
}

// ----------------------------------------------------------
// API Functions

void init() {
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        log_printf("socket creation failed\n");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = 0;  // Let OS pick a free port

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_printf("bind failed\n");
        exit(EXIT_FAILURE);
    }

    // Retrieve the assigned port
    socklen_t len = sizeof(server_addr);
    if (getsockname(sockfd, (struct sockaddr *)&server_addr, &len) == 0) {
    } else {
        log_printf("getsockname() failed\n");
    }

    // retrieve actual IP address (non-loopback)
    char server_ip[INET_ADDRSTRLEN] = "unknown";
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr && (ifa->ifa_addr->sa_family == AF_INET)) {
                if (!(ifa->ifa_flags & IFF_LOOPBACK)) {
                    struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
                    inet_ntop(AF_INET, &addr->sin_addr, server_ip, sizeof(server_ip));
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
    }
    log_printf("Server contact: %s:%d\n", server_ip, ntohs(server_addr.sin_port));

    pthread_t listener_tid, sender_tid, heartbeat_tid, rmv_tid;
    if (pthread_create(&listener_tid, NULL, udp_listener_thread, NULL) != 0) {
        log_printf("pthread_create (listener) failed\n");
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&sender_tid, NULL, udp_sender_thread, NULL) != 0) {
        log_printf("pthread_create (sender) failed\n");
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&heartbeat_tid, NULL, heartbeat_thread, NULL) != 0) {
        log_printf("pthread_create (heartbeat_thread) failed\n");
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&rmv_tid, NULL, reset_reqid_log, NULL) != 0) {
        log_printf("pthread_create (reset_reqid_log) failed\n");
        exit(EXIT_FAILURE);
    }
    load_processed_reqs();
    pthread_detach(listener_tid);
    pthread_detach(sender_tid);
    pthread_detach(heartbeat_tid);
    pthread_detach(rmv_tid);
}


// register_service: registers a service with the given svcid.
int register_service(int svcid) {
    services[svcid] = 1;
    log_printf("service %d registered.\n", svcid);
    return 0;
}

// unregister_service: unregisters a service with the given svcid.
int unregister_service(int svcid) {
    services[svcid] = 0;
    log_printf("service %d unregistered.\n", svcid);
    return 0;
}

// getRequest: blocks until a request for the given svcid is available.
// copies the request payload into buf (up to *len bytes), updates *len, and returns the reqid.
int getRequest (int svcid, void *buf, int *len) {
    request_t req;
    int found = 0;

    pthread_mutex_lock(&queue_mutex);
    while (!found) {
        // wait until queue is nonempty.
        while (queue_head == queue_tail) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        // search the request queue for a matching service id.
        int n = (queue_tail >= queue_head) ? (queue_tail - queue_head)
                                           : (QUEUE_SIZE - queue_head + queue_tail);
        for (int i = 0; i < n; i++) {
            int idx = (queue_head + i) % QUEUE_SIZE;
            if (request_queue[idx].svcid == svcid) {
                req = request_queue[idx];
                // remove the found request by shifting subsequent ones.
                for (int j = idx; j != queue_tail; j = (j + 1) % QUEUE_SIZE) {
                    int next = (j + 1) % QUEUE_SIZE;
                    request_queue[j] = request_queue[next];
                }
                queue_tail = (queue_tail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
                found = 1;
                break;
            }
        }
        if (!found) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
    }
    pthread_mutex_unlock(&queue_mutex);

    // copy payload into provided buffer.
    int copy_len = (req.payload_len < *len) ? req.payload_len : *len;
    memcpy(buf, req.payload, copy_len);
    *len = copy_len;

    // save client address for later reply.
    pthread_mutex_lock(&client_map_mutex);
    if (client_map_size < MAX_CLIENTS) {
        client_map[client_map_size].reqid = req.reqid;
        client_map[client_map_size].client_addr = req.client_addr;
        client_map[client_map_size].addr_len = req.addr_len;
        client_map_size++;
    } else {
        log_printf("client map full for reqid %d\n", req.reqid);
    }
    pthread_mutex_unlock(&client_map_mutex);

    log_printf("getRequest() returning reqid %d with payload: %s\n", req.reqid, req.payload);
    return req.reqid;
}

// sendReply: sends a reply for the given reqid by enqueuing the response for the UDP sender thread.
int sendReply (int reqid, void *buf, int len) {
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    int found = 0;

    // look up the client address associated with the reqid.
    pthread_mutex_lock(&client_map_mutex);
    for (int i = 0; i < client_map_size; i++) {
        if (client_map[i].reqid == reqid) {
            client_addr = client_map[i].client_addr;
            addr_len = client_map[i].addr_len;
            // remove mapping once used.
            client_map[i] = client_map[client_map_size - 1];
            client_map_size--;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&client_map_mutex);

    if (!found) {
        log_printf("no mapping found for reqid %d\n", reqid);
        return -1;
    }

    // build the response object.
    response_t res;
    res.reqid = reqid;
    if (len > MAX_PAYLOAD)
        len = MAX_PAYLOAD;
    memcpy(res.response, buf, len);
    res.response_len = len;
    res.client_addr = client_addr;
    res.addr_len = addr_len;

    // enqueue the response.
    while(1) {
        if(enqueue_response(res) == -1) {
            continue; // if response queue is full, retry until the reply gets q'ed
        }
        else break;
    }
    log_printf("sendReply() enqueued reply for reqid %d\n", reqid);
    return 0;
}