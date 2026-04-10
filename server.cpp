#include <arpa/inet.h>     // IP address and port helpers
#include <fcntl.h>         // File/socket flags like non-blocking mode
#include <sys/epoll.h>     // epoll event loop functions
#include <sys/sendfile.h>  // sendfile() for sending files efficiently
#include <sys/socket.h>    // socket functions like send, recv, bind, accept
#include <sys/stat.h>      // file info like size and regular file checks
#include <unistd.h>        // close() and other Unix system calls

#include <cerrno>         // error codes like EAGAIN
#include <chrono>         // time utilities
#include <cstring>        // C string helpers
#include <iostream>       // printing to console
#include <sstream>        // string stream parsing/building
#include <string>         // std::string
#include <unordered_map>  // hash maps
#include <vector>         // dynamic arrays

const int PORT = 8080;                     // main server port
const int MAX_EVENTS = 10;                 // max epoll events handled at once
const int BUFFER_SIZE = 4096;              // 4 KB temp read buffer
const int BACKEND_COUNT = 2;               // number of backend servers
const int BACKEND_RETRY_MS = 5000;         // retry failed backend after 5 sec
const int BACKEND_PORTS[] = {9001, 9002};  // backend server ports

// Stores info about one backend server.
// Holds its port, health status, and retry timing.
struct Backend {
    int port;                      // backend port number
    bool is_healthy = true;        // whether backend is usable
    long long retry_after_ms = 0;  // when we can retry if unhealthy
};

// Stores one parsed HTTP request.
// Holds method, path, headers, and body.
struct HttpRequest {
    std::string method;   // GET, POST, etc.
    std::string path;     // request path (/index.html)
    std::string version;  // HTTP version (HTTP/1.1)
    std::unordered_map<std::string, std::string> headers;  // request headers
    std::string body;  // request body (for POST)
};

// Stores all state for one client connection.
// Tracks buffers, file sending, and proxy state.
struct Connection {
    int fd;  // client socket fd

    std::string read_buffer;    // incoming request data
    std::string write_buffer;   // outgoing response data
    size_t bytes_sent = 0;      // how much we already sent
    bool should_close = false;  // close after response?

    int file_fd = -1;           // file being sent
    off_t file_offset = 0;      // current file position
    off_t file_size = 0;        // total file size
    bool sending_file = false;  // are we sending a file?

    bool is_proxy = false;           // is this a proxy request?
    int backend_fd = -1;             // backend socket fd
    int backend_index = -1;          // which backend we picked
    bool backend_connected = false;  // did connect finish?

    std::string backend_write_buffer;  // request to backend
    size_t backend_bytes_sent = 0;     // bytes sent to backend
};

// Stores the main shared server state.
// Tracks all connections, epoll, and backends.
struct ServerState {
    int server_fd = -1;          // listening socket
    int epoll_fd = -1;           // epoll instance
    int next_backend_index = 0;  // round-robin pointer

    std::unordered_map<int, Connection> connections;  // client fd -> connection
    std::unordered_map<int, int> backend_to_client;   // backend fd -> client fd
    std::vector<Backend> backends;                    // list of backends
};

// Checks if this request should go to a backend server.
// Input: request. Output: true if path starts with /api/.
bool should_proxy_request(const HttpRequest& request) {
    return request.path.rfind("/api/", 0) == 0;
}

// Makes a file descriptor non-blocking.
// Input: fd. Output: true if successful.
bool set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        return false;
    }

    return true;
}

// Finds where the HTTP headers end.
// Input: raw request. Output: position of "\r\n\r\n".
size_t find_header_end(const std::string& raw_request) {
    return raw_request.find("\r\n\r\n");
}

// Reads Content-Length from the headers.
// Input: header text. Output: fills content_length.
bool parse_content_length_from_headers(const std::string& header_section,
                                       size_t& content_length) {
    std::istringstream stream(header_section);
    std::string line;
    content_length = 0;

    std::getline(stream, line);

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);
        if (!value.empty() && value[0] == ' ') {
            value.erase(0, 1);
        }

        if (key == "Content-Length") {
            try {
                content_length = std::stoul(value);
            } catch (...) {
                return false;
            }
        }
    }

    return true;
}

