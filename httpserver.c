#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <poll.h>

#define OPTIONS              "t:l:"
#define BUF_SIZE             4096
#define DEFAULT_THREAD_COUNT 4

static FILE *logfile;
#define LOG(...) fprintf(logfile, __VA_ARGS__);

extern int errno;

volatile sig_atomic_t gSignalStatus = 0;
int jobQueue[1000] = { -1 };
int in = 0, out = 0, total_requests = 0, size = 1000;

int totalThreads = 0, threadUsage = 0;

pthread_mutex_t mutex;
sem_t empty_sem;
sem_t full_sem;

//Enumerated object which specifies the types of requests the client can send.
typedef enum method {
    _GET_ = 0,
    _PUT_ = 1,
    _APPEND_ = 2,
} method;

//Struct to hold all aspects of the client request. Holds 3 ints and 3 char buffers.
struct clientRequest {
    int socket;
    int method;
    int messageLength;
    int requestID;
    int prelimLen;
    char methodStr[300];
    char fileName[300];
    char httpType[300];
    char partMessage[BUF_SIZE];

} clientRequest;

struct clientRequest saveState[1000];
int saveIn = 0, saveOut = 0, totalSaved = 0, saveSize = 1000;

//Function that takes a status code and a pointer to the string of correct response.
const char *Status(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    }
    return NULL;
}

// Converts a string to an 16 bits unsigned integer.
// Returns 0 if the string is malformed or out of the range.
static size_t strtouint16(char number[]) {
    char *last;
    long num = strtol(number, &last, 10);
    if (num <= 0 || num > UINT16_MAX || *last != '\0') {
        return 0;
    }
    return num;
}

// Creates a socket for listening for connections.
// Closes the program and prints an error message on error.
static int create_listen_socket(uint16_t port) {
    struct sockaddr_in addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        err(EXIT_FAILURE, "socket error");
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htons(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *) &addr, sizeof addr) < 0) {
        err(EXIT_FAILURE, "bind error");
    }
    if (listen(listenfd, 128) < 0) {
        err(EXIT_FAILURE, "listen error");
    }
    return listenfd;
}

/*
Function that takes a clientRequest object and an integer representing status code
Will print response based on code
*/
void servResponse(int code, struct clientRequest rObj) {
    char response[BUF_SIZE];
    if (rObj.method == _PUT_) { //PUT situation
        int slen = strlen(Status(code)) + 1;

        sprintf(response, "HTTP/1.1 %d %s\r\nContent-Length: %d\r\n\r\n%s\n", code, Status(code),
            slen, Status(code));
        send(rObj.socket, response, strlen(response), 0);
    }

    else if (rObj.method == _APPEND_) { //APPEND situation
        int slen = strlen(Status(code)) + 1;

        sprintf(response, "HTTP/1.1 %d %s\r\nContent-Length: %d\r\n\r\n%s\n", code, Status(code),
            slen, Status(code));
        send(rObj.socket, response, strlen(response), 0);
    }

    else { //Get Situation
        if (code == 200) {
            sprintf(response, "HTTP/1.1 %d %s\r\nContent-Length: %d\r\n\r\n", code, Status(code),
                rObj.messageLength);

            send(rObj.socket, response, strlen(response), 0);
        } else {

            int slen = strlen(Status(code)) + 1;
            sprintf(response, "HTTP/1.1 %d %s \r\nContent-Length: %d\r\n\r\n%s\n", code,
                Status(code), slen, Status(code));
            send(rObj.socket, response, strlen(response), 0);
        }
    }
}

/*
*Function that takes a clientRequest Object pointer
*Based on the data on given object, will determine the validity of request
*Returns 1 if is a bad request, 0 if valid
*/
int isBadRequest(struct clientRequest *rObj) {
    if (!(strncmp(rObj->httpType, "HTTP/1.1", 8) == 0)) {
        return 1;
    }

    if (!(strncmp(rObj->fileName, "//", 1) == 0)) {
        return 1;
    }
    memmove(rObj->fileName, rObj->fileName + 1, strlen(rObj->fileName));
    if (strlen(rObj->fileName) > 19) {
        return 1;
    }

    //Check valid characters in fileName
    for (size_t i = 0; i < strlen(rObj->fileName); i++) {
        if (!(isalnum(rObj->fileName[i])) && (rObj->fileName[i] != '_')
            && (rObj->fileName[i] != '.')) {
            return 1;
        }
    }

    return 0;
}

/*
Function that takes a clientRequest Object
Checks if method requested is valid
Returns an integer indicating validity.
*/
int validReq(struct clientRequest *rObj) {
    if (strcmp(rObj->methodStr, "GET") == 0 || strcmp(rObj->methodStr, "PUT") == 0
        || strcmp(rObj->methodStr, "APPEND") == 0)
        return 0;
    else
        return 1;
}

