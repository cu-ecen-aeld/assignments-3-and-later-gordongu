#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>
#include <syslog.h>
#include <pthread.h>

// Constants
#define PORT "9000"
#define BUFF_SIZE 1024
const char *filepath = "/var/tmp/aesdsocketdata";

// Timestamp handler thread ID
pthread_t timestamp_thread_id;

// Linked list declarations
typedef struct thread_data{
    pthread_t thread_id;
    int socket_fd;
    int completed;
    LIST_ENTRY(thread_data) entries;
} thread_data_t;

LIST_HEAD(thread_data_list, thread_data) list_head;

// Mutex declarations
pthread_mutex_t file_access = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t thread_access = PTHREAD_MUTEX_INITIALIZER;

volatile sig_atomic_t run = 1;

struct addrinfo *servinfo;
int socket_fd;

// Close sockets, free dynamically allocated memory, and delete file on interruption or termination signals
void signal_handler(int signo) {
    syslog(LOG_INFO, "Caught signal, exiting");

    if (signo == SIGINT || signo == SIGTERM) {
        pthread_cancel(timestamp_thread_id);
        pthread_join(timestamp_thread_id, NULL);

        pthread_mutex_lock(&thread_access);
        thread_data_t *thread = LIST_FIRST(&list_head);

        while (thread != NULL) {
            thread_data_t *next = LIST_NEXT(thread, entries);
            pthread_cancel(thread->thread_id);
            pthread_mutex_unlock(&thread_access);
            pthread_join(thread->thread_id, NULL);
            LIST_REMOVE(thread, entries);
            pthread_mutex_lock(&thread_access);
            free(thread);
            thread = next;
        }
        pthread_mutex_unlock(&thread_access);

        pthread_mutex_destroy(&file_access);
        pthread_mutex_destroy(&thread_access);

        freeaddrinfo(servinfo);

        if (close(socket_fd) != 0) {
            syslog(LOG_ERR, "Error: unable to close socket connection");
        }

        if (remove(filepath) != 0) {
            syslog(LOG_ERR, "Error: unable to delete temporary file");
        }

        closelog();
    }

    run = 0;
}

void *timestamp_handler(void *arg) {
    while (run) {
        // Wait 10 seconds
        sleep(10);

        // Exit loop if termination occurred during sleep
        if (run != 1) break;

        char timestamp[BUFF_SIZE];
        time_t current_time = time(NULL);
        struct tm *tm_struct = localtime(&current_time);

        // Create timestamp string in RFC 2822 compliant strftime format
        strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_struct);

        // Open and write timestamp to file
        pthread_mutex_lock(&file_access);
        FILE *fileptr = fopen(filepath, "a+");

        if (fprintf(fileptr, "%s", timestamp) < 0) {
            syslog(LOG_ERR, "Error: unable to write to file");
            closelog();
            exit(EXIT_FAILURE);
        }

        fclose(fileptr);
        pthread_mutex_unlock(&file_access);
    }
}