// Checks if the full HTTP request has arrived yet.
// Input: raw request text. Output: true if complete.
bool is_full_http_request(const std::string& raw_request) {
    size_t header_end = find_header_end(raw_request);
    if (header_end == std::string::npos) {
        return false;
    }

    size_t body_start = header_end + 4;
    size_t content_length = 0;

    std::string header_section = raw_request.substr(0, header_end);
    if (!parse_content_length_from_headers(header_section, content_length)) {
        return false;
    }

    return raw_request.size() >= body_start + content_length;
}

// Gets total length of one full HTTP request.
// Input: raw request text. Output: full request size.
size_t get_full_request_length(const std::string& raw_request) {
    size_t header_end = find_header_end(raw_request);
    if (header_end == std::string::npos) {
        return 0;
    }

    size_t body_start = header_end + 4;
    size_t content_length = 0;

    std::string header_section = raw_request.substr(0, header_end);
    if (!parse_content_length_from_headers(header_section, content_length)) {
        return 0;
    }

    return body_start + content_length;
}

// Parses raw HTTP text into a request object.
// Input: raw request. Output: fills request fields.
bool parse_http_request(const std::string& raw_request, HttpRequest& request) {
    request = HttpRequest{};

    std::istringstream stream(raw_request);
    std::string line;

    if (!std::getline(stream, line)) {
        return false;
    }

    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream request_line(line);
    if (!(request_line >> request.method >> request.path >> request.version)) {
        return false;
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            break;
        }

        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);
        if (!value.empty() && value[0] == ' ') {
            value.erase(0, 1);
        }

        request.headers[key] = value;
    }

    size_t header_end = find_header_end(raw_request);
    if (header_end != std::string::npos) {
        request.body = raw_request.substr(header_end + 4);
    }

    return true;
}

// Decides if the client connection stays open.
// Input: request. Output: true if keep-alive.
bool should_keep_alive(const HttpRequest& request) {
    auto it = request.headers.find("Connection");

    if (request.version == "HTTP/1.1") {
        if (it != request.headers.end() && it->second == "close") {
            return false;
        }
        return true;
    }

    if (request.version == "HTTP/1.0") {
        if (it != request.headers.end() && it->second == "keep-alive") {
            return true;
        }
        return false;
    }

    return false;
}

// Builds only the HTTP headers string.
// Input: status/type/length. Output: header text.
std::string build_http_headers(const std::string& status,
                               const std::string& content_type,
                               size_t content_length, bool keep_alive) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << content_length << "\r\n";
    response << "Connection: " << (keep_alive ? "keep-alive" : "close")
             << "\r\n";
    response << "\r\n";
    return response.str();
}

// Builds a full HTTP response.
// Input: status/type/body. Output: full response string.
std::string build_http_response(const std::string& status,
                                const std::string& content_type,
                                const std::string& body, bool keep_alive) {
    return build_http_headers(status, content_type, body.size(), keep_alive) +
           body;
}

// Picks the correct content type from the file name.
// Input: file path. Output: MIME type string.
std::string get_content_type(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") {
        return "text/html";
    } else if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") {
        return "text/css";
    } else if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") {
        return "application/javascript";
    } else if (path.size() >= 4 && path.substr(path.size() - 4) == ".png") {
        return "image/png";
    } else if (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg") {
        return "image/jpeg";
    } else if (path.size() >= 5 && path.substr(path.size() - 5) == ".jpeg") {
        return "image/jpeg";
    } else if (path.size() >= 4 && path.substr(path.size() - 4) == ".txt") {
        return "text/plain";
    }

    return "application/octet-stream";
}

// Checks if a file path is safe to use.
// Input: path. Output: false if it contains ..
bool is_safe_path(const std::string& path) {
    return path.find("..") == std::string::npos;
}