void readWriteFD(int in_FD, int out_FD, struct clientRequest rObj) {

    char *buf = (char *) malloc(BUF_SIZE);
    int rd = -1, wr = -1;
    int bytes = 0;
    //Reading and writing to specified areas
    memset(buf, 0, sizeof(BUF_SIZE));
    while (bytes < rObj.messageLength) {

        rd = read(in_FD, buf, BUF_SIZE);
        if (rd == 0) {
            printf("No connection, breaking\n");
            break;
        }

        bytes += (int) rd;

        //Locking file to be written to based on method
        if (rObj.method == _GET_) {
            while (flock(out_FD, LOCK_SH) != 0)
                continue;
        } else {
            while (flock(out_FD, LOCK_EX) != 0) {
                continue;
            }
        }

        wr = write(out_FD, buf, rd);

        flock(out_FD, LOCK_UN);

        memset(buf, 0, sizeof(BUF_SIZE));
    }
    free(buf);
}

int processGET(int connfd, struct clientRequest rObj) {
    int this_file = open(rObj.fileName, O_RDONLY, S_IRWXU);
    //Checking file premissions
    if (this_file == -1 && errno != EACCES) {
        int code = 404;
        pthread_mutex_lock(&mutex);

        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        fflush(logfile);
        servResponse(code, rObj);

        pthread_mutex_unlock(&mutex);

        return -1;
    } else if (errno == EACCES) {
        int code = 403;
        pthread_mutex_lock(&mutex);
        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        fflush(logfile);
        servResponse(code, rObj);
        pthread_mutex_unlock(&mutex);

        return -1;
    } else {
        struct stat sb;
        fstat(this_file, &sb);
        rObj.messageLength = sb.st_size;
        int code = 200;
        pthread_mutex_lock(&mutex);
        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        fflush(logfile);
        servResponse(code, rObj);
        pthread_mutex_unlock(&mutex);
        readWriteFD(this_file, connfd, rObj);

        close(this_file);
        return 1;
    }
}

int processPUT(int connfd, struct clientRequest rObj) {
    int this_file = open(rObj.fileName, O_WRONLY | O_TRUNC, S_IRWXU);

    //Checking file premissions
    if (this_file == -1) {
        if (errno == EACCES) {
            int code = 403;
            pthread_mutex_lock(&mutex);

            LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
            fflush(logfile);
            servResponse(code, rObj);

            pthread_mutex_unlock(&mutex);
            return -1;
        } else {
            this_file = creat(rObj.fileName, S_IRWXU);

            readWriteFD(connfd, this_file, rObj);
            int code = 201;
            pthread_mutex_lock(&mutex);

            LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
            fflush(logfile);
            servResponse(code, rObj);
            pthread_mutex_unlock(&mutex);
        }
    } else {
        readWriteFD(connfd, this_file, rObj);
        int code = 200;
        pthread_mutex_lock(&mutex);
        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        fflush(logfile);
        servResponse(code, rObj);
        pthread_mutex_unlock(&mutex);
    }

    close(this_file);
    return 1;
}

