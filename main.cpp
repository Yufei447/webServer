/*
    Use synchronous IO to simulate the Proactor:
        The main thread listens for new connections from the client or data arrival;
        When data arrives, the main thread reads the data at one time. 
        Then the main thread encapsulates the task object and notifies the child thread (thread pool) to complete the task.
            - Push the request to threadpoll;
            - Threadpool run -> get the request to process
        
        When the task takes over by a child thread, it needs to parse the request(passed by main thread); Eencapsulate resources requested, and give to main thread.
            - http_conn::process()

        Main thread writes response.
            - http_conn::write()

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include "http_conn.h"

#define MAX_FD 65536  // Maximum number of file descriptors
#define MAX_EVENT_NUMBER 10000 // Maximum number of listen events

// Capture Signal 
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// Manage file descriptor utilities
// Add fd to epoll
extern void addfd(int epollfd, int fd, bool one_shot);
// Delete fd from epoll
extern void removefd(int epollfd, int fd);
// Modify fd
extern void modfd(int epollfd, int fd, int ev);

// Run the program with port number
int main(int argc, char* argv[]) {
    
    if(argc <= 1) {
        // basename: extracts the base name of the path of program
        printf("Please use the following command to run the program: %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 1. Get port number
    int port = atoi(argv[1]);

    // 2. If one ends the connection while the other still tries to write data in network programming, a SIGPIPE error will occur. Thus, SIGPIPE must be processed.
    addsig(SIGPIPE, SIG_IGN);

    // 3. Create and initiate thread pool
    // Task: When a client connects, the client may send HTTP request
    threadpool<http_conn>* pool = nullptr;
    try {
        pool = new threadpool<http_conn>;

    } catch(...) { // catch any exception thrown in a try block
        exit(-1);
    }

    // 4. Save all clients' info
    http_conn* users = new http_conn[MAX_FD];

    // 5. Socket programming
    // 5.1 Socket creation
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    
    if(listenfd == -1) {
        perror("socket");
        return -1;
    }

    // 5.2 port multiplexing
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 5.3 bind ip and port
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    int ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    if(ret == -1) {
        perror("bind");
        return -1;
    }

    // 5.4 listen
    ret = listen(listenfd, 5);
    if(ret == -1) {
        perror("listen");
        return -1;
    }

    // 5.5.1 IO multiplexing - epoll
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    // 5.5.2 Add the monitoring file descriptor to the epoll instance
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 5.5.3 Detect events
    while(true) {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((num < 0) && (errno != EINTR)) {
            printf("epoll failed\n");
            break;
        }

        // 5.5.4 Process events events
        for(int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;
            
            if(sockfd == listenfd) { // Client connection
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                
                if (connfd < 0) {
                    printf("errno is: %d\n", errno);
                    continue;
                } 

                if(http_conn::m_user_count >= MAX_FD) {
                    // The current number of connections is full, write a message to the client: the server is busy
                    close(connfd);
                    continue;
                }
                
                // Initialize new clients' data 
                users[connfd].init(connfd, client_address);

            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) { // The other party is abnormally disconnected or has errors, etc.
                // close connection
                users[sockfd].close_conn();

            } else if(events[i].events & EPOLLIN) { // Read event
                // Read all data at one time
                if(users[sockfd].read()) {
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();
                }
            } else if(events[i].events & EPOLLOUT) { // Write event
                // Write all data at one time
                if(!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }

            }

        }
    }

    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;
    return 0;
}