// Gets current time in milliseconds.
// Input: none. Output: current steady-clock ms.
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Clears backend/proxy fields in a connection.
// Input: connection. Output: backend state reset.
void reset_backend_state(Connection& conn) {
    conn.backend_fd = -1;
    conn.backend_index = -1;
    conn.is_proxy = false;
    conn.backend_connected = false;
    conn.backend_write_buffer.clear();
    conn.backend_bytes_sent = 0;
}

// Switches a client socket to EPOLLOUT mode.
// Input: state and client fd. Output: true if successful.
bool switch_client_to_epollout(ServerState& state, int client_fd) {
    epoll_event client_ev{};
    client_ev.events = EPOLLOUT;
    client_ev.data.fd = client_fd;
    return epoll_ctl(state.epoll_fd, EPOLL_CTL_MOD, client_fd, &client_ev) !=
           -1;
}

// Switches a client socket back to EPOLLIN mode.
// Input: state and client fd. Output: true if successful.
bool switch_client_to_epollin(ServerState& state, int client_fd) {
    epoll_event client_ev{};
    client_ev.events = EPOLLIN;
    client_ev.data.fd = client_fd;
    return epoll_ctl(state.epoll_fd, EPOLL_CTL_MOD, client_fd, &client_ev) !=
           -1;
}

// Closes a client and cleans up its resources.
// Input: state and client fd. Output: removes connection.
void close_connection(ServerState& state, int client_fd) {
    auto it = state.connections.find(client_fd);
    if (it != state.connections.end()) {
        if (it->second.file_fd != -1) {
            close(it->second.file_fd);
            it->second.file_fd = -1;
        }

        if (it->second.backend_fd != -1) {
            epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, it->second.backend_fd,
                      nullptr);
            close(it->second.backend_fd);
            state.backend_to_client.erase(it->second.backend_fd);
            it->second.backend_fd = -1;
        }

        state.connections.erase(it);
    }

    epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
}

// Starts a connection to one backend server.
// Input: backend port. Output: backend fd or -1.
int connect_to_backend(int backend_port) {
    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd == -1) {
        perror("socket backend");
        return -1;
    }

    if (!set_non_blocking(backend_fd)) {
        close(backend_fd);
        return -1;
    }

    sockaddr_in backend_addr{};
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(backend_port);

    if (inet_pton(AF_INET, "127.0.0.1", &backend_addr.sin_addr) != 1) {
        close(backend_fd);
        return -1;
    }

    int result =
        connect(backend_fd, (sockaddr*)&backend_addr, sizeof(backend_addr));

    if (result == 0) {
        return backend_fd;
    }

    if (result == -1 && errno == EINPROGRESS) {
        return backend_fd;
    }

    perror("connect backend");
    close(backend_fd);
    return -1;
}
// Handles backend failure and prepares a 502 response.
// Input: state, backend fd, client fd. Output: cleans up backend and queues
// error response.
void handle_backend_failure(ServerState& state, int backend_fd, int client_fd) {
    auto conn_it = state.connections.find(client_fd);
    if (conn_it == state.connections.end()) {
        epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, backend_fd, nullptr);
        close(backend_fd);
        state.backend_to_client.erase(backend_fd);
        return;
    }

    Connection& conn = conn_it->second;

    if (conn.backend_index != -1) {
        state.backends[conn.backend_index].is_healthy = false;
        state.backends[conn.backend_index].retry_after_ms =
            now_ms() + BACKEND_RETRY_MS;

        std::cout << "Marked backend port "
                  << state.backends[conn.backend_index].port
                  << " unhealthy until retry window\n";
    }

    std::cout << "Backend failure: client fd=" << client_fd
              << ", backend fd=" << backend_fd << " -> returning 502"
              << std::endl;

    std::string body = "<h1>502 Bad Gateway</h1>";
    conn.write_buffer += build_http_response("502 Bad Gateway", "text/html",
                                             body, !conn.should_close);

    epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, backend_fd, nullptr);
    close(backend_fd);
    state.backend_to_client.erase(backend_fd);

    reset_backend_state(conn);

    if (!switch_client_to_epollout(state, client_fd)) {
        close_connection(state, client_fd);
    }
}