int processAPPEND(int connfd, struct clientRequest rObj) {
    int this_file = open(rObj.fileName, O_WRONLY | O_APPEND, S_IRWXU);
    //Checking file premissions
    if (this_file == -1 && errno != EACCES) {
        int code = 404;
        pthread_mutex_lock(&mutex);
        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        fflush(logfile);
        servResponse(code, rObj);
        pthread_mutex_unlock(&mutex);
        return -1;
    } else if (errno == EACCES) {
        int code = 403;
        pthread_mutex_lock(&mutex);
        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        servResponse(code, rObj);
        pthread_mutex_unlock(&mutex);
        return -1;
    } else {
        readWriteFD(connfd, this_file, rObj);
        int code = 200;
        pthread_mutex_lock(&mutex);
        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        fflush(logfile);
        servResponse(code, rObj);
        pthread_mutex_unlock(&mutex);
        close(this_file);
        return 1;
    }
}
static void handle_connection(int connfd) {
    char buff[BUF_SIZE + 1];
    char header[BUF_SIZE];
    char content[BUF_SIZE];
    char *headEnd = NULL;
    char *modhead = NULL;
    ssize_t bytes;
    int poller = 0;
    int ID = 0;
    int pastReq = 0;
    struct clientRequest rObj = { 0 };
    memset(&rObj.methodStr, 0, sizeof(rObj.methodStr));
    memset(&rObj.fileName, 0, sizeof(rObj.fileName));
    memset(&rObj.httpType, 0, sizeof(rObj.httpType));
    rObj.socket = connfd;
    struct pollfd fds[1];
    fds[0].fd = connfd;
    fds[0].events = 0;
    fds[0].events |= POLL_IN;

    memset(&buff, 0, sizeof(buff));
    memset(&header, 0, sizeof(header));
    memset(&content, 0, sizeof(content));
    //Getting Request (Read Request Byte by Byte
    //Checking to see if connection hangs too long
    if (totalSaved != 0) {

        for (int i = saveOut; i < saveIn; i++) {

            if (connfd == saveState[i].socket) {

                pthread_mutex_lock(&mutex);
                pastReq = 1;
                strcpy(rObj.methodStr, saveState[i].methodStr);
                strcpy(rObj.fileName, saveState[i].fileName);
                strcpy(rObj.httpType, saveState[i].httpType);
                saveOut = (saveOut + 1) % saveSize;
                totalSaved--;
                pthread_mutex_unlock(&mutex);
            }
        }
    }

    poller = poll(fds, 1, 5);

    if (poller == 0 && totalSaved != saveSize && total_requests > 0) {

        sem_wait(&empty_sem);
        pthread_mutex_lock(&mutex);
        jobQueue[in] = connfd;
        in = (in + 1) % size;
        total_requests++;
        pthread_mutex_unlock(&mutex);
        sem_post(&full_sem);
        return;
    }
    //Reading for request
    while (pastReq != 1) {

        bytes = recv(connfd, buff, sizeof(char), 0);
        if (bytes == 0) {
            return;
        }
        if (bytes < 0) {
            int code = 500;
            pthread_mutex_lock(&mutex);
            LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, 0);
            fflush(logfile);
            servResponse(code, rObj);
            pthread_mutex_unlock(&mutex);
            close(connfd);
            return;
        }
        strncat(header, buff, sizeof(char));

        headEnd = strstr(header, "\r\n");
        if (headEnd != NULL) {

            break;
        }
        memset(&buff, 0, sizeof(buff));
    }

    //Getting Method, Filename, and httptype
    if (pastReq != 1) {
        sscanf(header, "%s %s %s", rObj.methodStr, rObj.fileName, rObj.httpType);
    }
    //Checking for Saved Connfd's to do work

    poller = poll(fds, 1, 5);
    if (poller == 0 && totalSaved != saveSize && total_requests != 0) {
        pthread_mutex_lock(&mutex);
        saveState[saveIn].socket = connfd;
        strcpy(saveState[saveIn].methodStr, rObj.methodStr);
        strcpy(saveState[saveIn].fileName, rObj.fileName);
        strcpy(saveState[saveIn].httpType, rObj.httpType);
        saveIn = (saveIn + 1) % saveSize;
        totalSaved++;
        pthread_mutex_unlock(&mutex);
        sem_wait(&empty_sem);
        pthread_mutex_lock(&mutex);
        jobQueue[in] = connfd;
        in = (in + 1) % size;
        total_requests++;
        pthread_mutex_unlock(&mutex);
        sem_post(&full_sem);
        return;
    }

    //Getting Header (Read Header Byte by Byte);
    memset(&buff, 0, sizeof(buff));
    while (1) {
        bytes = recv(connfd, buff, sizeof(char), 0);
        if (bytes == 0) {
            return;
        }
        if (bytes < 0) {
            int code = 500;
            pthread_mutex_lock(&mutex);
            LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, 0);
            fflush(logfile);
            servResponse(code, rObj);
            pthread_mutex_unlock(&mutex);
            close(connfd);
            return;
        }
        strncat(header, buff, sizeof(char));
        headEnd = strstr(header, "\r\n\r\n");
        if (headEnd != NULL) {
            break;
        }
        memset(&buff, 0, sizeof(buff));
    }
    modhead = strstr(header, "Request-Id:");

    //------Getting Request ID-----
    if (modhead == NULL) {
        rObj.requestID = ID;
    } else {
        sscanf(modhead, "Request-Id: %d", &ID);
        rObj.requestID = ID;
    }
    int status = -1;
    //-----Checking if bad Request-----
    if (isBadRequest(&rObj) == 1) {
        int code = 400;
        pthread_mutex_lock(&mutex);
        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        fflush(logfile);
        servResponse(code, rObj);
        pthread_mutex_unlock(&mutex);
        close(connfd);
        return;
    }
    //------Checking if Valid Method------
    if (validReq(&rObj) == 1) {
        int code = 501;
        pthread_mutex_lock(&mutex);
        LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
        fflush(logfile);
        servResponse(code, rObj);
        pthread_mutex_unlock(&mutex);
        close(connfd);
        return;
    }
    //-----Get Method-----
    else if (strcmp(rObj.methodStr, "GET") == 0) {

        rObj.method = _GET_;
        status = processGET(connfd, rObj);
        close(connfd);
        return;
    }
    //-----Put Method-----
    else if (strcmp(rObj.methodStr, "PUT") == 0) {

        rObj.method = _PUT_;
        modhead = strstr(header, "Content-Length: ");
        if (modhead == NULL) {
            int code = 400;
            pthread_mutex_lock(&mutex);
            LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
            fflush(logfile);
            servResponse(code, rObj);
            pthread_mutex_unlock(&mutex);
            close(connfd);
            return;
        } else {
            sscanf(modhead, "Content-Length: %d", &rObj.messageLength);
        }
        status = processPUT(connfd, rObj);
        close(connfd);
        return;
    }
    //-----Append Method------
    else if (strcmp(rObj.methodStr, "APPEND") == 0) {

        rObj.method = _APPEND_;
        modhead = strstr(header, "Content-Length: ");
        if (modhead == NULL) {
            int code = 400;
            pthread_mutex_lock(&mutex);
            LOG("%s,/%s,%d,%d\n", rObj.methodStr, rObj.fileName, code, rObj.requestID);
            fflush(logfile);
            servResponse(code, rObj);
            pthread_mutex_unlock(&mutex);
            close(connfd);
            return;
        } else {
            sscanf(modhead, "Content-Length: %d", &rObj.messageLength);
        }

        status = processAPPEND(connfd, rObj);
        close(connfd);
        return;
    }
}

