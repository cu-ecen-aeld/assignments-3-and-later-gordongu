#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>
#include <syslog.h>

// Constants
#define PORT "9000"
#define BUFF_SIZE 1024
const char *filepath = "/var/tmp/aesdsocketdata";

// Dynamically allocated variables that need to be freed
char *receive_buffer;
char *write_buffer;
struct addrinfo *servinfo;
int socket_fd, new_socket_fd;

// Close sockets, free dynamically allocated memory, and delete file on interruption or termination signals
void interrupt_signal_handler() {
    close(socket_fd);
    close(new_socket_fd);
    free(receive_buffer);
    free(write_buffer);
    freeaddrinfo(servinfo);
    if (remove(filepath) != 0) {
        syslog(LOG_ERR, "Error: unable to delete temporary file");
    }
    syslog(LOG_INFO, "Caught signal, exiting");
    closelog();
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    // Open syslog to begin logging
    openlog("aesdsocket", LOG_CONS | LOG_PID, LOG_USER);

    FILE *fileptr;
    struct addrinfo hints;

    // Set hints properties
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // Check if address info can be retreived
    if (getaddrinfo(NULL, PORT, &hints, &servinfo) != 0) {
        syslog(LOG_ERR, "Error: unable to get local address info");
        closelog();
        return -1;
    }

    // Get local address and prepare incoming connecting address
    struct sockaddr local_address = *servinfo->ai_addr;
    struct sockaddr connecting_address;
    socklen_t connecting_address_len = sizeof connecting_address;

    // Check if new socket was created successfully
    if ((socket_fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        freeaddrinfo(servinfo);
        syslog(LOG_ERR, "Error: unable to create socket for listening for incoming connections");
        closelog();
        return -1;
    }

    // Check if binding socket and local address was successful
    if (bind(socket_fd, &local_address, sizeof local_address) < 0) {
        freeaddrinfo(servinfo);
        syslog(LOG_ERR, "Error: unable to bind socket and local address");
        closelog();
        return -1;
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
        freeaddrinfo(servinfo);
        syslog(LOG_ERR, "Error: socket unable to listen for incoming connections");
        closelog();
        return -1;
    }

    // Dynamically allocate memory to buffers
    receive_buffer = malloc(BUFF_SIZE);
    write_buffer = malloc(BUFF_SIZE);

    // Bind interrupt and terminate signals to interrupt function
    signal(SIGINT, interrupt_signal_handler);
    signal(SIGTERM, interrupt_signal_handler);

    // Main process loop
    while (1) {
        // Check if incoming connection can be accepted and if new socket was created successfully
        if ((new_socket_fd = accept(socket_fd, &connecting_address, &connecting_address_len)) < 0) {
            free(receive_buffer);
            free(write_buffer);
            freeaddrinfo(servinfo);
            syslog(LOG_ERR, "Error: unable to accept incoming connection");
            closelog();
            return -1;
        }

        // Get connecting address' IP address as string
        char *connecting_address_str = inet_ntoa(((struct sockaddr_in *)&connecting_address)->sin_addr);

        syslog(LOG_INFO, "Accepted connection from %s", connecting_address_str);

        ssize_t received;
        int size = 0;

        // Begin receiving data from incoming connection
        while ((received = recv(new_socket_fd, receive_buffer, BUFF_SIZE - 1, 0)) > 0) {
            // Null terminate the end of the receiving buffer
            receive_buffer[received] = '\0';

            // Dynamically allocate more memory to buffers if received data exceeds default buffer size
            if (size + received >= BUFF_SIZE) {
                ssize_t new_buff_size = size + received + 1;

                char *new_receive_buffer = realloc(receive_buffer, new_buff_size);

                if (new_receive_buffer == NULL) {
                    freeaddrinfo(servinfo);
                    syslog(LOG_ERR, "Error: unable to reallocate memory for receiving buffer");
                    closelog();
                    return -1;
                }

                char *new_write_buffer = realloc(write_buffer, new_buff_size);

                if (new_write_buffer == NULL) {
                    freeaddrinfo(servinfo);
                    syslog(LOG_ERR, "Error: unable to reallocate memory for writing buffer");
                    closelog();
                    return -1;
                }

                receive_buffer = new_receive_buffer;
                write_buffer = new_write_buffer;
            }

            // Copy data from receiving buffer to writing buffer and null terminate the end of the writing buffer
            memcpy(write_buffer + size, receive_buffer, received);
            size += received;
            write_buffer[size] = '\0';

            // Check for newline in writing buffer
            char *newline = memchr(write_buffer, '\n', size);
            
            // If newline is found, open file and begin writing to file from writing buffer
            if (newline != NULL) {
                fileptr = fopen(filepath, "a+");

                // Write to file from writing buffer
                if (fprintf(fileptr, "%s", write_buffer) < 0) {
                    free(receive_buffer);
                    free(write_buffer);
                    freeaddrinfo(servinfo);
                    syslog(LOG_ERR, "Error: unable to write to file");
                    closelog();
                    return -1;
                }

                // Find file size after write
                fseek(fileptr, 0, SEEK_END);
                int filesize = ftell(fileptr);
                fseek(fileptr, 0, SEEK_SET);

                char *send_buffer = malloc(BUFF_SIZE);
                ssize_t read;
                
                // Begin sending data to incoming connection in BUFF_SIZE-sized chunks
                while ((read = fread(send_buffer, sizeof(char), BUFF_SIZE - 1, fileptr)) > 0) {
                    // Null terminate the end of the sending buffer
                    send_buffer[read] = '\0';

                    ssize_t sent;

                    if ((sent = send(new_socket_fd, send_buffer, read, 0)) < 1) {
                        free(receive_buffer);
                        free(write_buffer);
                        free(send_buffer);
                        freeaddrinfo(servinfo);
                        syslog(LOG_ERR, "Error: unable to send data to incoming connection");
                        closelog();
                        return -1;
                    }
                }
                
                fclose(fileptr);

                // Reset for potential new incoming data
                size = 0;

                free(send_buffer);
            }
        }

        syslog(LOG_INFO, "Closed connection from %s", connecting_address_str);
    }

    return 0;
}