// Removes /api from the front of the path for backend routing.
// Input: client path. Output: backend path.
std::string get_backend_path(const std::string& path) {
    std::string backend_path = path;
    if (backend_path.rfind("/api", 0) == 0) {
        backend_path.erase(0, 4);
        if (backend_path.empty()) {
            backend_path = "/";
        }
    }
    return backend_path;
}

// Builds the raw HTTP request to send to the backend.
// Input: parsed request. Output: full request string.
std::string build_proxy_request(const HttpRequest& request) {
    std::ostringstream out;
    std::string backend_path = get_backend_path(request.path);

    out << request.method << " " << backend_path << " " << request.version
        << "\r\n";

    for (const auto& [key, value] : request.headers) {
        out << key << ": " << value << "\r\n";
    }

    out << "\r\n";
    out << request.body;
    return out.str();
}

// Tries to connect to a healthy backend using round-robin.
// Input: backend list and next index. Output: backend fd or -1.
int try_connect_to_any_backend(std::vector<Backend>& backends,
                               int& next_backend_index, int& chosen_port,
                               int& chosen_index) {
    long long current_time = now_ms();

    for (int attempt = 0; attempt < BACKEND_COUNT; attempt++) {
        int index = (next_backend_index + attempt) % BACKEND_COUNT;
        Backend& backend = backends[index];

        if (!backend.is_healthy && current_time < backend.retry_after_ms) {
            continue;
        }

        int backend_fd = connect_to_backend(backend.port);
        if (backend_fd != -1) {
            backend.is_healthy = true;
            backend.retry_after_ms = 0;
            chosen_port = backend.port;
            chosen_index = index;
            next_backend_index = (index + 1) % BACKEND_COUNT;
            return backend_fd;
        }

        backend.is_healthy = false;
        backend.retry_after_ms = current_time + BACKEND_RETRY_MS;
    }

    chosen_port = -1;
    chosen_index = -1;
    return -1;
}

// Creates the main listening socket for the server.
// Input: none. Output: server fd or -1.
int create_listening_socket() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) ==
        -1) {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }

    if (!set_non_blocking(server_fd)) {
        close(server_fd);
        return -1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

// Creates epoll and adds the server socket to it.
// Input: server fd. Output: epoll fd or -1.
int create_epoll_instance(int server_fd) {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        return -1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl: server_fd");
        close(epoll_fd);
        return -1;
    }

    return epoll_fd;
}

// Accepts all waiting client connections.
// Input: server state. Output: adds new clients to epoll.
void accept_new_clients(ServerState& state) {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd =
            accept(state.server_fd, (sockaddr*)&client_addr, &client_len);

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("accept");
            break;
        }

        if (!set_non_blocking(client_fd)) {
            close(client_fd);
            continue;
        }

        state.connections[client_fd] = Connection{client_fd};

        std::cout << "New client connected: fd=" << client_fd
                  << ", IP=" << inet_ntoa(client_addr.sin_addr)
                  << ", port=" << ntohs(client_addr.sin_port) << std::endl;

        epoll_event client_ev{};
        client_ev.events = EPOLLIN;
        client_ev.data.fd = client_fd;

        if (epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) ==
            -1) {
            perror("epoll_ctl: client_fd");
            close(client_fd);
            state.connections.erase(client_fd);
        }
    }
}

// Finishes one response and resets or closes the client.
// Input: state and client fd. Output: client ready for next request or closed.
void finish_response_and_reset_client(ServerState& state, int client_fd) {
    Connection& conn = state.connections[client_fd];

    if (conn.should_close) {
        close_connection(state, client_fd);
        std::cout << "Closed client fd=" << client_fd
                  << " after full response sent\n";
        return;
    }

    conn.write_buffer.clear();
    conn.bytes_sent = 0;
    conn.should_close = false;

    if (!switch_client_to_epollin(state, client_fd)) {
        perror("epoll_ctl: mod client_fd back to EPOLLIN");
        close_connection(state, client_fd);
        return;
    }

    std::cout << "Kept client fd=" << client_fd << " alive for next request\n";
}

