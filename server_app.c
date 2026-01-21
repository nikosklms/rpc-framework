#include "server.h"

// ==================== Worker Threads ====================
void* worker_thread(void* arg) {
    log_printf("Worker thread %lu ξεκίνησε.\n", pthread_self());
    
    while (1) {
        char buf[MAX_PAYLOAD] = {0};  // Reset buffer
        int len = MAX_PAYLOAD;        // Reset length
        char reply[MAX_PAYLOAD] = {0};// Reset reply
        
        int reqid = getRequest(1, buf, &len);
        if(len == 0) { // Null request
            sendReply(reqid, NULL, 0);
            printf("[APP] Sent NULL reply for NULL request %d\n", reqid);
            continue;
        }

        if (reqid < 0) {
            log_printf("[APP] Προειδοποίηση: Λήψη μη έγκυρου αιτήματος.\n");
            continue;
        }

        log_printf("[APP] Επεξεργασία αιτήματος %d...\n", reqid);
        //sleep(7); // test heartbeat mode
        long number = atoi(buf);
        int prime = (number >= 2);
        for (long i = 2; i*i <= number; i++) {
            if (number % i == 0) prime = 0;
        }

        snprintf(reply, sizeof(reply), "%ld is %sprime", number, prime ? "" : "not ");
        log_printf("[APP] Αποτέλεσμα για αίτημα %d: %s\n", reqid, reply);
      
        sleep(10);
        sendReply(reqid, reply, strlen(reply)+1);
        log_printf("[APP] Απεστάλη απάντηση για αίτημα %d\n", reqid);
    }
    return NULL;
}

// ==================== main programa ====================
int main(int argc, char *argv[]) {
    if(argc != 1) {
        printf("Wrong num of args\n");
        return -1;
    }
    log_printf("=== Server Starting ===\n");
    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_create(&workers[i], NULL, worker_thread, NULL);
    }
    init();
    register_service(1);
    log_printf("=== Server Ready ===\n");
    
    while(1) pause();
    
    unregister_service(1);
    log_printf("=== Server Terminated ===\n");
    return 0;
}
