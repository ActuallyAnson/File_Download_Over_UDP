/*!*****************************************************************************
\file echoserver.cpp
\author Mohamed Ridhwan Bin Mohamed Afandi (mohamedridhwan.b\@digipen.edu)
\author Jeremy Lim Ting Jie (jeremytingjie.lim\@digipen.edu)
\author Anson Teng (anson.t\@digipen.edu)
\par Course: CSD2160
\par Assignment: Assignment 3
\date 03/18/2025
\brief
This file implements a UDP client for file transfer and command communication.

Copyright (C) 2025 DigiPen Institute of Technology.
Reproduction or disclosure of this file or its contents without the
prior written consent of DigiPen Institute of Technology is prohibited.
*******************************************************************************/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

//#define SIMULATE_PACKET_LOSS
//#define SIMULATE_CORRUPTED_PACKET

#include "Windows.h"        // Entire Win32 API...
#include "winsock2.h"       // ...or Winsock alone
#include "ws2tcpip.h"       // getaddrinfo()

#pragma comment(lib, "ws2_32.lib")
#include <cstdio>
#include <iostream>         // cout, cerr
#include <string>           // string
#include <vector>           // vector
#include <filesystem>       // filesystem
#include <fstream>          // ifstream
#include "taskqueue.h"
#include <map>

#ifndef WINSOCK_VERSION
#define WINSOCK_VERSION 2
#endif
#ifndef WINSOCK_SUBVERSION
#define WINSOCK_SUBVERSION 2
#endif
#define MAX_STR_LEN         1000
#define RETURN_CODE_1       1
#define RETURN_CODE_2       2
#define RETURN_CODE_3       3
#define RETURN_CODE_4       4

const uint32_t UDP_PAYLOAD_SIZE = 1472;
std::chrono::seconds g_keepAliveTimeout(10); // Keep-alive timeout in seconds
const int MIN_TIMEOUT_MS = 50;               // Minimum Timeout value for ACKs received

enum CMDID {
    UNKNOWN = (unsigned char)0x0,
    REQ_QUIT = (unsigned char)0x1,
    REQ_DOWNLOAD = (unsigned char)0x2,
    RSP_DOWNLOAD = (unsigned char)0x3,
    REQ_LISTFILES = (unsigned char)0x4,
    RSP_LISTFILES = (unsigned char)0x5,
    CMD_TEST = (unsigned char)0x20,
    DOWNLOAD_ERROR = (unsigned char)0x30
};
struct ClientInfo {
    SOCKET socket;
    std::string ip;
    std::string tcpPort;
    std::string udpPort;
};

struct AckPacket {
    uint32_t sessionId;       // Unique identifier for the file transfer session.
    uint32_t sequenceNumber;  // The missing packet's sequence number.
    bool received;            // false indicates request to retransmit.
};

std::mutex g_lastAckTimesMutex;
std::map<uint32_t, std::chrono::steady_clock::time_point> g_lastAckTimes;

struct PacketInfo {
    std::vector<uint8_t> data;
    std::chrono::steady_clock::time_point sendTime;
    sockaddr_in clientAddr = {};
};


// Unified Tracker for all packets
// SessionID -> Packet sequence number -> Payload
std::map<uint32_t, std::map<uint32_t, PacketInfo>> gPacketBuffer;
std::mutex g_packetBufferMutex;

static std::vector<ClientInfo> g_connectedClients;
static std::mutex g_clientsMutex;
static std::string directoryPath;

SOCKET udpSocket = INVALID_SOCKET;

// Global session ID counter
static std::atomic<uint32_t> g_sessionCounter{ 0 };

// Global RTT parameters (in milliseconds)
std::chrono::milliseconds g_estimatedRTT(500);
std::chrono::milliseconds g_devRTT(0);
std::chrono::milliseconds g_dynamicTimeout = g_estimatedRTT + 4 * g_devRTT;


