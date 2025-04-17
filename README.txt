============================
    HOW TO USE
============================

# File Transfer Application User Guide


## System Requirements
* Windows OS with Winsock support (for server and client)
* Visual Studio (e.g., 2019 or 2022) with C++ development workload
* Compile in Release Mode
* Network connectivity between devices

## Setup Instructions

### Server Configuration
1. Launch the server application (echoserver)
2. Configure the following parameters:
   * TCP port number (e.g., 1111)
   * UDP port number (e.g., 2222)
   * File storage directory (e.g., .\Test_Downloads\Server_Downloads or absolute path)
3. Note the displayed Server IP address for client connections

### Client Configuration
1. Launch the client application
2. Configure the following parameters:
   * Server IP address (from server setup)
   * Server TCP port (matching server configuration, e.g., 1111)
   * Server UDP port (matching server configuration, e.g., 2222)
   * Client UDP port (e.g., 3333)
   * File storage directory (e.g., .\Test_Downloads\Client_Downloads or absolute path)


## Usage Instructions

### Basic Commands
* `/l` - List all available files stored on the server
* `/d clientIP:clientUDPport filename` - Download the specified file
* `/q` - Quit the client connection gracefully.

### Download Process
1. Issue a download command (e.g., `/d 192.168.1.101:3333 1`).
2. The server sends file chunks over UDP with Selective Repeat reliability.
3. The client verifies each packet’s checksum and requests retransmission if invalid.
4. Wait for "File downloaded successfully" message (times out after 10 minutes if incomplete).
5. Open the file in the client directory to verify integrity.

### Multiple Client Operation
1. Launch additional clients with unique UDP ports (e.g., 3333, 4444).
2. Use the same server IP and ports for each client.
3. The server supports concurrent transfers via a task queue and session IDs.


### Shutting Down
* Client: Enter `/q` to disconnect gracefully.
* Server: Send `*` from a client via TCP (manual connection required).

## Error Handling
* Server:
- Validates directory path existence.
- Returns `DOWNLOAD_ERROR` for non-existent files.
* Client:
- Checks directory path and port validity (1024-65535).
- Ensures IP/port match connected server/client UDP settings.
- Reports checksum failures and timeouts (10 minutes).

## Testing Large Files
* Tested with files ≥ 200MB under simulated packet loss (enable `SIMULATE_PACKET_LOSS` in server code).
* Selective Repeat ensures reliability; UDP pipelining boosts speed.

## Notes
* Uses Selective Repeat over UDP for efficient, reliable transfers.
* Dynamic timeouts (RTT-based, min 50ms) adapt to network conditions.
* Performance: Tested 100MB in ~2s with simulation of packet loss (approx. 50.00 MB/s, varies by network).
* Performance: Tested 400MB in ~4s without simulation of packet loss (approx. 100.00 MB/s, varies by network).

