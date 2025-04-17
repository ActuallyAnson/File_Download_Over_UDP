/*!*****************************************************************************
\file echoclient.cpp
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

#include "Windows.h"        // Entire Win32 API...
#include "winsock2.h"       // ...or Winsock alone
#include "ws2tcpip.h"       // getaddrinfo()

#pragma comment(lib, "ws2_32.lib")

#include <iostream>         // cout, cerr
#include <string>           // string
#include <vector>
#include <fstream>          // ifstream
#include <filesystem>       // exists, is_directory
#include <sstream>          // hex
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
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
const std::chrono::minutes TIMEOUT(10);

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

std::mutex gResponseMutex;
std::condition_variable gResponseCV;
std::vector<uint8_t> gResponseBuffer;
bool gResponseReceived = false;

// Structure to represent a UDP file-transfer packet.
struct FileTransferPacket {
    uint32_t sessionId;       // Unique identifier for the file transfer session.
    uint32_t sequenceNumber;  // Sequence number of the packet.
    uint32_t packetLength;    // Length of the current packet.
    uint16_t checksum;        // Checksum for integrity verification.
    std::vector<uint8_t> data; // Actual data payload.

    // Constructor to initialize all member variables
    FileTransferPacket()
        : sessionId(0), sequenceNumber(0), packetLength(0), checksum(0) {}

    bool verifyChecksum() const {
        // (Optionally) compute checksum over 'data' and compare with checksum.
        uint32_t sum = 0;
        for (auto byte : data)
            sum += byte;
        return ((sum & 0xFFFF) == checksum);
    }
};


// Global UDP client socket.
SOCKET UDPClientSocket = INVALID_SOCKET;

// Define a simple AckPacket structure to request retransmission.
struct AckPacket {
    uint32_t sessionId;       // Unique identifier for the file transfer session.
    uint32_t sequenceNumber;  // The missing packet's sequence number.
    bool received;            // false indicates request to retransmit.

	// constructor
    AckPacket(uint32_t sessionId, uint32_t sequenceNumber, bool received) : sessionId(sessionId), sequenceNumber(sequenceNumber), received(received)
    {
		this->sessionId = htonl(sessionId);
		this->sequenceNumber = htonl(sequenceNumber);
		send(UDPClientSocket, reinterpret_cast<const char*>(this), sizeof(AckPacket), 0);
    }
};

// Global container for file packets along with sync primitives.
std::mutex gFileMutex;
std::condition_variable gFileCV;

// SessionID -> Packet sequence number -> FileTransferPacket
std::map < uint32_t, std::map<uint32_t, FileTransferPacket >> gFilePackets;
// SessionID -> Expected number of packets.
std::map<uint32_t, uint32_t> gExpectedPackets;

// Global destination IP address and port (set based on the server connection).
uint32_t gDestIP = 0;
uint16_t gDestPort = 0;
static std::string directoryPath;


struct Message {
    CMDID command;
    uint32_t ipAddress;    // in host byte order; will be converted during serialization
    uint16_t port;         // in host byte order; will be converted during serialization
    uint32_t textLength;
    std::vector<uint8_t> text;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;
        // Append the 1-byte Command ID.
        buffer.push_back(static_cast<uint8_t>(command));

        // Convert IP address to network byte order.
        uint32_t netIp = htonl(ipAddress);
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&netIp),
            reinterpret_cast<const uint8_t*>(&netIp) + sizeof(netIp));

        // Convert port to network byte order.
        uint16_t netPort = htons(port);
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&netPort),
            reinterpret_cast<const uint8_t*>(&netPort) + sizeof(netPort));

        // Append text length (4 bytes, network byte order).
        uint32_t netTextLength = htonl(textLength);
        buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&netTextLength),
            reinterpret_cast<const uint8_t*>(&netTextLength) + sizeof(netTextLength));

        // Append the text payload.
        buffer.insert(buffer.end(), text.begin(), text.end());

        return buffer;
    }
};

// Converts a hexadecimal string to a vector of bytes.
std::vector<uint8_t> hexStringToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtol(byteString.c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

// Function to get the current timestamp as a string
std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm timeInfo;
    localtime_s(&timeInfo, &in_time_t);
    std::stringstream ss;
    ss << std::put_time(&timeInfo, "%Y-%m-%d %X");
    return ss.str();
}

// Receive the response from the server.
void receiveCommand(SOCKET clientSocket, std::atomic<bool>& running) {
    char buffer[MAX_STR_LEN];
    int bytesReceived = recv(clientSocket, buffer, MAX_STR_LEN, 0);
    if (bytesReceived <= 0) {  // <= 0 catches both errors and clean closes
        if (bytesReceived == SOCKET_ERROR) {
            std::cerr << "recv() failed. Server may have closed the connection." << std::endl;
        }
        else {
            std::cout << "Server closed the connection." << std::endl;
        }
        // Signal to the main thread that we should stop
        running.store(false);
        return;
    }
    if (buffer[0] == REQ_QUIT) {
        std::cout << "Received quit command." << std::endl;
    }
    else if (buffer[0] == RSP_LISTFILES) {
        std::cout << "==========FILE LIST START==========" << std::endl;
        int offset = 1;

        uint16_t numFiles;
        memcpy(&numFiles, buffer + offset, sizeof(numFiles));
        numFiles = ntohs(numFiles);
        offset += 2;

        // Read the total list length.
        uint32_t totalListLength;
        memcpy(&totalListLength, buffer + offset, sizeof(totalListLength));
        totalListLength = ntohl(totalListLength);
        offset += 4;

        for (uint16_t i = 0; i < numFiles; i++) {
            if (offset + 4 > bytesReceived) {
                std::cerr << "Incomplete filename length." << std::endl;
                break;
            }
            uint32_t netFilenameLen;
            memcpy(&netFilenameLen, buffer + offset, sizeof(netFilenameLen));
            uint32_t filenameLen = ntohl(netFilenameLen);
            offset += 4;

            if (static_cast<size_t>(offset) + filenameLen > static_cast<size_t>(bytesReceived)) {
                std::cerr << "Incomplete filename data." << std::endl;
                break;
            }
            std::string filename(buffer + offset, filenameLen);
            offset += filenameLen;
            std::cout << filename << std::endl;
        }
        std::cout << "==========FILE LIST END==========" << std::endl;
    }
    else if (buffer[0] == RSP_DOWNLOAD)
    {
        {   // Lock response-specific variables
            std::unique_lock<std::mutex> respLock(gResponseMutex);
            gResponseBuffer.assign(buffer, buffer + bytesReceived);
            gResponseReceived = true;
            gResponseCV.notify_one();
        }
    }
    else if (buffer[0] == DOWNLOAD_ERROR)
    {
        {   // Lock response-specific variables
            std::unique_lock<std::mutex> respLock(gResponseMutex);
            gResponseBuffer.assign(buffer, buffer + bytesReceived);
            gResponseReceived = true;
            gResponseCV.notify_one();
        }
    }

}

// Receives data over a UDP socket and processes file transfer packets.
void receiveDataUDP(SOCKET udpSocket, std::atomic<bool>& running) {
    const size_t BUFFER_SIZE = 1500;
    uint8_t buffer[BUFFER_SIZE] = { 0 };
    while (running.load()) {
        int bytesReceived = recv(udpSocket, reinterpret_cast<char*>(buffer), BUFFER_SIZE, 0);
        if (bytesReceived == SOCKET_ERROR) {
            std::cerr << "UDP recv() failed with error: " << WSAGetLastError() << std::endl;
            continue;
        }
        // Validate that we received at least the header size.
        if (bytesReceived < 14) {
            std::cerr << "Incomplete UDP packet received." << std::endl;
            continue;
        }

        FileTransferPacket packet;
        memcpy(&packet.sessionId, buffer, sizeof(uint32_t));
        packet.sessionId = ntohl(packet.sessionId);
        memcpy(&packet.sequenceNumber, buffer + 4, sizeof(uint32_t));
        packet.sequenceNumber = ntohl(packet.sequenceNumber);
        memcpy(&packet.packetLength, buffer + 8, sizeof(uint32_t));
        packet.packetLength = ntohl(packet.packetLength);
        memcpy(&packet.checksum, buffer + 12, sizeof(uint16_t));
        packet.checksum = ntohs(packet.checksum);

        // The rest of the data is the payload.
        if (bytesReceived > 14) {
            packet.data.assign(buffer + 14, buffer + bytesReceived);
        }

        // Optionally verify checksum.
        if (!packet.verifyChecksum()) {
            std::cerr << "Invalid checksum in packet " << packet.sequenceNumber << std::endl;
			AckPacket ack{ packet.sessionId, packet.sequenceNumber, false };
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(gFileMutex);
            gFilePackets[packet.sessionId][packet.sequenceNumber] = packet;
            AckPacket ack{ packet.sessionId, packet.sequenceNumber, true };
            // If we have received all packets, notify waiting thread.
            if (gFilePackets[packet.sessionId].size() == gExpectedPackets[packet.sessionId]) {
                gFileCV.notify_one();
            }
        }
    }
}

// Processes user input and sends appropriate commands to the server.
void processInput(SOCKET clientSocket, const std::string& input, std::atomic<bool>& running) {
    if (input.substr(0, 2) == "/t") {
        std::string hexData = input.substr(3);
        std::vector<uint8_t> rawData = hexStringToBytes(hexData);
        send(clientSocket, reinterpret_cast<const char*>(rawData.data()), static_cast<int>(rawData.size()), 0);
    }
    else if (input.substr(0, 2) == "/l") {
        Message listFilesMessage{ REQ_LISTFILES, 0, 0, 0, {} };
        std::vector<uint8_t> buffer = listFilesMessage.serialize();
        send(clientSocket, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
    }
    else if (input.substr(0, 2) == "/d") {
        std::string inputData = input.substr(3); // Remove "/d "
        size_t spacePos = inputData.find(' ');
        if (spacePos == std::string::npos) {
            std::cerr << "Invalid download command format. Expected: /d <IP>:<Port> <filename>" << std::endl;
            return;
        }

        std::string ipPort = inputData.substr(0, spacePos);
        std::string filename = inputData.substr(spacePos + 1);

        size_t colonPos = ipPort.find(':');
        if (colonPos == std::string::npos) {
            std::cerr << "Invalid IP:Port format. Expected: <IP>:<Port>" << std::endl;
            return;
        }

        std::string ip = ipPort.substr(0, colonPos);
        std::string portStr = ipPort.substr(colonPos + 1);

        // Convert port to integer and validate it
        uint16_t port;
        try {
            port = static_cast<uint16_t>(std::stoi(portStr));
            if (port < 1024 || port > 65535) {
                std::cerr << "Invalid port number. Port must be between 1024 and 65535." << std::endl;
                return;
            }
        }
        catch (const std::invalid_argument&) {
            std::cerr << "Invalid port number. Port must be a valid integer." << std::endl;
            return;
        }
        catch (const std::out_of_range&) {
            std::cerr << "Invalid port number. Port must be between 1024 and 65535." << std::endl;
            return;
        }

        // Get the server's IP from the connected TCP socket
        sockaddr_in serverAddr{};
        int addrLen = sizeof(serverAddr);
        if (getpeername(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), &addrLen) == SOCKET_ERROR) {
            std::cerr << "getpeername() failed with error: " << WSAGetLastError() << std::endl;
            return;
        }

        // Convert server IP to string for comparison
        char serverIpStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(serverAddr.sin_addr), serverIpStr, INET_ADDRSTRLEN);

        // Check if the provided IP matches the server's IP
        if (ip != serverIpStr) {
            std::cerr << "Error: The provided IP address (" << ip << ") does not match the connected server IP ("
                << serverIpStr << ")." << std::endl;
            return;
        }

        // Get the server's UDP port from the connected UDP socket
        sockaddr_in udpClientAddr{};
        addrLen = sizeof(udpClientAddr);
        if (getsockname(UDPClientSocket, reinterpret_cast<sockaddr*>(&udpClientAddr), &addrLen) == SOCKET_ERROR) {
            std::cerr << "UDP getsockname() failed with error: " << WSAGetLastError() << std::endl;
            return;
        }
        uint16_t clientUdpPort = ntohs(udpClientAddr.sin_port);

        // Check if the provided port matches the client's UDP port
        if (port != clientUdpPort) {
            std::cerr << "Error: The provided port (" << port << ") does not match the connected client UDP port ("
                << clientUdpPort << ")." << std::endl;
            return;
        }

        // Remove whitespaces from the filename.
        const char* whitespace = " \t\n\r";
        filename.erase(0, filename.find_first_not_of(whitespace));
        filename.erase(filename.find_last_not_of(whitespace) + 1);

        // Retrieve local IP address and port from the UDP socket for download
        sockaddr_in localAddr{};
        addrLen = sizeof(localAddr);
        if (getsockname(UDPClientSocket, reinterpret_cast<sockaddr*>(&localAddr), &addrLen) == SOCKET_ERROR) {
            std::cerr << "getsockname() failed." << std::endl;
            return;
        }

        // Convert values to host order.
        uint32_t localIP = ntohl(localAddr.sin_addr.s_addr);
        uint16_t localUDPPort = ntohs(localAddr.sin_port);

        // Build the download message according to the required format.
        Message downloadMessage{
            REQ_DOWNLOAD,                  // Command ID.
            localIP,                       // IP Address in host order.
            localUDPPort,                  // Port Number in host order.
            static_cast<uint32_t>(filename.size()), // Filename length.
            {}                             // Payload, set below.
        };
        downloadMessage.text = std::vector<uint8_t>(filename.begin(), filename.end());
        std::vector<uint8_t> buffer = downloadMessage.serialize();
        // Log the received download information with a timestamp
        std::cout << "[" << getCurrentTimestamp() << "] Received download information: IP=" << ip << ", Port=" << port << ", Filename=" << filename << std::endl;

        // Reset the shared response flag:
        {
            std::unique_lock<std::mutex> respLock(gResponseMutex);
            gResponseReceived = false;
            gResponseBuffer.clear();
        }

        // Clear download buffer
        {
            std::unique_lock<std::mutex> lock(gFileMutex);
            gFilePackets.clear();
        }

        send(clientSocket, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), 0);

        // Now wait for the response:
        std::unique_lock<std::mutex> respLock(gResponseMutex);
        if (!gResponseCV.wait_for(respLock, std::chrono::seconds(5), [] { return gResponseReceived; })) {
            std::cerr << "Timeout waiting for download response." << std::endl;
            return;
        }

        uint32_t senderIP;
        uint16_t senderPort;
        uint32_t sessionID;
        uint32_t fileSize = 0;

        // Process gResponseBuffer...
        if (gResponseBuffer[0] == RSP_DOWNLOAD) {
            gResponseCV.notify_one();

            std::cout << "Parsing download response..." << std::endl;

            memcpy(&senderIP, gResponseBuffer.data() + 1, sizeof(senderIP));
            memcpy(&senderPort, gResponseBuffer.data() + 5, sizeof(senderPort));
            memcpy(&sessionID, gResponseBuffer.data() + 7, sizeof(sessionID));
            memcpy(&fileSize, gResponseBuffer.data() + 11, sizeof(fileSize));

            senderIP = ntohl(senderIP);
            senderPort = ntohs(senderPort);
            sessionID = ntohl(sessionID);
            fileSize = ntohl(fileSize);

            std::cout << "Received Download information" << std::endl;
        }

		else if (gResponseBuffer[0] == DOWNLOAD_ERROR)
		{
			std::cout << "Download error." << std::endl;
			return;
		}

        {
            gExpectedPackets[sessionID] = (fileSize + UDP_PAYLOAD_SIZE - 1) / UDP_PAYLOAD_SIZE;
            std::unique_lock<std::mutex> lock(gFileMutex);

        }

        // Timeout if download takes more than timeout duration.
        {
            std::unique_lock<std::mutex> lock(gFileMutex);
            if (!gFileCV.wait_for(lock, TIMEOUT, [sessionID] { return gFilePackets[sessionID].size() == gExpectedPackets[sessionID]; })) {
                std::cerr << "Timeout waiting for complete file transfer via UDP." << std::endl;
                return;
            }
        }

        std::filesystem::path fullPath = std::filesystem::path(directoryPath) / filename;
        std::ofstream outFile(fullPath, std::ios::binary);
        if (!outFile.is_open()) {
            std::cerr << "Failed to open file for writing: " << fullPath.string() << std::endl;
            return;
        }

        for (uint32_t i = 0; i < gExpectedPackets[sessionID]; ++i) {
            auto it = gFilePackets[sessionID].find(i);
            if (it != gFilePackets[sessionID].end()) {
                outFile.write(reinterpret_cast<const char*>(it->second.data.data()), it->second.data.size());
            }
            else {
                std::cerr << "Missing packet " << i << std::endl;
            }
        }

        outFile.close();
        gFilePackets.erase(sessionID);
        std::cout << "File downloaded successfully." << std::endl;
        // Log the successful download with a timestamp
        std::cout << "[" << getCurrentTimestamp() << "] Download successful: " << filename << std::endl;

    }
    else {
        std::cout << "Disconnection..." << std::endl;
        Message quitMessage{ REQ_QUIT, 0, 0, 0, {} };
        std::vector<uint8_t> buffer = quitMessage.serialize();
        send(clientSocket, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        running.store(false);
    }
}

//main function
int main(int argc, char** argv)
{
    constexpr uint16_t port = 2048;

    std::string host, hostTcpPort, hostUdpPort, clientUdpPort;

    std::cout << "Server IP Address: ";
    std::getline(std::cin, host);
    std::cout << "Server TCP Port Number: ";
    std::getline(std::cin, hostTcpPort);
    std::cout << "Server UDP Port Number: ";
    std::getline(std::cin, hostUdpPort);
    std::cout << "Client UDP Port Number: ";
    std::getline(std::cin, clientUdpPort);
    std::cout << "Path: ";
    std::getline(std::cin, directoryPath);


    // Check directory path
    namespace fs = std::filesystem;
    if (!fs::exists(directoryPath) || !fs::is_directory(directoryPath))
    {
        std::cerr << "Invalid directory path: " << directoryPath << std::endl;
        return RETURN_CODE_4;
    }

    // -------------------------------------------------------------------------
    // Start up Winsock, asking for version 2.2.
    WSADATA wsaData{};
    SecureZeroMemory(&wsaData, sizeof(wsaData));

    int errorCode = WSAStartup(MAKEWORD(WINSOCK_VERSION, WINSOCK_SUBVERSION), &wsaData);
    if (NO_ERROR != errorCode)
    {
        std::cerr << "WSAStartup() failed." << std::endl;
        return errorCode;
    }

    addrinfo hints{};
    SecureZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;           // IPv4
    hints.ai_socktype = SOCK_STREAM;      // Reliable delivery
    hints.ai_protocol = IPPROTO_TCP;      // TCP

    addrinfo* tcpinfo = nullptr;
    errorCode = getaddrinfo(host.c_str(), hostTcpPort.c_str(), &hints, &tcpinfo);
    if ((NO_ERROR != errorCode) || (nullptr == tcpinfo))
    {
        std::cerr << "getaddrinfo() failed." << std::endl;
        WSACleanup();
        return errorCode;
    }

    // Update hints for UDP
    hints.ai_socktype = SOCK_DGRAM;    // Datagram for UDP
    hints.ai_protocol = IPPROTO_UDP;   // UDP

    addrinfo* udpInfo = nullptr;
    errorCode = getaddrinfo(host.c_str(), hostUdpPort.c_str(), &hints, &udpInfo);
    if ((NO_ERROR != errorCode) || (nullptr == udpInfo))
    {
        std::cerr << "getaddrinfo() failed." << std::endl;
        WSACleanup();
        return errorCode;
    }


    SOCKET TCPClientSocket = socket(tcpinfo->ai_family, tcpinfo->ai_socktype, tcpinfo->ai_protocol);
    if (INVALID_SOCKET == TCPClientSocket)
    {
        std::cerr << "socket() failed." << std::endl;
        freeaddrinfo(tcpinfo);
        WSACleanup();
        return RETURN_CODE_2;
    }

    errorCode = connect(TCPClientSocket, tcpinfo->ai_addr, static_cast<int>(tcpinfo->ai_addrlen));
    if (SOCKET_ERROR == errorCode)
    {
        std::cerr << "connect() failed." << std::endl;
        freeaddrinfo(tcpinfo);
        closesocket(TCPClientSocket);
        WSACleanup();
        return RETURN_CODE_3;
    }

    // Create UDP socket.
    UDPClientSocket = socket(udpInfo->ai_family, udpInfo->ai_socktype, udpInfo->ai_protocol);
    if (INVALID_SOCKET == UDPClientSocket)
    {
        std::cerr << "UDP socket() failed." << std::endl;
        freeaddrinfo(udpInfo);
        closesocket(TCPClientSocket);
        WSACleanup();
        return RETURN_CODE_2;
    }

    // Bind UDP socket to client specified UDP port.
    sockaddr_in clientUdpAddr{};
    clientUdpAddr.sin_family = AF_INET;
    clientUdpAddr.sin_addr.s_addr = INADDR_ANY;
    clientUdpAddr.sin_port = htons(static_cast<u_short>(std::stoi(clientUdpPort)));
    errorCode = bind(UDPClientSocket, reinterpret_cast<sockaddr*>(&clientUdpAddr), sizeof(clientUdpAddr));
    if (SOCKET_ERROR == errorCode)
    {
        std::cerr << "UDP bind() failed." << std::endl;
        freeaddrinfo(udpInfo);
        closesocket(TCPClientSocket);
        closesocket(UDPClientSocket);
        WSACleanup();
        return RETURN_CODE_2;
    }

    // Connect UDP socket to the server's UDP port.
    errorCode = connect(UDPClientSocket, udpInfo->ai_addr, static_cast<int>(udpInfo->ai_addrlen));
    if (SOCKET_ERROR == errorCode)
    {
        std::cerr << "UDP connect() failed." << std::endl;
        freeaddrinfo(udpInfo);
        closesocket(TCPClientSocket);
        closesocket(UDPClientSocket);
        WSACleanup();
        return RETURN_CODE_3;
    }
    freeaddrinfo(udpInfo);
    std::cout << "UDP socket created, bound to port " << clientUdpPort << " and connected to server on port " << hostUdpPort << "." << std::endl;


    std::atomic<bool> running(true);
    std::thread listenerThread([&]() {
        while (running.load()) {
            receiveCommand(TCPClientSocket, running);
        }
        });
    std::thread udpListenerThread([&]() {
        receiveDataUDP(UDPClientSocket, running);
        });

    std::string input;
    while (true) {
        std::getline(std::cin, input);
        processInput(TCPClientSocket, input, running);
        if (running.load() == false) {
            break;
        }
    }

    running.store(false);
    if (listenerThread.joinable()) {
        listenerThread.join();
    }
    if (udpListenerThread.joinable()) {
        udpListenerThread.join();
    }

    errorCode = shutdown(TCPClientSocket, SD_SEND);
    if (SOCKET_ERROR == errorCode) {
        std::cerr << "shutdown() failed." << std::endl;
    }

    closesocket(TCPClientSocket);
    closesocket(UDPClientSocket);
    WSACleanup();
}
