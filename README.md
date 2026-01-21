# UDP RPC Client-Server System

A reliable request-reply system over UDP implementing RPC semantics with at-most-once delivery guarantees.

## Features

- Service registration and discovery
- Duplicate request detection (at-most-once semantics)
- Client heartbeat mechanism for failure detection
- Request cancellation support
- Crash recovery with persistent request logging
- Round-robin load balancing (client-side)

## Architecture

```
+------------------+                    +------------------+
|     CLIENT       |                    |      SERVER      |
|                  |                    |                  |
|  client_app.py   |                    |  server_app.c    |
|       |          |                    |       |          |
|  client_api.py   | <--- UDP/IP --->   |  server_api.c    |
+------------------+                    +------------------+
```

## Project Structure

```
.
├── server_api.c      # Server-side RPC runtime (C)
├── server_app.c      # Server application - primality service
├── server.h          # Server header definitions
├── client_api.py     # Client-side RPC runtime (Python)
├── client_app.py     # Batch client - sends multiple requests
├── client_app2.py    # Interactive client - manual input
├── config.txt        # Server address configuration
└── Makefile          # Build configuration
```

## Server API

```c
void init();                                    // Initialize server runtime
int register_service(int svcid);               // Register a service
int unregister_service(int svcid);             // Unregister a service
int getRequest(int svcid, void *buf, int *len); // Block until request arrives
void sendReply(int reqid, void *buf, int len);  // Send reply to client
```

## Client API

```python
init(configfname)
# Initialize client with server configuration file

doRequestReply(svcid, reqbuf, reqlen, rspbuf, rsplen)
# Execute request-reply cycle
# Returns 0 on success, -1 on failure
```

## Building

Compile the server:

```bash
make clean && make
```

## Configuration

Edit `config.txt` with server IP and port:

```
192.168.1.100 10000
```

Multiple servers can be listed for load balancing:

```
192.168.1.100 10000
192.168.1.101 10000
```

## Running

Start the server:

```bash
./server
```

Run the batch client (sends predefined numbers):

```bash
python3 client_app.py
```

Run the interactive client (manual input):

```bash
python3 client_app2.py
```

## Protocol Specification

### Request Packet Format

| Field | Size | Description |
|-------|------|-------------|
| reqid | 4 bytes | Request identifier (network byte order) |
| svcid | 4 bytes | Service identifier (network byte order) |
| payload_len | 4 bytes | Payload length (network byte order) |
| payload | variable | Request data |

### Response Packet Format

| Field | Size | Description |
|-------|------|-------------|
| reqid | 4 bytes | Request identifier (network byte order) |
| response_len | 4 bytes | Response length (network byte order) |
| response | variable | Response data |

### Special Values

- Negative reqid: Heartbeat message (server checking if client is alive)
- svcid = -1: Cancel request

## Reliability Mechanisms

### Duplicate Detection

The server maintains a circular buffer of processed request IDs. When a request arrives, the server checks if the reqid has already been processed. Duplicates are silently ignored to ensure at-most-once semantics.

### Heartbeat Protocol

While a request is being processed, the server periodically sends heartbeat messages to the client. The client must acknowledge these heartbeats. If no acknowledgment is received, the server assumes the client has crashed and cleans up associated resources.

### Client Retry

On timeout (5 seconds), the client retries the request up to 3 times. If no response or heartbeat is received after all retries, the request fails.

### Crash Recovery

The client logs pending requests to disk. On restart, any pending requests from previous sessions are automatically cancelled on the server.

## Example Service

The included server application implements a primality testing service:

- Service ID: 1
- Input: Integer as ASCII string
- Output: "[number] is prime" or "[number] is not prime"

Example session:

```
$ python3 client_app2.py
[APP] Enter a number (or type 'exit' to quit): 17
[APP] Server Response: 17 is prime
[APP] Enter a number (or type 'exit' to quit): 20
[APP] Server Response: 20 is not prime
```

## License

MIT