// Sends queued response data to the client.
// Input: state and client fd. Output: sends buffer/file and finishes response.
void handle_client_writable(ServerState& state, int client_fd) {
    auto it = state.connections.find(client_fd);
    if (it == state.connections.end()) {
        return;
    }

    Connection& conn = it->second;

    while (conn.bytes_sent < conn.write_buffer.size()) {
        ssize_t n = send(client_fd, conn.write_buffer.c_str() + conn.bytes_sent,
                         conn.write_buffer.size() - conn.bytes_sent, 0);

        if (n > 0) {
            conn.bytes_sent += n;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            perror("send");
            close_connection(state, client_fd);
            return;
        }
    }

    if (conn.bytes_sent < conn.write_buffer.size()) {
        return;
    }

    if (conn.sending_file) {
        while (conn.file_offset < conn.file_size) {
            ssize_t n = sendfile(client_fd, conn.file_fd, &conn.file_offset,
                                 conn.file_size - conn.file_offset);

            if (n > 0) {
            } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                perror("sendfile");
                close_connection(state, client_fd);
                return;
            }
        }

        if (conn.file_offset < conn.file_size) {
            return;
        }

        close(conn.file_fd);
        conn.file_fd = -1;
        conn.file_offset = 0;
        conn.file_size = 0;
        conn.sending_file = false;
    }

    finish_response_and_reset_client(state, client_fd);
}

// Starts a proxy request to one backend.
// Input: client request data. Output: starts backend flow or builds 502
// response.
void start_proxy_request(ServerState& state, Connection& conn, int client_fd,
                         const HttpRequest& request, bool keep_alive,
                         std::string& response_out) {
    int backend_port = -1;
    int backend_index = -1;
    int backend_fd = try_connect_to_any_backend(
        state.backends, state.next_backend_index, backend_port, backend_index);

    if (backend_fd == -1) {
        std::string body = "<h1>502 Bad Gateway</h1>";
        response_out = build_http_response("502 Bad Gateway", "text/html", body,
                                           keep_alive);
        return;
    }

    std::string backend_path = get_backend_path(request.path);
    std::cout << "Proxy route for client fd=" << client_fd << ": "
              << request.path << " -> " << backend_path << " on backend port "
              << backend_port << std::endl;

    conn.is_proxy = true;
    conn.backend_fd = backend_fd;
    conn.backend_index = backend_index;
    conn.backend_connected = false;
    conn.backend_write_buffer = build_proxy_request(request);
    conn.backend_bytes_sent = 0;

    state.backend_to_client[backend_fd] = client_fd;

    epoll_event backend_ev{};
    backend_ev.events = EPOLLOUT | EPOLLIN;
    backend_ev.data.fd = backend_fd;

    if (epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, backend_fd, &backend_ev) ==
        -1) {
        perror("epoll_ctl: backend_fd");
        close(backend_fd);
        state.backend_to_client.erase(backend_fd);
        reset_backend_state(conn);

        std::string body = "<h1>502 Bad Gateway</h1>";
        response_out = build_http_response("502 Bad Gateway", "text/html", body,
                                           keep_alive);
    }
}

