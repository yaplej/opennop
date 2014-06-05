#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <strings.h>
#include <errno.h>
#include <pthread.h> // for multi-threading
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/unistd.h>
#include <sys/fcntl.h>
#include <sys/epoll.h>

#include <linux/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "ipc.h"
#include "clicommands.h"
#include "logger.h"
#include "sockets.h"

typedef int (*t_neighbor_command)(int, __u32, char *);

/*
 * I was using "head" and "tail" here but that seemed to conflict with another module.
 * Should the internal variable names be isolated between modules?
 * They don't appear to be clicommands.c also uses a variable "head".
 * Using "static" should fix this.
 */
static pthread_t t_ipc; // thread for cli.
static struct neighbor_head ipchead;
static char UUID[OPENNOP_IPC_UUID_LENGTH]; //Local UUID.
static char key[OPENNOP_IPC_KEY_LENGTH]; //Local key.
struct opennop_message_header *opennop_msg_header;

int set_opennop_message_security(struct opennop_message_header *opennop_msg_header) {
	char message[LOGSZ] = {0};

    if (strcmp(key, "") == 0) {
        opennop_msg_header->security = OPENNOP_MSG_SECURITY_NO;
        sprintf(message, "IPC: NO security.\n");
    }else{
    	opennop_msg_header->security = OPENNOP_MSG_SECURITY_SHA;
    	sprintf(message, "IPC: SHA security.\n");
    }
    logger(LOG_INFO, message);

    return 0;
}

int initialize_opennop_message_header(struct opennop_message_header *opennop_msg_header) {
    opennop_msg_header->type = OPENNOP_MSG_TYPE_IPC;
    opennop_msg_header->version = OPENNOP_MSG_VERSION;
    opennop_msg_header->length = OPENNOP_DEFAULT_HEADER_LENGTH; // Header is at least 8 bytes.
    set_opennop_message_security(opennop_msg_header);
    opennop_msg_header->antireplay = OPENNOP_MSG_ANTI_REPLAY_NO;
    return 0;
}

int print_opennnop_header(struct opennop_message_header *opennop_msg_header) {
	struct opennop_message_data data;
    char message[LOGSZ] = {0};
    char securitydata[33] = {0};

    sprintf(message, "Type: %u\n", opennop_msg_header->type);
    logger(LOG_INFO, message);
    sprintf(message, "Version: %u\n", opennop_msg_header->version);
    logger(LOG_INFO, message);
    sprintf(message, "Length: %u\n", opennop_msg_header->length);
    logger(LOG_INFO, message);
    sprintf(message, "Security: %u\n", opennop_msg_header->security);
    logger(LOG_INFO, message);
    sprintf(message, "Anti-Replay: %u\n", opennop_msg_header->antireplay);
    logger(LOG_INFO, message);

    if(opennop_msg_header->security == 1){
        	data.securitydata = (char*)opennop_msg_header + sizeof(struct opennop_message_header);
        	memset(&securitydata, 0, sizeof(securitydata));
        	memcpy(&securitydata, data.securitydata, 32);
            sprintf(message, "Security Data: %s\n", securitydata);
            logger(LOG_INFO, message);
    }

    return 0;
}

int ipc_handler(int fd, void *buf) {
    char message[LOGSZ] = {0};
    /*
     *TODO: Here we need to check the message type of process it.
     *TODO: The epoll server should have a 2nd callback function that verifies the security of a connection.
     *TODO: So data passed from the epoll server to the handler *should* be considered secure.
     */
    sprintf(message, "IPC: Received a message\n");
    logger(LOG_INFO, message);
    print_opennnop_header((struct opennop_message_header *)buf);

    return 0;
}