void *connection_handler(void *arg) {
    // Get and cast thread params from arg
    thread_data_t *thread = (thread_data_t *)arg;
    int socket_fd = thread->socket_fd;

    // Create and dynamically allocate memory to buffer
    char *buffer = malloc(BUFF_SIZE);

    ssize_t received;
    int size = 0;

    // Begin receiving data from incoming connection
    while ((received = recv(socket_fd, buffer, BUFF_SIZE - 1, 0)) > 0) {
        // Null terminate the end of the buffer
        buffer[received] = '\0';

        // Dynamically allocate more memory to buffer if received data exceeds default buffer size
        if (size + received >= BUFF_SIZE) {
            ssize_t new_buff_size = size + received + 1;

            char *new_buffer = realloc(buffer, new_buff_size);

            if (new_buffer == NULL) {
                syslog(LOG_ERR, "Error: unable to reallocate memory for receiving buffer");
                closelog();
                exit(EXIT_FAILURE);
            }

            buffer = new_buffer;
        }

        // Update size and null terminate the end of the buffer
        size += received;
        buffer[size] = '\0';

        // Check for newline in buffer
        char *newline = memchr(buffer, '\n', size);
        
        // If newline is found, open file and begin writing to file from buffer
        if (newline != NULL) {
            pthread_mutex_lock(&file_access);
            FILE *fileptr = fopen(filepath, "a+");

            // Write to file from buffer
            if (fprintf(fileptr, "%s", buffer) < 0) {
                syslog(LOG_ERR, "Error: unable to write to file");
                closelog();
                exit(EXIT_FAILURE);
            }

            // Find file size after write
            fseek(fileptr, 0, SEEK_END);
            int filesize = ftell(fileptr);
            fseek(fileptr, 0, SEEK_SET);

            ssize_t read;
            
            // Begin sending data to incoming connection in BUFF_SIZE-sized chunks
            while ((read = fread(buffer, sizeof(char), BUFF_SIZE - 1, fileptr)) > 0) {
                // Null terminate the end of the buffer
                buffer[read] = '\0';

                ssize_t sent;

                if ((sent = send(socket_fd, buffer, read, 0)) < 1) {
                    syslog(LOG_ERR, "Error: unable to send data to incoming connection");
                    closelog();
                    exit(EXIT_FAILURE);
                }
            }
            
            fclose(fileptr);
            pthread_mutex_unlock(&file_access);

            // Reset for potential new incoming data
            size = 0;
        }
    }

    // Cleanup for thread and connection handler
    free(buffer);

    if (close(thread->socket_fd) != 0) {
        syslog(LOG_ERR, "Error: unable to close thread socket connection");
    }

    // Mark thread as completed in params
    thread->completed = 1;
    
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    // Open syslog to begin logging
    openlog("aesdsocket", LOG_CONS | LOG_PID, LOG_USER);

    // Bind interrupt and terminate signals to interrupt function
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        syslog(LOG_ERR, "Error: unable to bind signal to signal handler");
    }

    if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        syslog(LOG_ERR, "Error: unable to bind signal to signal handler");
    }

    struct addrinfo hints;

    // Set hints properties
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int new_socket_fd;

    // Check if address info can be retreived
    if (getaddrinfo(NULL, PORT, &hints, &servinfo) != 0) {
        syslog(LOG_ERR, "Error: unable to get local address info");
        closelog();
        exit(EXIT_FAILURE);
    }

    // Get local address and prepare incoming connecting address
    struct sockaddr local_address = *servinfo->ai_addr;
    struct sockaddr connecting_address;
    socklen_t connecting_address_len = sizeof connecting_address;

    // Check if new socket was created successfully
    if ((socket_fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        syslog(LOG_ERR, "Error: unable to create socket for listening for incoming connections");
        closelog();
        exit(EXIT_FAILURE);
    }

    // Check if binding socket and local address was successful
    if (bind(socket_fd, &local_address, sizeof local_address) < 0) {
        syslog(LOG_ERR, "Error: unable to bind socket and local address");
        closelog();
        exit(EXIT_FAILURE);
    }

    // Run process as daemon if -d argument is used
    int option;

    while ((option = getopt(argc, argv, "d")) != -1) {
        if (option == 'd') {
            if (daemon(0, 0) != 0) {
                syslog(LOG_ERR, "Error: unable to create process as daemon");
            }
        }
    }

    // Check if socket can successfully listen to incoming connections
    if (listen(socket_fd, 1) < 0) {
        syslog(LOG_ERR, "Error: socket unable to listen for incoming connections");
        closelog();
        exit(EXIT_FAILURE);
    }

    // Initialize linked list
    LIST_INIT(&list_head);

    // Create timestamp handler thread
    if (pthread_create(&timestamp_thread_id, NULL, timestamp_handler, NULL) != 0) {
        syslog(LOG_ERR, "Error: unable to create thread for handling timestamps");
        closelog();
        exit(EXIT_FAILURE);
    }

    // Main process loop
    while (run) {
        // Check if incoming connection can be accepted and if new socket was created successfully
        if ((new_socket_fd = accept(socket_fd, &connecting_address, &connecting_address_len)) < 0) {
            syslog(LOG_ERR, "Error: unable to accept incoming connection");
            closelog();
            exit(EXIT_FAILURE);
        }

        // Get connecting address' IP address as string
        char *connecting_address_str = inet_ntoa(((struct sockaddr_in *)&connecting_address)->sin_addr);

        syslog(LOG_INFO, "Accepted connection from %s", connecting_address_str);

        // Set up and dynamically allocate memory for thread params
        thread_data_t *thread = (thread_data_t *)malloc(sizeof(thread_data_t));

        if (thread == NULL) {
            syslog(LOG_ERR, "Error: unable to allocate memory for thread handling connection");
            closelog();
            exit(EXIT_FAILURE);
        }

        thread->socket_fd = new_socket_fd;
        thread->completed = 0;

        // Insert new thread node at head of linked list
        pthread_mutex_lock(&thread_access);
        LIST_INSERT_HEAD(&list_head, thread, entries);
        pthread_mutex_unlock(&thread_access);

        // Create connection handler thread
        if (pthread_create(&thread->thread_id, NULL, connection_handler, (void *)thread) != 0) {
            syslog(LOG_ERR, "Error: unable to create thread for handling connection");
            closelog();
            exit(EXIT_FAILURE);
        }

        // Join completed threads and remove them from the linked list
        pthread_mutex_lock(&thread_access);
        thread_data_t *iter = LIST_FIRST(&list_head);

        while (iter != NULL) {
            thread_data_t *next = LIST_NEXT(iter, entries);

            if (iter->completed == 1) {
                pthread_mutex_unlock(&thread_access);
                pthread_join(iter->thread_id, NULL);
                LIST_REMOVE(iter, entries);
                pthread_mutex_lock(&thread_access);
                free(iter);
            }

            iter = next;
        }
        pthread_mutex_unlock(&thread_access);

        // syslog(LOG_INFO, "Closed connection from %s", connecting_address_str);
    }

    // Cleanup
    pthread_cancel(timestamp_thread_id);
    pthread_join(timestamp_thread_id, NULL);

    pthread_mutex_lock(&thread_access);
    thread_data_t *thread = LIST_FIRST(&list_head);

    while (thread != NULL) {
        thread_data_t *next = LIST_NEXT(thread, entries);
        pthread_cancel(thread->thread_id);
        pthread_mutex_unlock(&thread_access);
        pthread_join(thread->thread_id, NULL);
        LIST_REMOVE(thread, entries);
        pthread_mutex_lock(&thread_access);
        free(thread);
        thread = next;
    }
    pthread_mutex_unlock(&thread_access);

    pthread_mutex_destroy(&file_access);
    pthread_mutex_destroy(&thread_access);

    freeaddrinfo(servinfo);

    if (close(socket_fd) != 0) {
        syslog(LOG_ERR, "Error: unable to close socket connection");
    }

    if (remove(filepath) != 0) {
        syslog(LOG_ERR, "Error: unable to delete temporary file");
    }

    closelog();
    exit(EXIT_SUCCESS);
}
