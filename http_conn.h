#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>

class http_conn {
public:

    // All socket events will be registered in the same epoll event
    static int m_epollfd;
    // Number of users
    static int m_user_count;
    // Maximum length of request file name
    static const int FILENAME_LEN = 200;

    // Size of read and write buffer
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;


    // HTTP request method, only GET is supported here
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};

    /* The state of the main state machine when parsing the client request 
        CHECK_STATE_REQUESTLINE: Currently parsing the request line;
        CHECK_STATE_HEADER: Currently parsing the header field; 
        CHECK_STATE_CONTENT: Currently parsing the request body 
    */ 
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};
    
    /* Possible results of the server processing the HTTP request: 
        NO_REQUEST: The request is incomplete and the client data needs to continue to be read; 
        GET_REQUEST: A completed client request has been completed; 
        BAD_REQUEST: The client request syntax is incorrect; 
        NO_RESOURCE: The server has no resources; 
        FORBIDDEN_REQUEST: The client does not have sufficient access rights to the resource; 
        FILE_REQUEST: File request, file acquisition is successful; 
        INTERNAL_ERROR: An internal server error; 
        CLOSED_CONNECTION: iThe client has closed the connection 
    */
    enum HTTP_CODE {NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };

    /*
        Three possible states of the state machine (i.e., the read state of the line):
            1. Read a complete line; 
            2. Line error; 
            3. Line data is not yet complete.
    */
    enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};


    http_conn() {};
    ~http_conn() {};

    // Process client request, entry function for the worker thread in the thread pool to process http requests
    void process();
    // Initialize new accepted connection
    void init(int sockfd, const sockaddr_in & addr);
    // Close connection
    void close_conn();
    // Non-blocking read
    bool read();
    // Non-blocking write
    bool write();

private:
    // Socket for current HTTP connection
    int m_sockfd;
    // Socket address
    sockaddr_in m_address;
    // Buffer for reading
    char m_read_buf[READ_BUFFER_SIZE];
    // Next position of the last byte of client data that has been read into the read buffer
    int m_read_idx;


    // Position of the character currently being parsed in the read buffer
    int m_checked_idx;      
    // Starting position of the line currently being parsed
    int m_start_line;                       
    // Current state of the main state machine
    CHECK_STATE m_check_state;      
   

    METHOD m_method;       
    // File name of the target file requested by the client
    char* m_url; 
    // HTTP protocol version number (only support HTTP1.1)                           
    char* m_version;    
    // Host name                   
    char* m_host;   
    // Total length of the HTTP request message                    
    int m_content_length;   
    // Whether the HTTP request requires a connection to be maintained               
    bool m_linger;  
    // Full path of the target file requested by the client, which is doc_root + m_url (doc_root is the root dir)
    char m_real_file[FILENAME_LEN];   

    // Status of file
    struct stat m_file_stat;
    // File address of the requested file, which is mmapped to the memory
    char* m_file_address;      


    // Write buffer
    char m_write_buf[ WRITE_BUFFER_SIZE ]; 
    // Number of bytes to be sent in the write buffer
    int m_write_idx;  
    // Scatter/gather write, which allow both write buffer and requested resource(m_file_address) to write in a single system call
    struct iovec m_iv[2];                  
    int m_iv_count;         
    // The number of bytes to be sent   
    int bytes_to_send = 0;
    // The number of bytes have sent
    int bytes_have_send = 0;
    


    // Initialization before parsing request
    void init();
    // Parse HTTP request
    HTTP_CODE process_read(); 
    // The following set of functions are called by process_read to analyze HTTP requests
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    // Parse a specific content of a line
    LINE_STATUS parse_line(); 
    // Get the actual current of line <Parse before Get>
    char* get_line() {return m_read_buf + m_start_line;}
    HTTP_CODE do_request();
    // Release memory map
    void unmap();


    // Generate response
    bool process_write(HTTP_CODE ret);
    // The following set of functions are called by process_write to generate HTTP response
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_content_type();
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    

};

#endif