// Prepares a static file response.
// Input: request path and connection. Output: response headers or error
// response.
void prepare_static_response(Connection& conn, int client_fd,
                             const HttpRequest& request, bool keep_alive,
                             std::string& response_out) {
    std::string file_path = request.path;

    if (!is_safe_path(file_path)) {
        std::cout << "Static 403 for client fd=" << client_fd << ": "
                  << request.path << std::endl;
        response_out = build_http_response(
            "403 Forbidden", "text/html", "<h1>403 Forbidden</h1>", keep_alive);
        return;
    }

    if (file_path == "/") {
        file_path = "/index.html";
    }

    std::string full_path = "./www" + file_path;
    std::cout << "Static route for client fd=" << client_fd << ": "
              << request.path << " -> " << full_path << std::endl;

    int static_fd = open(full_path.c_str(), O_RDONLY);
    if (static_fd == -1) {
        std::cout << "Static 404 for client fd=" << client_fd << ": "
                  << full_path << std::endl;
        response_out = build_http_response(
            "404 Not Found", "text/html", "<h1>404 Not Found</h1>", keep_alive);
        return;
    }

    struct stat st{};
    if (fstat(static_fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        close(static_fd);
        response_out = build_http_response(
            "404 Not Found", "text/html", "<h1>404 Not Found</h1>", keep_alive);
        return;
    }

    std::string content_type = get_content_type(full_path);
    response_out =
        build_http_headers("200 OK", content_type, st.st_size, keep_alive);

    conn.file_fd = static_fd;
    conn.file_offset = 0;
    conn.file_size = st.st_size;
    conn.sending_file = true;
}

// Handles one parsed request and chooses the response path.
// Input: request and parse result. Output: queues response into write buffer.
void process_one_request(ServerState& state, Connection& conn, int client_fd,
                         const HttpRequest& request, bool parsed_ok) {
    std::string response;

    if (!parsed_ok) {
        conn.should_close = true;
        response = build_http_response("400 Bad Request", "text/html",
                                       "<h1>400 Bad Request</h1>", false);
        conn.write_buffer += response;
        return;
    }

    bool keep_alive = should_keep_alive(request);
    conn.should_close = !keep_alive;

    std::cout << "Request on client fd=" << client_fd << ": " << request.method
              << " " << request.path
              << " keep_alive=" << (keep_alive ? "true" : "false") << std::endl;

    if (should_proxy_request(request)) {
        start_proxy_request(state, conn, client_fd, request, keep_alive,
                            response);
    } else if (request.method == "POST" && request.path == "/echo") {
        std::string body = "<html><body><h1>POST body:</h1><pre>" +
                           request.body + "</pre></body></html>";
        response = build_http_response("200 OK", "text/html", body, keep_alive);
    } else if (request.method != "GET") {
        response =
            build_http_response("405 Method Not Allowed", "text/html",
                                "<h1>405 Method Not Allowed</h1>", keep_alive);
    } else {
        prepare_static_response(conn, client_fd, request, keep_alive, response);
    }

    conn.write_buffer += response;
}

// Reads incoming client data and processes full requests.
// Input: state and client fd. Output: fills buffers and queues response.
void handle_client_readable(ServerState& state, int client_fd) {
    auto it = state.connections.find(client_fd);
    if (it == state.connections.end()) {
        return;
    }

    Connection& conn = it->second;
    char buffer[BUFFER_SIZE];

    while (true) {
        ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer), 0);

        if (bytes_received > 0) {
            conn.read_buffer.append(buffer, bytes_received);
        } else if (bytes_received == 0) {
            std::cout << "Client disconnected: fd=" << client_fd << std::endl;
            close_connection(state, client_fd);
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            perror("recv");
            close_connection(state, client_fd);
            return;
        }
    }

    while (is_full_http_request(conn.read_buffer)) {
        size_t request_len = get_full_request_length(conn.read_buffer);
        if (request_len == 0) {
            conn.should_close = true;
            conn.write_buffer +=
                build_http_response("400 Bad Request", "text/html",
                                    "<h1>400 Bad Request</h1>", false);
            break;
        }

        std::string raw_request = conn.read_buffer.substr(0, request_len);
        HttpRequest request;
        bool ok = parse_http_request(raw_request, request);

        process_one_request(state, conn, client_fd, request, ok);

        conn.read_buffer.erase(0, request_len);

        if (conn.sending_file || conn.is_proxy) {
            break;
        }
    }

    if (!conn.write_buffer.empty()) {
        conn.bytes_sent = 0;
        if (!switch_client_to_epollout(state, client_fd)) {
            perror("epoll_ctl: mod client_fd");
            close_connection(state, client_fd);
        }
    }
}

