# miniNginx (C++ epoll Reverse Proxy)

A lightweight HTTP server and reverse proxy with load balancer built from scratch in C++ using Linux sockets and `epoll`.

This project was designed to learn systems programming concepts including:

* Non-blocking I/O
* Event-driven architecture (`epoll`)
* HTTP parsing
* Static file serving
* Reverse proxying
* Basic load balancing + health detection

---

# 🚀 Features

* Event-driven server using `epoll`
* Handles multiple clients concurrently (no threads)
* HTTP/1.1 request parsing (including `Content-Length`)
* Keep-alive connections
* Static file serving (`sendfile` for efficiency)
* Reverse proxy for `/api/*`
* Round-robin load balancing
* Passive backend health detection + retry
* Basic routing and error handling (400, 403, 404, 405, 502)

---

# 📁 Project Structure

```
miniNginx/
│
├── server.cpp        # Main server implementation
│
├── www/              # Static files served directly
│   ├── index.html
│   └── style.css
│
├── backend/          # Backend server 1 (port 9001)
│   └── index.html
│
├── backend2/         # Backend server 2 (port 9002)
│   └── index.html
```

---

# 🧠 High-Level Architecture

The server is built around an **event loop** using `epoll`.

## Core Idea

```
while true:
    events = epoll_wait() 
    (wait until the OS tells us which file descriptors are ready)

    for each event:
        if backend socket:
            (this fd is a socket connected to one of our backend servers,
             used when proxying /api requests)
            handle backend
            (finish connect, send request to backend, or read response from backend)

        elif server socket:
            (this is the main listening socket created with socket/bind/listen,
             it becomes ready when new clients are trying to connect)
            accept client
            (call accept() to create a new client socket and add it to epoll)

        elif client writable:
            (this client socket is ready for sending data without blocking,
             meaning the kernel send buffer has space)
            send response
            (send HTTP headers/body or stream file with sendfile)

        elif client readable:
            (this client socket has incoming data available to read,
             meaning the client sent us a request)
            read + process request
            (recv() bytes → parse HTTP → decide static vs proxy → queue response)
```

---

# 🔄 Request Flow Examples

## 1. Static File Request

```
GET / HTTP/1.1
```

Flow:

1. Client connects → accepted
2. Request is read into buffer
3. Parsed as GET `/`
4. Mapped to `./www/index.html`
5. Headers queued
6. File streamed via `sendfile`
7. Connection kept alive (if applicable)

---

## 2. Proxy Request (`/api/*`)

```
GET /api/hello HTTP/1.1
```

Flow:

1. Request parsed
2. `/api` prefix detected → proxy path
3. Backend selected via round-robin
4. Non-blocking connection started
5. Request forwarded to backend
6. Backend response read
7. Response streamed back to client

---

## 3. POST Echo

```
POST /echo
```

Flow:

1. Request body read
2. Server builds HTML response with body
3. Response sent back

---

## 4. Error Handling

| Scenario           | Response |
| ------------------ | -------- |
| Bad request        | 400      |
| Invalid method     | 405      |
| Missing file       | 404      |
| Unsafe path (`..`) | 403      |
| Backend failure    | 502      |

---

# ⚖️ Load Balancing

Backends are selected using **round-robin**:

```
Request 1 → backend1
Request 2 → backend2
Request 3 → backend1
...
```

---

# ❤️ Backend Health Detection

## How it works (Passive)

* If a backend fails during:

  * connect
  * send
  * recv
* It is marked **unhealthy**
* It is skipped for a cooldown period (~5 seconds)
* After cooldown, it is retried

## Important

There are **no background health checks**.

Health is only determined during real requests.

---

# 🛠️ How to Build & Run

## 1. Compile

```
g++ server.cpp -o server
```

## 2. Run Server

```
./server
```

---

# 🧪 Testing Guide

## Start Backends

### Backend 1

```
cd backend
python3 -m http.server 9001
```

### Backend 2

```
cd backend2
python3 -m http.server 9002
```

---

## Test Static Files

```
curl -i http://localhost:8080/
curl -i http://localhost:8080/style.css
```

---

## Test Errors

```
curl -i http://localhost:8080/missing.html
curl -i --path-as-is http://localhost:8080/../../etc/passwd
```

---

## Test POST Echo

```
curl -i -X POST http://localhost:8080/echo -d "hello world"
```

---

## Test Proxy + Load Balancing

```
curl http://localhost:8080/api/
curl http://localhost:8080/api/
curl http://localhost:8080/api/
```

Expected:

* Alternating responses from backend1 and backend2

---

## 🔥 Test Backend Failure (IMPORTANT)

### Step 1: Kill one backend

Stop backend2 (Ctrl+C)

### Step 2: Send requests

```
curl -i http://localhost:8080/api/
curl -i http://localhost:8080/api/
curl -i http://localhost:8080/api/
```

Expected:

* One request may return `502`
* After that, only backend1 is used

### Step 3: Wait ~5 seconds

### Step 4: Restart backend2

```
python3 -m http.server 9002
```

### Step 5: Send requests again

```
curl http://localhost:8080/api/
curl http://localhost:8080/api/
curl http://localhost:8080/api/
```

Expected:

* backend2 starts receiving traffic again

---

# 🧩 Key Concepts Demonstrated

* Non-blocking sockets
* Edge-triggered event loops (epoll-style thinking)
* Connection state machines
* Backpressure handling (`EAGAIN`)
* Zero-copy file transfer (`sendfile`)
* Reverse proxy architecture
* Load balancing + failover

---

# 🎯 Future Improvements (Optional)

* Config file (nginx-style)
* Thread pool for CPU work
* Active health checks
* HTTP response parsing from backend
* Caching layer
* TLS support
* Metrics / logging system

---

# 💡 Summary

This project mimics a simplified version of nginx:

* Event-driven core
* Stateless request handling
* Separation of client and backend flows