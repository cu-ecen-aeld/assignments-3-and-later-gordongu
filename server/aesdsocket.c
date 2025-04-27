#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>
#include <syslog.h>
#include <pthread.h>
#include "../aesd-char-driver/aesd_ioctl.h"

// Constants
#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define FILE_PATH "/dev/aesdchar"
#else
#define FILE_PATH "/var/tmp/aesdsocketdata"
#endif

#define PORT "9000"
#define BUFF_SIZE 1024
#define IOCTL_CMD "AESDCHAR_IOCSEEKTO"
// const char *filepath = "/var/tmp/aesdsocketdata";

#if !USE_AESD_CHAR_DEVICE
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
#endif

volatile sig_atomic_t finished = 0;

int socket_fd;

// Exit main loop on interrupt or termination
void signal_handler(int signo) {
    syslog(LOG_INFO, "Caught signal %i, exiting", signo);

    if (signo == SIGINT || signo == SIGTERM) finished = 1;
}

#if !USE_AESD_CHAR_DEVICE
void *timestamp_handler(void *arg) {
    while (!finished) {
        // Wait 10 seconds
        sleep(10);

        // Exit loop if termination occurred during sleep
        if (finished) break;

        char timestamp[BUFF_SIZE];
        time_t current_time = time(NULL);
        struct tm *tm_struct = localtime(&current_time);

        // Create timestamp string in RFC 2822 compliant strftime format
        strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_struct);

        // Open and write timestamp to file
        pthread_mutex_lock(&file_access);
        FILE *fileptr = fopen(FILE_PATH, "a+");

        if (fprintf(fileptr, "%s", timestamp) < 0) {
            syslog(LOG_ERR, "Error: unable to write to file");
            closelog();
            exit(EXIT_FAILURE);
        }

        // Force write to file from buffer
        fflush(fileptr);
        fclose(fileptr);
        pthread_mutex_unlock(&file_access);
    }

    return NULL;
}