int ipc_neighbor_hello(int socket) {
	struct opennop_message_data data;
    char buf[IPC_MAX_MESSAGE_SIZE] = {0};
    int error;
    char message[LOGSZ] = {0};

    /*
     * Setting up the OpenNOP Message Header.
     */
    opennop_msg_header = (struct opennop_message_header *)&buf;
    initialize_opennop_message_header(opennop_msg_header);
    sprintf(message, "IPC: Sending a message\n");
    logger(LOG_INFO, message);

    if(opennop_msg_header->security == 1){
    	data.securitydata = (char*)opennop_msg_header + sizeof(struct opennop_message_header);
    	memset(data.securitydata, 0, 32);
    	memcpy(data.securitydata, &key, 32);
    	data.messages = data.securitydata + 32;
    	opennop_msg_header->length = opennop_msg_header->length + 32;
    }else{
    	data.securitydata = NULL;
    	data.messages = (char*)opennop_msg_header + sizeof(struct opennop_message_header);
    }

    print_opennnop_header(opennop_msg_header);


    /**
     *TODO: When sending the buffer "opennop_msg_header->length" does not work correctly to transmit all the data.
     *TODO: I'm not sure if its caused by receiving side or the sending.
     *TODO: The length is reporting as 40 that is correct.  Sending 41 bytes seems to work correctly.
     *
     * Must ignore the signal typically caused when the remote stops responding by adding the MSG_NOSIGNAL flag.
     * http://stackoverflow.com/questions/1330397/tcp-send-does-not-return-cause-crashing-process
     */
    error = send(socket, buf, opennop_msg_header->length, MSG_NOSIGNAL);

    /**
     *TODO: It might be nice to make a separate function in sockets.c to handle sending data.
     *TODO: That function should check the results of send() to make sure all the data was send.
     *TODO: If not try sending the rest or error.
     */
    if(error != opennop_msg_header->length){
        sprintf(message, "[socket]: Only send %u bytes!\n", (unsigned int)error);
        logger(LOG_INFO, message);
    }

    return error;
}

int hello_neighbors(void) {
    struct neighbor *currentneighbor = NULL;
    time_t currenttime;
    int error = 0;
    char message[LOGSZ] = {0};

    time(&currenttime);

    for(currentneighbor = ipchead.next; currentneighbor != NULL; currentneighbor = currentneighbor->next) {

        if((currentneighbor->state == DOWN) && (difftime(currenttime, currentneighbor->timer) >= 30)) {
            currentneighbor->timer = currenttime;
            sprintf(message, "state is down & timer > 30\n");
            logger(LOG_INFO, message);

            /*
             * If neighbor socket = 0 open a new one.
             */
            if(currentneighbor->sock == 0) {
                error = new_ip_client(currentneighbor->NeighborIP,OPENNOPD_IPC_PORT);

                if(error > 0) {
                    currentneighbor->sock = error;
                    currentneighbor->state = ATTEMPT;

                }
            }

        } else if((currentneighbor->state >= ATTEMPT) && (difftime(currenttime, currentneighbor->timer) >= 10)) {
            /*
             * TODO:
             * If we were successful in opening a connection we should sent a hello message.
             * Write the hello message function.
             */
            if(currentneighbor->sock != 0) {
                currentneighbor->timer = currenttime;
                error = ipc_neighbor_hello(currentneighbor->sock);

                /*
                 * Maybe a lot of this should be moved to ipc_neighbor_hello().
                 */
                if (error < 0) {
                    sprintf(message, "Failed sending hello.\n");
                    logger(LOG_INFO, message);
                    currentneighbor->state = DOWN;
                    shutdown(currentneighbor->sock, SHUT_RDWR);
                    currentneighbor->sock = 0;
                }
            }
        }
    }
    return 0;
}

/*
 * This thread will monitor all IPC events.
 * It has to monitor in-bound connection for remote IPC
 * and also handle in-bound local IPC from the CLI.
 *
 * The CLI will use the local IPC to notify the IPC thread
 * that new fd's need to be setup and monitored.
 * For events like adding/removing neighbors from the IPC.
 *
 * Thanks to Mukund Sivaraman for the tutorial on epoll().
 * https://banu.com/blog/2/how-to-use-epoll-a-complete-example-in-c/
 * http://man7.org/linux/man-pages/man7/epoll.7.html
 * http://www.cs.rutgers.edu/~pxk/rutgers/notes/sockets/
 * http://www.beej.us/guide/bgnet/output/html/multipage/fcntlman.html
 */