//  function to process UDP ACK packets and handle retransmission requests
void processUdpAcks() {
    std::thread([&]() {
        const size_t BUFFER_SIZE = 1500;
        char buffer[BUFFER_SIZE];

        while (udpSocket != INVALID_SOCKET) {
            sockaddr_in clientAddr = {};
            int clientAddrLen = sizeof(clientAddr);
            int bytesReceived = recvfrom(udpSocket, buffer, BUFFER_SIZE, 0,
                reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);

            if (bytesReceived >= 9) { // sessionId(4) + seqNum(4) + flag(1)
                // Extract session ID and sequence number
                uint32_t sessionId;
                uint32_t seqNum;
                uint8_t received;

                memcpy(&sessionId, buffer, sizeof(sessionId));
                memcpy(&seqNum, buffer + 4, sizeof(seqNum));
                memcpy(&received, buffer + 8, sizeof(received));

                sessionId = ntohl(sessionId);
                seqNum = ntohl(seqNum);

                // If received flag is false (0), client is requesting retransmission
                if (!received) {
                    std::vector<uint8_t> packet;
                    {
                        std::lock_guard<std::mutex> lock(g_packetBufferMutex);
                        auto sessionIt = gPacketBuffer.find(sessionId);
                        if (sessionIt != gPacketBuffer.end()) {
                            auto pktIt = sessionIt->second.find(seqNum);
                            if (pktIt != sessionIt->second.end()) {
                                packet = pktIt->second.data;
                            }
                        }
                    }
                    if (!packet.empty()) {
                        int sendResult = sendto(udpSocket,
                            reinterpret_cast<const char*>(packet.data()),
                            static_cast<int>(packet.size()),
                            0,
                            reinterpret_cast<sockaddr*>(&clientAddr),
                            clientAddrLen);
                        if (sendResult == SOCKET_ERROR) {
                            //std::cerr << "Retransmit sendto() failed for session "
                            //    << sessionId << " seq " << seqNum << std::endl;
                        }
                        else {
                            //std::cout << "Retransmitted packet: session "
                            //    << sessionId << " sequence " << seqNum << std::endl;
                        }
                    }
                }
                else
                {
                    PacketInfo pi;
                    bool found = false;
                    {
                        std::lock_guard<std::mutex> lock(g_packetBufferMutex);
                        auto sessionIt = gPacketBuffer.find(sessionId);
                        if (sessionIt != gPacketBuffer.end()) {
                            auto pktIt = sessionIt->second.find(seqNum);
                            if (pktIt != sessionIt->second.end()) {
                                pi = pktIt->second;
                                sessionIt->second.erase(pktIt);
                                found = true;
                            }
                        }
                    }
                    if (found) {
                        // Calculate new RTT and dynamic timeout
                        // Exponential Weighted Moving Average (EWMA)
                        auto sampleRTT = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - pi.sendTime);
                        double alpha = 0.125, beta = 0.25;
                        double sample = static_cast<double>(sampleRTT.count());
                        double est = static_cast<double>(g_estimatedRTT.count());
                        double dev = static_cast<double>(g_devRTT.count());
                        double newEst = (1 - alpha) * est + alpha * sample;
                        double newDev = (1 - beta) * dev + beta * fabs(sample - newEst);
                        g_estimatedRTT = std::chrono::milliseconds(static_cast<int>(newEst));
                        g_devRTT = std::chrono::milliseconds(static_cast<int>(newDev));
                        g_dynamicTimeout = max(g_estimatedRTT + 4 * g_devRTT, std::chrono::milliseconds(MIN_TIMEOUT_MS));
                    }
                    
                }
                {
					std::lock_guard<std::mutex> lock(g_lastAckTimesMutex);
                    // Update lastAckTime for the session
                    auto it = g_lastAckTimes.find(sessionId);
                    if (it != g_lastAckTimes.end()) {
                        it->second = std::chrono::steady_clock::now();
                    }
                }

            }
        }
        }).detach();
}


//1. track heartbeat per session
//2. clear buffer for session
void heartbeatChecker() {
    while (udpSocket != INVALID_SOCKET) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock1(g_lastAckTimesMutex);
        std::lock_guard<std::mutex> lock2(g_packetBufferMutex);
        for (auto it = g_lastAckTimes.begin(); it != g_lastAckTimes.end(); ) {
            if (now - it->second > g_keepAliveTimeout) {
                uint32_t sessionId = it->first;
                std::cout << "Session " << sessionId << " timed out." << std::endl;
                // Remove session from gPacketBuffer
                gPacketBuffer.erase(sessionId);
                // Remove session from g_lastAckTimes
                it = g_lastAckTimes.erase(it);
            }
            else {
                ++it;
            }
        }
    }
}