void *connection_handler(void *arg) {
    // Get and cast thread params from arg
    thread_data_t *thread = (thread_data_t *)arg;
    int socket_fd = thread->socket_fd;

    // Create and dynamically allocate memory to buffer
    char *buffer = malloc(BUFF_SIZE);

    ssize_t received;
    int size = 0;

    pthread_mutex_lock(&file_access);
    FILE *fileptr = fopen(FILE_PATH, "a+");

    // Begin receiving data from incoming connection
    while ((received = recv(socket_fd, buffer, BUFF_SIZE - 1, 0)) > 0) {
        // Null terminate the end of the buffer
        buffer[received] = '\0';

        // Write to file from buffer
        if (fprintf(fileptr, "%s", buffer) < 0) {
            syslog(LOG_ERR, "Error: unable to write to file");
            closelog();
            exit(EXIT_FAILURE);
        }

        // Force write to file from buffer
        fflush(fileptr);

        // Check for newline in buffer
        char *newline = memchr(buffer, '\n', received);
        
        // If newline is found, exit loop and open file to begin writing to from buffer
        if (newline != NULL) break;
    }

    // Find file size after write
    fseek(fileptr, 0, SEEK_END);
    int filesize = ftell(fileptr);
    fseek(fileptr, 0, SEEK_SET);

    ssize_t bytes_read;
    
    // Begin sending data to incoming connection in BUFF_SIZE-sized chunks
    while ((bytes_read = fread(buffer, sizeof(char), BUFF_SIZE - 1, fileptr)) > 0) {
        // Null terminate the end of the buffer
        buffer[bytes_read] = '\0';

        ssize_t bytes_sent;

        if ((bytes_sent = send(socket_fd, buffer, bytes_read, 0)) < 1) {
            syslog(LOG_ERR, "Error: unable to send data to incoming connection");
            closelog();
            exit(EXIT_FAILURE);
        }
    }
    
    fclose(fileptr);
    pthread_mutex_unlock(&file_access);

    // Cleanup for thread and connection handler
    free(buffer);

    if (close(thread->socket_fd) != 0) {
        syslog(LOG_ERR, "Error: unable to close thread socket connection");
    }

    // Mark thread as completed in params
    thread->completed = 1;
    
    return NULL;
}
#endif

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
    struct addrinfo *servinfo;

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

    int opt = 1;
    
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        syslog(LOG_ERR, "Error: unable to set SO_REUSEADDR opt for socket");
        closelog();
        exit(EXIT_FAILURE);
    }

    // Check if binding socket and local address was successful
    if (bind(socket_fd, &local_address, sizeof local_address) < 0) {
        syslog(LOG_ERR, "Error: unable to bind socket and local address");
        closelog();
        exit(EXIT_FAILURE);
    }

    // Free addrinfo after all related calls are finished
    freeaddrinfo(servinfo);

    // Run process as daemon if -d argument is used
    int option;

    while ((option = getopt(argc, argv, "d")) != -1) {
        if (option == 'd') {
            pid_t pid = fork();

            if (pid < 0) {
                syslog(LOG_ERR, "Error: unable to fork child process");
                closelog();
                exit(EXIT_FAILURE);
            } else if (pid > 0) {
                exit(EXIT_SUCCESS);
            }

            if (chdir("/") != 0) {
                syslog(LOG_ERR, "Error: unable to change directory to root");
                closelog();
                exit(EXIT_FAILURE);
            }

            setsid();
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }
    }

    #if !USE_AESD_CHAR_DEVICE
    // Create empty file or truncate existing file at start
    FILE *fileptr = fopen(FILE_PATH, "w");
    fclose(fileptr);
    #endif

    // Check if socket can successfully listen to incoming connections
    if (listen(socket_fd, 10) < 0) {
        syslog(LOG_ERR, "Error: socket unable to listen for incoming connections");
        closelog();
        exit(EXIT_FAILURE);
    }

    #if !USE_AESD_CHAR_DEVICE
    // Initialize linked list
    LIST_INIT(&list_head);

    // Create timestamp handler thread
    if (pthread_create(&timestamp_thread_id, NULL, timestamp_handler, NULL) != 0) {
        syslog(LOG_ERR, "Error: unable to create thread for handling timestamps");
        closelog();
        exit(EXIT_FAILURE);
    }

    // Detach because of incompatability with pthread_cancel and creating weird memory leaks
    pthread_detach(timestamp_thread_id);
    #endif

    // Main process loop
    while (!finished) {
        // Use select before accept to prevent blocking
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        struct timeval timeout = {1, 0};

        int status;

        if ((status = select(socket_fd + 1, &read_fds, NULL, NULL, &timeout)) < 0) break;

        // No connection found so continue to next loop iteration
        if (status == 0) continue;

        // Check if incoming connection can be accepted and if new socket was created successfully
        if ((new_socket_fd = accept(socket_fd, &connecting_address, &connecting_address_len)) < 0) {
            syslog(LOG_ERR, "Error: unable to accept incoming connection");
            closelog();
            exit(EXIT_FAILURE);
        }

        // Get connecting address' IP address as string
        char *connecting_address_str = inet_ntoa(((struct sockaddr_in *)&connecting_address)->sin_addr);

        syslog(LOG_INFO, "Accepted connection from %s", connecting_address_str);

        #if !USE_AESD_CHAR_DEVICE
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
        #else
        // Create and dynamically allocate memory to buffer
        char *buffer = malloc(BUFF_SIZE);
        bool ioctl_enabled = 0;
        FILE *fileptr = fopen(FILE_PATH, "a+");

        ssize_t received;

        // Begin receiving data from incoming connection
        while ((received = recv(new_socket_fd, buffer, BUFF_SIZE - 1, 0)) > 0) {
            // Null terminate the end of the buffer
            buffer[received] = '\0';

            // Write to file from buffer
            if (strncmp(buffer, IOCTL_CMD, strlen(IOCTL_CMD)) == 0) {
                unsigned int write_cmd, write_cmd_offset;
                ioctl_enabled = 1;

                if (sscanf(buffer, "AESDCHAR_IOCSEEKTO:%u,%u", &write_cmd, &write_cmd_offset) == 2) {
                    struct aesd_seekto seekto;
                    seekto.write_cmd = write_cmd;
                    seekto.write_cmd_offset = write_cmd_offset;
                    int fd = open(FILE_PATH, O_RDWR);

                    if (fd < 0) {
                        syslog(LOG_ERR, "Error: unable to open file for ioctl operation");
                        closelog();
                        exit(EXIT_FAILURE);
                    }

                    if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) == 0) {
                        size_t bytes_read = 0;

                        while ((bytes_read = read(fd, buffer, BUFF_SIZE)) > 0) {
                            if (send(new_socket_fd, buffer, bytes_read, 0) == -1) {
                                syslog(LOG_ERR, "Error: unable to send data to ioctl file");
                                closelog();
                                exit(EXIT_FAILURE);
                            }
                        }

                        close(fd);
                    } else {
                        syslog(LOG_ERR, "Error: unable to perform ioctl operation");
                        closelog();
                        exit(EXIT_FAILURE);
                    }
                }
            } else {
                if (fprintf(fileptr, "%s", buffer) < 0) {
                    syslog(LOG_ERR, "Error: unable to write to file");
                    closelog();
                    exit(EXIT_FAILURE);
                }

                // Force write to file from buffer
                fflush(fileptr);
            }

            // Check for newline in buffer
            char *newline = memchr(buffer, '\n', received);
            
            // If newline is found, exit loop and open file to begin writing to from buffer
            if (newline != NULL) break;
        }

        // Find file size after write
        fseek(fileptr, 0, SEEK_END);
        int filesize = ftell(fileptr);
        fseek(fileptr, 0, SEEK_SET);

        if (ioctl_enabled == 0) {
            ssize_t bytes_read;
            
            // Begin sending data to incoming connection in BUFF_SIZE-sized chunks
            while ((bytes_read = fread(buffer, sizeof(char), BUFF_SIZE - 1, fileptr)) > 0) {
                // Null terminate the end of the buffer
                buffer[bytes_read] = '\0';

                ssize_t bytes_sent;

                if ((bytes_sent = send(new_socket_fd, buffer, bytes_read, 0)) < 1) {
                    syslog(LOG_ERR, "Error: unable to send data to incoming connection");
                    closelog();
                    exit(EXIT_FAILURE);
                }
            }
        }
        
        fclose(fileptr);

        // Cleanup for thread and connection handler
        free(buffer);

        // if (close(new_socket_fd) != 0) {
        //     syslog(LOG_ERR, "Error: unable to close socket connection");
        // }

        syslog(LOG_INFO, "Closed connection from %s", connecting_address_str);
        #endif
    }

    // Cleanup
    // pthread_cancel(timestamp_thread_id);
    // pthread_join(timestamp_thread_id, NULL);

    #if !USE_AESD_CHAR_DEVICE
    pthread_mutex_lock(&thread_access);
    thread_data_t *thread = LIST_FIRST(&list_head);

    while (thread != NULL) {
        thread_data_t *next = LIST_NEXT(thread, entries);
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

    if (remove(FILE_PATH) != 0) {
        syslog(LOG_ERR, "Error: unable to delete temporary file");
    }
    #endif

    if (close(socket_fd) != 0) {
        syslog(LOG_ERR, "Error: unable to close socket connection");
    }

    closelog();
    exit(EXIT_SUCCESS);
}