void *ipc_thread(void *dummyPtr) {
    int error = 0;
    struct epoll_server ipc_server = {
                                         0
                                     };
    char message[LOGSZ] = {0};

    sprintf(message, "IPC: Is starting.\n");
    logger(LOG_INFO, message);

    error = new_ip_epoll_server(&ipc_server, ipc_handler, OPENNOPD_IPC_PORT);

    /*
     * This should not return until the epoll server is shutdown.
     */
    epoll_handler(&ipc_server);

    sprintf(message, "IPC: Is exiting.\n");
    logger(LOG_INFO, message);

    shutdown_epoll_server(&ipc_server);

    return EXIT_SUCCESS;
}

int cli_neighbor_help(int client_fd) {
    char msg[IPC_MAX_MESSAGE_SIZE] = { 0 };

    sprintf(msg,"Usage: [no] neighbor <ip address> [key]\n");
    cli_send_feedback(client_fd, msg);

    return 0;
}

struct commandresult cli_show_neighbors(int client_fd, char **parameters, int numparameters, void *data) {
    struct neighbor *currentneighbor = NULL;
    struct commandresult result  = {
                                       0
                                   };
    char temp[20];
    char col1[17];
    char col2[33];
    char col3[66];
    char end[3];
    char msg[IPC_MAX_MESSAGE_SIZE] = { 0 };

    currentneighbor = ipchead.next;

    sprintf(
        msg,
        "---------------------------------------------------------------------------------------------------------\n");
    cli_send_feedback(client_fd, msg);
    sprintf(
        msg,
        "|   Neighbor IP   |       GUID       |                               Key                                |\n");
    cli_send_feedback(client_fd, msg);
    sprintf(
        msg,
        "---------------------------------------------------------------------------------------------------------\n");
    cli_send_feedback(client_fd, msg);

    while(currentneighbor != NULL) {
        strcpy(msg, "");
        inet_ntop(AF_INET, &currentneighbor->NeighborIP, temp,
                  INET_ADDRSTRLEN);
        sprintf(col1, "| %-16s", temp);
        strcat(msg, col1);
        sprintf(col2, "| %-17s", currentneighbor->UUID);
        strcat(msg, col2);
        sprintf(col3, "| %-65s", currentneighbor->key);
        strcat(msg, col3);
        sprintf(end, "|\n");
        strcat(msg, end);
        cli_send_feedback(client_fd, msg);

        currentneighbor = currentneighbor->next;
    }

    sprintf(
        msg,
        "---------------------------------------------------------------------------------------------------------\n");
    cli_send_feedback(client_fd, msg);

    result.finished = 0;
    result.mode = NULL;
    result.data = NULL;

    return result;
}

int set_neighbor_key(struct neighbor *currentneighbor, char *key) {
    memset(currentneighbor->key, 0, sizeof(currentneighbor->key));

    if(key != NULL) {
        strncpy(currentneighbor->key, key, strlen(key));
    }
    return 0;
}

struct neighbor* allocate_neighbor(__u32 neighborIP, char *key) {
    struct neighbor *newneighbor = (struct neighbor *) malloc (sizeof (struct neighbor));

    if(newneighbor == NULL) {
        fprintf(stdout, "Could not allocate memory... \n");
        exit(1);
    }
    newneighbor->next = NULL;
    newneighbor->prev = NULL;
    newneighbor->NeighborIP = 0;
    newneighbor->state = DOWN;
    newneighbor->UUID[0] = '\0';
    newneighbor->sock = 0;
    newneighbor->key[0] = '\0';
    time(&newneighbor->timer);

    if (neighborIP != 0) {
        newneighbor->NeighborIP = neighborIP;
    }

    if (key != NULL) {
        set_neighbor_key(newneighbor, key);
    }

    return newneighbor;
}