// Retransmission timer thread. Periodically scans gPacketBuffer and retransmits
// any packet whose elapsed time exceeds the dynamic timeout.
void retransmissionTimer() {
    while (udpSocket != INVALID_SOCKET) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_packetBufferMutex);
        for (auto& sessionPair : gPacketBuffer) {
            for (auto& pktPair : sessionPair.second) {
                PacketInfo& pi = pktPair.second;
                if (now - pi.sendTime >= g_dynamicTimeout) {
                    int sendResult = sendto(udpSocket,
                        reinterpret_cast<const char*>(pi.data.data()),
                        static_cast<int>(pi.data.size()),
                        0,
                        reinterpret_cast<sockaddr*>(&pi.clientAddr),
                        sizeof(pi.clientAddr));
                    if (sendResult != SOCKET_ERROR) {
                        pi.sendTime = now;
                        //std::cout << "Retransmitted packet: session "
                        //    << sessionPair.first << " sequence " << pktPair.first << std::endl;
                    }
                    //else {
                    //    std::cerr << "Retransmission failed for session " << sessionPair.first
                    //        << " seq " << pktPair.first << std::endl;
                    //}
                }
            }
        }
    }
}

void disconnect(SOCKET& listenerSocket);
bool execute(SOCKET clientSocket);
int main()
{
    // Prompt User For server's TCP, UDP and path of the files to be served
    std::string tcpPortNumber, udpPortNumber;
    std::cout << "Server TCP Port Number: ";
    std::getline(std::cin, tcpPortNumber);

    std::cout << "Server UDP Port Number: ";
    std::getline(std::cin, udpPortNumber);

    std::cout << "Path: ";
    std::getline(std::cin, directoryPath);

    namespace fs = std::filesystem;
    if (!fs::exists(directoryPath) || !fs::is_directory(directoryPath))
    {
        std::cerr << "Invalid directory path: " << directoryPath << std::endl;
        return RETURN_CODE_4;
    }

    // -------------------------------------------------------------------------
    // Start up Winsock, asking for version 2.2.
    //
    // WSAStartup()
    // -------------------------------------------------------------------------

    // This object holds the information about the version of Winsock that we
    // are using, which is not necessarily the version that we requested.
    WSADATA wsaData{};

    // Initialize Winsock. You must call WSACleanup when you are finished.
    // As this function uses a reference counter, for each call to WSAStartup,
    // you must call WSACleanup or suffer memory issues.
    int errorCode = WSAStartup(MAKEWORD(WINSOCK_VERSION, WINSOCK_SUBVERSION), &wsaData);
    if (NO_ERROR != errorCode)
    {
        std::cerr << "WSAStartup() failed." << std::endl;
        return errorCode;
    }

    // -------------------------------------------------------------------------
    // Resolve own host name into IP addresses (in a singly-linked list).
    //
    // getaddrinfo()
    // -------------------------------------------------------------------------
    // Object hints indicates which protocols to use to fill in the info.
    addrinfo hints{};
    SecureZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;           // IPv4
    // For UDP use SOCK_DGRAM instead of SOCK_STREAM.
    hints.ai_socktype = SOCK_STREAM;    // Reliable delivery
    // Could be 0 for autodetect, but reliable delivery over IPv4 is always TCP.
    hints.ai_protocol = IPPROTO_TCP;    // TCP
    // Create a passive socket that is suitable for bind() and listen().
    hints.ai_flags = AI_PASSIVE;

    char host[MAX_STR_LEN];
    gethostname(host, MAX_STR_LEN);

    addrinfo* tcpInfo = nullptr;
    errorCode = getaddrinfo(host, tcpPortNumber.c_str(), &hints, &tcpInfo);
    if ((NO_ERROR != errorCode) || (nullptr == tcpInfo))
    {
        std::cerr << "getaddrinfo() for TCP failed." << std::endl;
        WSACleanup();
        return errorCode;
    }

    // Update hints for UDP
    hints.ai_socktype = SOCK_DGRAM;    // Datagram for UDP
    hints.ai_protocol = IPPROTO_UDP;   // UDP

    addrinfo* udpInfo = nullptr;
    errorCode = getaddrinfo(host, udpPortNumber.c_str(), &hints, &udpInfo);
    if ((NO_ERROR != errorCode) || (nullptr == udpInfo))
    {
        std::cerr << "getaddrinfo() for UDP failed." << std::endl;
        freeaddrinfo(tcpInfo);
        WSACleanup();
        return errorCode;
    }

    /* PRINT SERVER IP ADDRESS AND PORT NUMBER */
    char serverIPAddr[MAX_STR_LEN];
    struct sockaddr_in* serverAddress = reinterpret_cast<struct sockaddr_in*> (tcpInfo->ai_addr);
    inet_ntop(AF_INET, &(serverAddress->sin_addr), serverIPAddr, INET_ADDRSTRLEN);
    getnameinfo(tcpInfo->ai_addr, static_cast <socklen_t> (tcpInfo->ai_addrlen), serverIPAddr, sizeof(serverIPAddr), nullptr, 0, NI_NUMERICHOST);
    std::cout << std::endl;
    std::cout << "Server IP Address: " << serverIPAddr << std::endl;
    std::cout << "Server TCP Port Number: " << tcpPortNumber << std::endl;
    std::cout << "Server UDP Port Number: " << udpPortNumber << std::endl;
    std::cout << "Path: " << directoryPath << std::endl;

    // -------------------------------------------------------------------------
    // Create a socket and bind it to own network interface controller.
    //
    // socket()
    // bind()
    // -------------------------------------------------------------------------

    SOCKET tcpListenerSocket = socket(
        tcpInfo->ai_family,
        tcpInfo->ai_socktype,
        tcpInfo->ai_protocol);
    if (INVALID_SOCKET == tcpListenerSocket)
    {
        std::cerr << "TCP socket() failed." << std::endl;
        freeaddrinfo(tcpInfo);
        freeaddrinfo(udpInfo);
        WSACleanup();
        return RETURN_CODE_1;
    }


    errorCode = bind(
        tcpListenerSocket,
        tcpInfo->ai_addr,
        static_cast<int>(tcpInfo->ai_addrlen));
    if (NO_ERROR != errorCode)
    {
        std::cerr << "TCP bind() failed." << std::endl;
        closesocket(tcpListenerSocket);
        freeaddrinfo(tcpInfo);
        freeaddrinfo(udpInfo);
        WSACleanup();
        return RETURN_CODE_2;
    }

    freeaddrinfo(tcpInfo);

    if (INVALID_SOCKET == tcpListenerSocket)
    {
        std::cerr << "TCP bind() failed." << std::endl;
        WSACleanup();
        return RETURN_CODE_2;
    }

    // Create and bind UDP socket
    udpSocket = socket(
        udpInfo->ai_family,
        udpInfo->ai_socktype,
        udpInfo->ai_protocol);
    if (INVALID_SOCKET == udpSocket)
    {
        std::cerr << "UDP socket() failed." << std::endl;
        closesocket(tcpListenerSocket);
        freeaddrinfo(udpInfo);
        WSACleanup();
        return RETURN_CODE_1;
    }

    // Set UDP socket buffer size
    int bufferSize = 1500; 
    if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, (char*)&bufferSize, sizeof(bufferSize)) == SOCKET_ERROR) {
        std::cerr << "Failed to set UDP receive buffer size." << std::endl;
    }
    if (setsockopt(udpSocket, SOL_SOCKET, SO_SNDBUF, (char*)&bufferSize, sizeof(bufferSize)) == SOCKET_ERROR) {
        std::cerr << "Failed to set UDP send buffer size." << std::endl;
    }

    errorCode = bind(
        udpSocket,
        udpInfo->ai_addr,
        static_cast<int>(udpInfo->ai_addrlen));
    if (NO_ERROR != errorCode)
    {
        std::cerr << "UDP bind() failed." << std::endl;
        closesocket(tcpListenerSocket);
        closesocket(udpSocket);
        freeaddrinfo(udpInfo);
        WSACleanup();
        return RETURN_CODE_2;
    }

    freeaddrinfo(udpInfo);

    // Start UDP ACK processing thread
    std::cout << "Starting UDP ACK processing thread..." << std::endl;
    processUdpAcks();
    std::cout << "Starting retransmission timer thread..." << std::endl;
    std::thread timerThread(retransmissionTimer);
    timerThread.detach();
    std::cout << "Starting heartbeat checker thread..." << std::endl;
    std::thread heartbeatThread(heartbeatChecker);
    heartbeatThread.detach();

    // -------------------------------------------------------------------------
    // Set a socket in a listening mode and accept incoming clients.
    //
    // listen()
    // accept()
    // -------------------------------------------------------------------------

    errorCode = listen(tcpListenerSocket, SOMAXCONN);
    if (NO_ERROR != errorCode)
    {
        std::cerr << "listen() failed." << std::endl;
        closesocket(tcpListenerSocket);
        closesocket(udpSocket);
        WSACleanup();
        return RETURN_CODE_3;
    }

    {
        const auto onDisconnect = [&]() { disconnect(tcpListenerSocket); };
        auto tq = TaskQueue<SOCKET, decltype(execute), decltype(onDisconnect)>{ 10, 20, execute, onDisconnect };
        while (tcpListenerSocket != INVALID_SOCKET)
        {
            sockaddr clientAddress{};
            SecureZeroMemory(&clientAddress, sizeof(clientAddress));
            int clientAddressSize = sizeof(clientAddress);
            SOCKET clientSocket = accept(
                tcpListenerSocket,
                &clientAddress,
                &clientAddressSize);
            if (clientSocket == INVALID_SOCKET)
            {
                break;
            }
            // PRINT CLIENT IP ADDRESS AND PORT NUMBER
            char clientIPAddr[MAX_STR_LEN];
            char clientPort[MAX_STR_LEN];
            getpeername(clientSocket, &clientAddress, &clientAddressSize);
            getnameinfo(&clientAddress, clientAddressSize, clientIPAddr, sizeof(clientIPAddr), clientPort, sizeof(clientPort), NI_NUMERICHOST);
            std::cout << std::endl;
            std::cout << "Client IP Address: " << clientIPAddr << std::endl;
            std::cout << "Client Port Number: " << clientPort << std::endl;
            {
                std::lock_guard<std::mutex> lock(g_clientsMutex);
                ClientInfo newClient{ clientSocket, clientIPAddr, clientPort, ""};
                g_connectedClients.push_back(newClient);
            }

            tq.produce(clientSocket);
        }
    }

    // Shut down and close sockets.
    shutdown(tcpListenerSocket, SD_BOTH);
    closesocket(tcpListenerSocket);
    closesocket(udpSocket);

    // Clean-up after Winsock.
    WSACleanup();
}

