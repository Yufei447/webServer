#include "http_conn.h"
#include <strings.h>
#include <string.h>

int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

void setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    // EPOLLRDHUP: don't need to use return value to see if the other has already ended the connection; use event to determine directly
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = fd;
    
    if(one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // set fd non-blocking
    setnonblocking(fd);
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::init(int sockfd, const sockaddr_in & addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // port multiplexing
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Add to epoll
    addfd(m_epollfd, m_sockfd, true);
    m_user_count ++;


    // Initialization before parsing request
    init();
}

void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count --;
    }
}

// Read data iteratively, until there's no data or the other closes the connection
bool http_conn::read() {

    if(m_read_idx >= READ_BUFFER_SIZE) {
        printf("Read buffer doesn't have enough size for holding client's data!\n");
        return false;
    }

    // Bytes has already read
    int bytes_read = 0;
    while(true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) { // no data
                break;
            }
            return false;
        } else if(bytes_read == 0) {
            // The other side's closed the connection
            return false;
        }

        m_read_idx += bytes_read;
    }

    printf("Read data: %s\n", m_read_buf);
    return true;
}

void http_conn::init() {
    // Initial state: Request Line
    m_check_state = CHECK_STATE_REQUESTLINE; 
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_method = GET;  
    m_url = 0;     
    m_version = 0;
    // Don't keep connection in default
    m_linger = false; 

    bzero(m_read_buf, READ_BUFFER_SIZE);

    m_content_length = 0;
    m_host = 0;
    
    m_write_idx = 0;
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

    bytes_to_send = 0;
    bytes_have_send = 0;

}

// Main State Machine
http_conn::HTTP_CODE http_conn::process_read(){
    // Initial state
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char* text = 0;
    // Parse into request content and prior line is valid, or,
    // process the new line and it's valid:
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
            || ((line_status = parse_line()) == LINE_OK)) { 
            
        // 1. Get a row of data: 
        text = get_line();
        // Reset starting position of next line: m_checked_idx's already been moved to m_read_idx by parse_line()
        m_start_line = m_checked_idx;
        printf("Got a http line: %s\n", text);

        // 2. State Transition
        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                printf("Check line\n");
                ret = parse_request_line(text);
                // If request has syntax error, directly return; Otherwise, continue parsing.
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                printf("Check header\n");
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } 
                // This request doesn't have request body
                else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                printf("Check content\n");
                ret = parse_content(text);
                // Parsed done
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                // Otherwise, continue
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// Parse a specific content of a line; every line ends with \r\n
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for (;m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            // Incomplete data (\r doesn't follow with \n), and all data in buffer's been read
            if ((m_checked_idx + 1) == m_read_idx) { 
                return LINE_OPEN;
            } 

            // Complete data
            else if (m_read_buf[m_checked_idx + 1] == '\n' ) { 
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }

            // Other scenarios
            return LINE_BAD;
        }

        // Last char of last line is \r, current line starts with \n and contain next line content
        else if(temp == '\n') {
            // m_checked_idx - 1: Last char of last line
            if((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r' ) ) {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN;
    
}


http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    // GET /index.html HTTP/1.1
    // 1. Parse request method
    m_url = strpbrk(text, " \t"); // Returns a pointer to the first occurrence in str1 of any character in str2(blank space or \t).
    if (!m_url) { 
        return BAD_REQUEST;
    }

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0'; 
    char* method = text;
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    // 2. Parse protocol and version
    // /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }

    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // 3. Parse request file
    // /index.html\0HTTP/1.1  
    if (strncasecmp(m_url, "http://", 7) == 0) { // http://192.168.110.129:10000/index.html
        // 192.168.110.129:10000/index.html
        m_url += 7;     
        // Returns a pointer to the first occurrence of the character c in the string str.
        m_url = strchr(m_url, '/'); 
    }
    if (!m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }

    // 4. State transition
    m_check_state = CHECK_STATE_HEADER; 

    return NO_REQUEST;

}

http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    // When encounter a blank line, that means we could start parse request body
    if(text[0] == '\0') {
        // There's a m_content_length field if the HTTP request has a request body, which means the request body's size
        if (m_content_length != 0) {
            // State transition
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // Otherwise, it means we've already parsed the full request
        return GET_REQUEST;

    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        // Connection: keep-alive
        text += 11;
        // Skip over leading whitespace (spaces and tabs) in a string
        text += strspn(text, " \t" ); // Returns the number of characters at the start of str1 that consist only of characters found in str2.
        if (strcasecmp(text, "keep-alive") == 0 ) {
            m_linger = true;
        }

    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);

    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;

    } else {
        printf("Unknow header %s\n", text);
    }

    return NO_REQUEST;
}

// Here we only check if the request body has been read completely, instead of parsing its actual contents
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}


/*
    When we get a complete and correct HTTP request, we analyze the properties of the target file.
    Valid only if the target file exists, is readable by all users, and is not a directory.

    Then use memory map to 
*/

// Server root dir
const char* doc_root = "/home/yufei/code2025/resources";

http_conn::HTTP_CODE http_conn::do_request() {
    printf("Start to prepare file\n");
    // 1. Get Complete Path 
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    printf("File path: %s\n", m_real_file);

    // 2. Check status 
    // Get file attributes for file
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // Check access
    if (!(m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // Check if it's a dir
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 3. Memory Map
    // Open file as Read-Only
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);


    return FILE_REQUEST;
}

void http_conn::unmap() {
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}


bool http_conn::write() {
    int temp = 0;
    printf("Bytes to send, %d\n", bytes_to_send);
    // Bytes to send is 0, end response
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN); 
        init();
        return true;
    }

    while(1) {
        // Scatter write
        printf("%s", m_file_address);
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            // If there is no space in the TCP write buffer, it waits for the next round of EPOLLOUT events. 
            // Although during this period, the server cannot immediately receive the next request from the same client, the integrity of the connection can be guaranteed.
            if(errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;

        // Check if the response header has sent
        if (bytes_have_send >= m_iv[0].iov_len){ // sent 
            printf("Response header has beeen sent, start to write response body\n");
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else{ // header has not been sent yet
            
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        // Successfully send HTTP response
        if (bytes_to_send <= bytes_have_send) {
            unmap();

             // Check if close connection immediately according to Connection field of the request
            if(m_linger) {
                init();
                return true;
            } else {
                return false;
            } 
        }
    }
}


// HTTP response code
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// Write data to be sent into the write buffer
bool http_conn::add_response(const char* format, ...) {
    if(m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }

    // Handle variable-length argument lists
    va_list arg_list;
    // Initialize va_list with the last fixed parameter
    va_start(arg_list, format);
    // Format a string with a variable list of arguments and storing the result in a buffer
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        return false;
    }

    m_write_idx += len;
    // Clean up the va_list
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}


bool http_conn::process_write(HTTP_CODE ret) {
    switch(ret) {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;

        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;

        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;

        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;

        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;

        default:
            return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}



void http_conn::process() {
    // printf("Parse request, create response\n");

    // 1. Parse HTTP request
    HTTP_CODE read_ret = process_read();
    // Incomplete request, continue reading
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    

    // 2. Generate response according to the result of the parsed request
    printf("Generating response...\n");
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    // ONESHOT: add event everytime
    printf("Start to write response\n");
    modfd(m_epollfd, m_sockfd, EPOLLOUT);

}