int add_update_neighbor(int client_fd, __u32 neighborIP, char *key) {
    struct neighbor *currentneighbor = NULL;

    currentneighbor = ipchead.next;

    /*
     * Make sure the neighbor does not already exist.
     */
    while (currentneighbor != NULL) {

        if (currentneighbor->NeighborIP == neighborIP) {

            set_neighbor_key(currentneighbor, key);

            return 0;
        }

        currentneighbor = currentneighbor->next;
    }

    /*
     * Did not find the neighbor so lets add it.
     */

    currentneighbor = allocate_neighbor(neighborIP, key);

    if (currentneighbor != NULL) {

        if (ipchead.next == NULL) {
            ipchead.next = currentneighbor;
            ipchead.prev = currentneighbor;

        } else {
            currentneighbor->prev = ipchead.prev;
            ipchead.prev->next = currentneighbor;
            ipchead.prev = currentneighbor;
        }

    }

    return 0;
}

int del_neighbor(int client_fd, __u32 neighborIP, char *key) {
    struct neighbor *currentneighbor = NULL;

    currentneighbor = ipchead.next;

    while (currentneighbor != NULL) {

        if (currentneighbor->NeighborIP == neighborIP) {

            if((currentneighbor->prev == NULL) && (currentneighbor->next == NULL)) {
                ipchead.next = NULL;
                ipchead.prev = NULL;

            } else if ((currentneighbor->prev == NULL) && (currentneighbor->next != NULL)) {
                ipchead.next = currentneighbor->next;
                currentneighbor->next->prev = NULL;

            } else if ((currentneighbor->prev != NULL) && (currentneighbor->next == NULL)) {
                ipchead.prev = currentneighbor->prev;
                currentneighbor->prev->next = NULL;

            } else if ((currentneighbor->next != NULL) && (currentneighbor->prev != NULL)) {
                currentneighbor->prev->next = currentneighbor->next;
                currentneighbor->next->prev = currentneighbor->prev;
            }

            free(currentneighbor);
            currentneighbor = NULL;

            return 0;
        }

        currentneighbor = currentneighbor->next;
    }

    return 0;
}

int validate_neighbor_input(int client_fd, char *stringip, char *key, t_neighbor_command neighbor_command) {
    int ERROR = 0;
    int keylength = 0;
    __u32 neighborIP = 0;
    char msg[IPC_MAX_MESSAGE_SIZE] = { 0 };

    /*
     * We must validate the user data here
     * before adding or updating anything.
     * 1. stringip should convert to an integer using inet_pton()
     * 2. key should be NULL or < 64 bytes
     */

    ERROR = inet_pton(AF_INET, stringip, &neighborIP); //Should return 1.

    if(ERROR != 1) {
        return cli_neighbor_help(client_fd);
    }

    if(key != NULL) {
        keylength = strlen(key);

        if(keylength > 64) {
            return cli_neighbor_help(client_fd);
        }
    }

    /*
     * The input is valid so we run the specified command
     * This will add, update or remove a neighbor.
     */
    neighbor_command(client_fd, neighborIP, key);

    /*
    sprintf(msg,"Neighbor string IP is [%s].\n", stringip);
    cli_send_feedback(client_fd, msg);
    sprintf(msg,"Neighbor integer IP is [%u].\n", ntohl(neighborIP));
    cli_send_feedback(client_fd, msg);
    sprintf(msg,"Neighbor key is [%s].\n", key);
    cli_send_feedback(client_fd, msg);

    if(key != NULL) {
        sprintf(msg,"Neighbor key length is [%u].\n", keylength);
        cli_send_feedback(client_fd, msg);
}
    */

    return 0;
}

/*
 * We only need to validate the number of parameters are correct here.
 * Lets let the more specific functions do the data validation from the input.
 * This should accept 1 parameter.
 * parameter[0] = IP in string format.
 */
struct commandresult cli_no_neighbor(int client_fd, char **parameters, int numparameters, void *data) {
    int ERROR = 0;
    struct commandresult result = {
                                      0
                                  };

    if (numparameters == 1) {
        ERROR = validate_neighbor_input(client_fd, parameters[0], NULL, &del_neighbor);

    } else {
        ERROR = cli_neighbor_help(client_fd);
    }

    result.finished = 0;
    result.mode = NULL;
    result.data = NULL;

    return result;
}

/*
 * We only need to validate the number of parameters are correct here.
 * Lets let the more specific functions do the data validation from the input.
 * This should accept 1 or 2 parameters.
 * parameter[0] = IP in string format.
 * parameter[1] = Authentication key(optional).
 */