void *workerThread() {

    while (gSignalStatus == 0) {
        sem_wait(&full_sem);
        if (gSignalStatus == 1) {
            break;
        }
        pthread_mutex_lock(&mutex);

        int job = jobQueue[out];
        out = (out + 1) % size;
        total_requests--;
        threadUsage++;
        pthread_mutex_unlock(&mutex);
        sem_post(&empty_sem);
        handle_connection(job);
        pthread_mutex_lock(&mutex);

        threadUsage--;
        pthread_mutex_unlock(&mutex);
    }

    return NULL;
}

static void sigterm_handler(int sig) {
    if (sig == SIGTERM) {

        gSignalStatus = 1;
    }
}

static void sigint_handler(int sig) {
    if (sig == SIGINT) {

        gSignalStatus = 1;
    }
}

static void usage(char *exec) {
    fprintf(stderr, "usage: %s [-t threads] [-l logfile] <port>\n", exec);
}

int main(int argc, char *argv[]) {
    int opt = 0;
    int threads = DEFAULT_THREAD_COUNT;
    logfile = stderr;

    while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
        switch (opt) {
        case 't':
            threads = strtol(optarg, NULL, 10);
            if (threads <= 0) {
                errx(EXIT_FAILURE, "bad number of threads");
            }
            break;
        case 'l':
            logfile = fopen(optarg, "w");
            if (!logfile) {
                errx(EXIT_FAILURE, "bad logfile");
            }
            break;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        warnx("wrong number of arguments");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    uint16_t port = strtouint16(argv[optind]);
    if (port == 0) {
        errx(EXIT_FAILURE, "bad port number: %s", argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigint_handler);

    int listenfd = create_listen_socket(port);
    struct pollfd fds[1];
    fds[0].fd = listenfd;
    fds[0].events = 0;
    fds[0].events |= POLL_IN;
    pthread_mutex_init(&mutex, NULL);
    sem_init(&full_sem, 0, 0);
    sem_init(&empty_sem, 0, 1000);
    pthread_t thread_pool[threads];
    totalThreads = threads;
    //start workers
    for (int i = 0; i < threads; i += 1) {
        if (pthread_create(&thread_pool[i], NULL, &workerThread, NULL) != 0) {
            err(EXIT_FAILURE, "PTHREAD_CREATE() failed");
        }
    }
    while (1) {
        int poller = 0;
        while (poller == 0) {
            if (gSignalStatus == 1) {
                break;
            }
            poller = poll(fds, 1, 5);
            if (poller > 1)
                break;
            else if (poller < 0)
                break;
            else
                continue;
        }
        if (gSignalStatus == 1)
            break;
        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            warn("accept error");
            continue;
        }
        if (gSignalStatus == 1)
            break;

        sem_wait(&empty_sem);
        pthread_mutex_lock(&mutex);
        jobQueue[in] = connfd;
        in = (in + 1) % size;
        total_requests++;
        pthread_mutex_unlock(&mutex);
        sem_post(&full_sem);
    }

    pthread_mutex_lock(&mutex);
    for (int i = 0; i < threads; i++) {
        sem_post(&full_sem);
    }

    pthread_mutex_unlock(&mutex);
    for (int i = 0; i < threads; i++) {
        pthread_join(thread_pool[i], NULL);
    }

    fclose(logfile);

    return EXIT_SUCCESS;
}