// Handles backend socket events for proxying.
// Input: backend fd and epoll events. Output: sends to backend and relays
// response back.
void handle_backend_event(ServerState& state, int backend_fd, uint32_t events) {
    auto map_it = state.backend_to_client.find(backend_fd);
    if (map_it == state.backend_to_client.end()) {
        return;
    }

    int client_fd = map_it->second;
    auto conn_it = state.connections.find(client_fd);
    if (conn_it == state.connections.end()) {
        epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, backend_fd, nullptr);
        close(backend_fd);
        state.backend_to_client.erase(backend_fd);
        return;
    }

    Connection& conn = conn_it->second;

    if (!conn.backend_connected && (events & EPOLLOUT)) {
        int so_error = 0;
        socklen_t len = sizeof(so_error);

        if (getsockopt(backend_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) ==
                -1 ||
            so_error != 0) {
            handle_backend_failure(state, backend_fd, client_fd);
            return;
        }

        conn.backend_connected = true;
        std::cout << "Backend connect complete: client fd=" << client_fd
                  << ", backend fd=" << backend_fd << std::endl;
    }

    if (conn.backend_connected && (events & EPOLLOUT)) {
        while (conn.backend_bytes_sent < conn.backend_write_buffer.size()) {
            ssize_t n = send(
                backend_fd,
                conn.backend_write_buffer.c_str() + conn.backend_bytes_sent,
                conn.backend_write_buffer.size() - conn.backend_bytes_sent, 0);

            if (n > 0) {
                conn.backend_bytes_sent += n;
            } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                handle_backend_failure(state, backend_fd, client_fd);
                return;
            }
        }
    }

    if (events & EPOLLIN) {
        char buffer[BUFFER_SIZE];

        while (true) {
            ssize_t n = recv(backend_fd, buffer, sizeof(buffer), 0);

            if (n > 0) {
                conn.write_buffer.append(buffer, n);
            } else if (n == 0) {
                std::cout << "Backend response complete: client fd="
                          << client_fd << ", backend fd=" << backend_fd
                          << std::endl;

                epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, backend_fd, nullptr);
                close(backend_fd);
                state.backend_to_client.erase(backend_fd);
                reset_backend_state(conn);

                if (!switch_client_to_epollout(state, client_fd)) {
                    close_connection(state, client_fd);
                }
                return;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                handle_backend_failure(state, backend_fd, client_fd);
                return;
            }
        }
    }
}

int main() {
    ServerState state;
    state.backends = {
        {BACKEND_PORTS[0], true, 0},
        {BACKEND_PORTS[1], true, 0},
    };

    state.server_fd = create_listening_socket();
    if (state.server_fd == -1) {
        return 1;
    }

    state.epoll_fd = create_epoll_instance(state.server_fd);
    if (state.epoll_fd == -1) {
        close(state.server_fd);
        return 1;
    }

    std::cout << "HTTP server listening on port " << PORT << std::endl;

    epoll_event events[MAX_EVENTS];

    while (true) {
        int num_events = epoll_wait(state.epoll_fd, events, MAX_EVENTS, -1);
        if (num_events == -1) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < num_events; i++) {
            int ready_fd = events[i].data.fd;
            uint32_t ready_events = events[i].events;

            // The event loop is now just a dispatcher:
            // 1) backend work
            // 2) accept new clients
            // 3) continue writing to a client
            // 4) continue reading from a client
            if (state.backend_to_client.find(ready_fd) !=
                state.backend_to_client.end()) {
                handle_backend_event(state, ready_fd, ready_events);
            } else if (ready_fd == state.server_fd) {
                accept_new_clients(state);
            } else if (ready_events & EPOLLOUT) {
                handle_client_writable(state, ready_fd);
            } else if (ready_events & EPOLLIN) {
                handle_client_readable(state, ready_fd);
            } else {
                std::cout << "Unhandled event on fd=" << ready_fd << std::endl;
            }
        }
    }

    close(state.epoll_fd);
    close(state.server_fd);
    return 0;
}