struct commandresult cli_neighbor(int client_fd, char **parameters, int numparameters, void *data) {
    int socket = 0;
    int ERROR = 0;
    struct commandresult result  = {
                                       0
                                   };
    char buf[IPC_MAX_MESSAGE_SIZE];
    char message[LOGSZ] = {0};

    result.mode = NULL;
    result.data = NULL;

    if ((numparameters < 1) || (numparameters > 2)) {
        ERROR = cli_neighbor_help(client_fd);

    } else if (numparameters == 1) {
        ERROR = validate_neighbor_input(client_fd, parameters[0], NULL, &add_update_neighbor);

    } else if (numparameters == 2) {
        ERROR = validate_neighbor_input(client_fd, parameters[0], parameters[1], &add_update_neighbor);
    }

    /*
     * The IPC does not respond to UNIX sockets anymore.
     */
    /*socket = new_unix_client(OPENNOPD_IPC_SOCK);

    if (socket < 0) {
        sprintf(message, "IPC: CLI failed to connect.\n");
        logger(LOG_INFO, message);
        result.finished = 1;
        close(socket);
        return result;
}

    sprintf(buf,"Hello!\n");
    ERROR = send(socket, buf, strlen(buf), 0);

    if (ERROR < 0) {
        sprintf(message, "IPC: CLI could not write to IPC socket.\n");
        logger(LOG_INFO, message);
        result.finished = 1;
        close(socket);
        return result;
}
    ERROR = shutdown(socket, SHUT_WR);
    */

    /*
     * TODO: Here we should read from the socket for x seconds.
     * If we receive the proper shutdown from the server we can close the socket.
     */

    result.finished = 0;

    return result;
}

struct commandresult cli_set_key(int client_fd, char **parameters, int numparameters, void *data) {
    int ERROR = 0;
    int keylength = 0;
    struct commandresult result  = {
                                       0
                                   };
    char buf[IPC_MAX_MESSAGE_SIZE];
    char message[LOGSZ] = {0};

    result.mode = NULL;
    result.data = NULL;

    if (numparameters == 1){

        if(parameters[0] != NULL) {
            keylength = strlen(parameters[0]);

            if(keylength > 64) {
                result.finished = 0;
                return result;
            }
            memset(key, 0, sizeof(key));
            strncpy(key, parameters[0], strlen(parameters[0]));

        }
    }

    if(numparameters == 0){
    	memset(key, 0, sizeof(key));
    }

    result.finished = 0;

    return result;
}

struct commandresult cli_show_key(int client_fd, char **parameters, int numparameters, void *data) {
    char msg[IPC_MAX_MESSAGE_SIZE] = { 0 };
    struct commandresult result  = {
                                       0
                                   };

    result.mode = NULL;
    result.data = NULL;
    sprintf(msg,"Key: [%s]\n",key);
    cli_send_feedback(client_fd, msg);
    result.finished = 0;
    return result;
}

/*
 * We need to verify that the remote accelerator is a member of this domain.
 * TODO: Later the UUID will need to be used instead of the IP address.
 */
int verify_neighbor_in_domain(__u32 neighborIP) {
    struct neighbor *currentneighbor = NULL;

    currentneighbor = ipchead.next;

    while (currentneighbor != NULL) {

        if (currentneighbor->NeighborIP == neighborIP) {

            return 1;
        }

        currentneighbor = currentneighbor->next;
    }

    return 0;
}

void start_ipc() {
    /*
     * This will setup and start the IPC threads.
     * We also register the IPC commands here.
     */
    register_command(NULL, "neighbor", cli_neighbor, true, false);
    register_command(NULL, "no neighbor", cli_no_neighbor, true, false);
    register_command(NULL, "show neighbors", cli_show_neighbors, false, false);
    register_command(NULL, "key", cli_set_key, true, true);
    register_command(NULL, "show key", cli_show_key, false, true);

    ipchead.next = NULL;
    ipchead.prev = NULL;

    pthread_create(&t_ipc, NULL, ipc_thread, (void *) NULL);

}

void rejoin_ipc() {
    pthread_join(t_ipc, NULL);
}