void disconnect(SOCKET& listenerSocket)
{
    if (listenerSocket != INVALID_SOCKET)
    {
        shutdown(listenerSocket, SD_BOTH);
        closesocket(listenerSocket);
        listenerSocket = INVALID_SOCKET;
    }
}

bool execute(SOCKET clientSocket)
{

    // -------------------------------------------------------------------------
    // Receive some text and send it back.
    //
    // recv()
    // send()
    // -------------------------------------------------------------------------

    constexpr size_t BUFFER_SIZE = 1000;
    char buffer[BUFFER_SIZE];
    bool stay = true;

    while (true)
    {
        const int bytesReceived = recv(
            clientSocket,
            buffer,
            BUFFER_SIZE - 1,
            0);
        if (bytesReceived == SOCKET_ERROR)
        {
            std::cerr << "recv() failed. Client may have disconnected." << std::endl;
            break;
        }
        if (bytesReceived == 0)
        {
            std::cerr << "Client disconnected." << std::endl;
            break;
        }

        buffer[bytesReceived] = '\0';

        std::string text(buffer, bytesReceived);

        if (text == "*")
        {
            std::cout << "Requested to close the server!" << std::endl;
            stay = false;
            break;
        }
        uint8_t commandID = buffer[0];
        if (commandID == REQ_LISTFILES)
        {
            std::cout << "Sending file list!" << std::endl;
            std::vector<uint8_t> response;
            response.push_back(RSP_LISTFILES);

            namespace fs = std::filesystem;
            // Collect regular files from the directory.
            std::vector<fs::directory_entry> files;
            for (const auto& entry : fs::directory_iterator(directoryPath))
            {
                if (entry.is_regular_file())
                    files.push_back(entry);
            }

            // Number of Files [2 bytes]: network order.
            uint16_t numFiles = static_cast<uint16_t>(files.size());
            uint16_t netNumFiles = htons(numFiles);
            response.insert(response.end(), reinterpret_cast<uint8_t*>(&netNumFiles),
                reinterpret_cast<uint8_t*>(&netNumFiles) + sizeof(netNumFiles));

            // Compute Length of File List [4 bytes]:
            // For each file, add 4 (for filename length field) + filename size.
            uint32_t totalLength = 0;
            for (const auto& entry : files)
            {
                std::string filename = entry.path().filename().generic_string();
                totalLength += sizeof(uint32_t) + static_cast<uint32_t>(filename.size());
            }
            uint32_t netTotalLength = htonl(totalLength);
            response.insert(response.end(), reinterpret_cast<uint8_t*>(&netTotalLength),
                reinterpret_cast<uint8_t*>(&netTotalLength) + sizeof(netTotalLength));

            // Add for each file: Filename Length (4 bytes) and Filename.
            for (const auto& entry : files)
            {
                std::string filename = entry.path().filename().generic_string();
                uint32_t nameLen = static_cast<uint32_t>(filename.size());
                uint32_t netNameLen = htonl(nameLen);
                response.insert(response.end(), reinterpret_cast<uint8_t*>(&netNameLen),
                    reinterpret_cast<uint8_t*>(&netNameLen) + sizeof(netNameLen));
                response.insert(response.end(), filename.begin(), filename.end());
            }

            // Send the response to the client.
            send(clientSocket, reinterpret_cast<const char*>(response.data()),
                static_cast<int>(response.size()), 0);
        }
        else if (commandID == REQ_DOWNLOAD)
        {
            // Parse client's UDP IP address (4 bytes) from the message.
            uint32_t clientUdpIp;
            memcpy(&clientUdpIp, buffer + 1, sizeof(uint32_t));
            // (Optional) Convert to dotted-decimal string if needed:
            // char clientIpStr[INET_ADDRSTRLEN];
            // inet_ntop(AF_INET, &clientUdpIp, clientIpStr, INET_ADDRSTRLEN);

            // Parse client's UDP port number (2 bytes).
            uint16_t clientUdpPort;
            memcpy(&clientUdpPort, buffer + 1 + sizeof(uint32_t), sizeof(uint16_t));
            // (Optional) Convert to host order if needed: clientUdpPort = ntohs(clientUdpPort);

            // Parse filename length (4 bytes in network order).
            uint32_t filenameLength;
            memcpy(&filenameLength, buffer + 1 + sizeof(uint32_t) + sizeof(uint16_t), sizeof(uint32_t));
            filenameLength = ntohl(filenameLength);

            // Extract the filename (which includes path).
            std::string requestedFile(buffer + 1 + 4 + 2 + 4, filenameLength);
            std::cout << "Download requested for file: " << requestedFile << std::endl;

            namespace fs = std::filesystem;
            fs::path fullPath = fs::path(directoryPath) / requestedFile;
            if (!fs::exists(fullPath) || !fs::is_regular_file(fullPath))
            {
                std::cerr << "File not found: " << fullPath.string() << std::endl;
                std::vector<uint8_t> errorResponse;
                errorResponse.push_back(DOWNLOAD_ERROR);
                send(clientSocket, reinterpret_cast<const char*>(errorResponse.data()),
                    static_cast<int>(errorResponse.size()), 0);
                continue;
            }

            // Determine the file size.
            uint32_t fileLength = static_cast<uint32_t>(fs::file_size(fullPath));

            // Get the server's UDP addressing info.
            sockaddr_in udpAddr = {};
            int udpAddrLen = sizeof(udpAddr);
            if (getsockname(udpSocket, reinterpret_cast<sockaddr*>(&udpAddr), &udpAddrLen) == SOCKET_ERROR)
            {
                std::cerr << "getsockname() for UDP socket failed." << std::endl;
                std::vector<uint8_t> errorResponse;
                errorResponse.push_back(DOWNLOAD_ERROR);
                send(clientSocket, reinterpret_cast<const char*>(errorResponse.data()),
                    static_cast<int>(errorResponse.size()), 0);
                continue;
            }

            // Build the RSP_DOWNLOAD header
            std::vector<uint8_t> downloadResponse;
            downloadResponse.push_back(RSP_DOWNLOAD);

            // i. IP Address (4 bytes) from the UDP socket.
            uint32_t ipField = udpAddr.sin_addr.s_addr; // Already in network order.
            downloadResponse.insert(downloadResponse.end(),
                reinterpret_cast<uint8_t*>(&ipField),
                reinterpret_cast<uint8_t*>(&ipField) + sizeof(ipField));

            // ii. Port Number (2 bytes) from the UDP socket.
            uint16_t portField = udpAddr.sin_port; // Already in network order.
            downloadResponse.insert(downloadResponse.end(),
                reinterpret_cast<uint8_t*>(&portField),
                reinterpret_cast<uint8_t*>(&portField) + sizeof(portField));

            // iii. Session ID (4 bytes)
            uint32_t sessionId = g_sessionCounter.fetch_add(1);
            uint32_t netSessionId = htonl(sessionId);
            downloadResponse.insert(downloadResponse.end(),
                reinterpret_cast<uint8_t*>(&netSessionId),
                reinterpret_cast<uint8_t*>(&netSessionId) + sizeof(netSessionId));

            // iv. File Length (4 bytes): converted to network order.
            uint32_t netFileLength = htonl(fileLength);
            downloadResponse.insert(downloadResponse.end(),
                reinterpret_cast<uint8_t*>(&netFileLength),
                reinterpret_cast<uint8_t*>(&netFileLength) + sizeof(netFileLength));

            // Send the complete header in one TCP stream (no file data).
            if (send(clientSocket, reinterpret_cast<const char*>(downloadResponse.data()),
                static_cast<int>(downloadResponse.size()), 0) == SOCKET_ERROR)
            {
                std::cerr << "Error sending RSP_DOWNLOAD." << std::endl;
				continue;
            }
            else
            {
				std::cout << "Session " << sessionId << " created for file: " << requestedFile << std::endl;
                {
					std::lock_guard<std::mutex> lock(g_lastAckTimesMutex);
                    // New session is created, create new heartbeat timer
                    g_lastAckTimes[sessionId] = std::chrono::steady_clock::now();
                }

                // Prepare client's UDP address from the parsed values.
                sockaddr_in clientUdpAddr{};
                clientUdpAddr.sin_family = AF_INET;
                // 'clientUdpIp' and 'clientUdpPort' were parsed from the TCP message.
                clientUdpAddr.sin_addr.s_addr = clientUdpIp; // Already in network order.
                clientUdpAddr.sin_port = clientUdpPort;        // Already in network order.

                // Open the requested file.
                std::ifstream file(fullPath, std::ios::binary);
                if (!file)
                {
                    std::cerr << "Failed to open file: " << fullPath.string() << std::endl;
                    continue;
                }

                uint32_t sequenceNumber = 0;
                // Read and send file data in chunks.
                while (file)
                {
                    // Reserve a buffer for UDP_PAYLOAD_SIZE bytes.
                    std::vector<uint8_t> fileData(UDP_PAYLOAD_SIZE);
                    file.read(reinterpret_cast<char*>(fileData.data()), UDP_PAYLOAD_SIZE);
                    std::streamsize bytesRead = file.gcount();
                    if (bytesRead <= 0)
                        break;
                    fileData.resize(static_cast<size_t>(bytesRead));

                    // Compute a simple checksum over the data.
                    uint32_t sum = 0;
                    for (auto byte : fileData)
                        sum += byte;
                    uint16_t checksum = static_cast<uint16_t>(sum & 0xFFFF);

                    // Build the UDP packet buffer.
                    std::vector<uint8_t> packetBuffer;
                    // Packet header: 4 + 4 + 4 + 2 bytes = 14 bytes, then data.
                    packetBuffer.resize(14 + fileData.size());
                    size_t offset = 0;

                    // Session ID (4 bytes)
                    uint32_t netSessionId = htonl(sessionId);
                    memcpy(packetBuffer.data() + offset, &netSessionId, sizeof(netSessionId));
                    offset += sizeof(netSessionId);

                    // Sequence Number (4 bytes)
                    uint32_t netSeq = htonl(sequenceNumber);
                    memcpy(packetBuffer.data() + offset, &netSeq, sizeof(netSeq));
                    offset += sizeof(netSeq);

                    // Packet Length (4 bytes): length of the data payload.
                    uint32_t netPacketLength = htonl(static_cast<uint32_t>(fileData.size()));
                    memcpy(packetBuffer.data() + offset, &netPacketLength, sizeof(netPacketLength));
                    offset += sizeof(netPacketLength);

                    // Checksum (2 bytes)
                    uint16_t netChecksum = htons(checksum);
                    memcpy(packetBuffer.data() + offset, &netChecksum, sizeof(netChecksum));
                    offset += sizeof(netChecksum);

                    // Copy the actual data payload.
                    memcpy(packetBuffer.data() + offset, fileData.data(), fileData.size());

                    // Save packet info with send time and destination address
                    PacketInfo pi;
                    pi.data = packetBuffer;
                    pi.sendTime = std::chrono::steady_clock::now();
                    pi.clientAddr = clientUdpAddr;
                    // Save to gPacketBuffer before sending packet in the download branch:
                    {
                        std::lock_guard<std::mutex> lock(g_packetBufferMutex);
                        gPacketBuffer[sessionId][sequenceNumber] = pi;
                    }
#ifdef SIMULATE_CORRUPTED_PACKET
                    if (sequenceNumber % 5000 == 0)
                    {
                        std::cout << "Simulating corrupted packet for session " << sessionId
                            << " sequence " << sequenceNumber << std::endl;
                        uint16_t netChecksum = htons(0);
                        memcpy(packetBuffer.data() + offset, &netChecksum, sizeof(netChecksum));
                        offset += sizeof(netChecksum);
                    }
#endif
#ifdef SIMULATE_PACKET_LOSS
                    if (sequenceNumber < 1000 && sequenceNumber > 900)
					{
						std::cout << "Simulating packet loss for session " << sessionId
							<< " sequence " << sequenceNumber << std::endl;
						sequenceNumber++;
						continue;
					}
#endif
                    // Send the UDP packet.
                    int sendResult = sendto(udpSocket, reinterpret_cast<const char*>(packetBuffer.data()),
                        static_cast<int>(packetBuffer.size()), 0,
                        reinterpret_cast<sockaddr*>(&clientUdpAddr), sizeof(clientUdpAddr));
                    if (sendResult == SOCKET_ERROR)
                    {
                        std::cerr << "sendto failed, terminating UDP transfer." << std::endl;
                        break;
                    }
                    sequenceNumber++;
                }
            }
        }


		else if (commandID == REQ_QUIT)
		{
            std::cerr << "shutdown." << std::endl;
            break;
        }
    }


    // -------------------------------------------------------------------------
    // Shut down and close sockets.
    //
    // shutdown()
    // closesocket()
    // -------------------------------------------------------------------------

    shutdown(clientSocket, SD_BOTH);
    closesocket(clientSocket);
	// Remove client from the list
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_connectedClients.erase(
            std::remove_if(g_connectedClients.begin(),
                g_connectedClients.end(),
                [clientSocket](const ClientInfo& client) {
                    return client.socket == clientSocket;
                }),
            g_connectedClients.end());
    }
    return stay;
}
