
#include "Network.h"

#include <iostream>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <fstream>
#include <array>
#include <algorithm>


Network::~Network()
{
    Disconnect();

    if (initialized_)
        WSACleanup();
}

bool Network::Initialize()
{
    if (initialized_) return true;
    WSADATA data{};

    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        std::cerr << "WSAStartup failed\n";
        return false;
    }

    initialized_ = true;
    return true;
}

bool Network::StartServer(uint16_t port)
{
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listenSocket == INVALID_SOCKET)
    {
        std::cerr << "socket failed: " << WSAGetLastError() << '\n';
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "bind failed: " << WSAGetLastError() << '\n';
        closesocket(listenSocket);
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "listen failed: " << WSAGetLastError() << '\n';
        closesocket(listenSocket);
        return false;
    }

    std::cout << "Waiting for connection on port " << port << "...\n";

    SOCKET accepted = accept(listenSocket, nullptr, nullptr);

    closesocket(listenSocket);

    if (accepted == INVALID_SOCKET)
    {
        std::cerr << "accept failed: " << WSAGetLastError() << '\n';
        return false;
    }

    if (!AdoptSocket(accepted)) return false;
    std::cout << "Client connected.\n";
    return true;
}

bool Network::Connect(const std::string& ip, uint16_t port)
{
    SOCKET candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (candidate == INVALID_SOCKET)
    {
        std::cerr << "socket failed: " << WSAGetLastError() << '\n';
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
    {
        std::cerr << "Invalid IP address\n";
        closesocket(candidate);
        return false;
    }

    if (connect(candidate, reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "connect failed: " << WSAGetLastError() << '\n';
        closesocket(candidate);
        return false;
    }

    if (!AdoptSocket(candidate)) return false;
    std::cout << "Connected to " << ip << ':' << port << '\n';
    return true;
}


bool Network::SendText(const std::string& text)
{
    return SendFrame(MessageType::Text, text);
}



void Network::ReceiveLoop()
{
    try
    {
        char buffer[1024];

        while (true)
        {
            int n;
            int receiveError = 0;
            {
                std::lock_guard<std::mutex> ioLock(receiveMutex_);
                const SOCKET connected = ConnectedSocket();
                if (connected == INVALID_SOCKET) break;
                n = recv(connected, buffer, sizeof(buffer), 0);
                if (n == SOCKET_ERROR) receiveError = WSAGetLastError();
            }

            if (n > 0)
            {
                std::vector<std::string> completedFiles;

                Protocol::FileDecision decision =
                    Protocol::FileDecision::None;
                bool valid = false;
                std::string fileError;

                {
                    std::lock_guard<std::mutex> lock(protocolMutex_);

                    // 수신된 바이트를 프레이밍 및 파싱
                    valid = protocol_.Feed(
                        std::string_view(buffer, static_cast<size_t>(n)));
                    fileError = protocol_.TakeFileError();

                    // 파일 저장 완료 목록
                    completedFiles = protocol_.TakeCompletedFiles();

                    // 상대방 승인 또는 거절 결과
                    decision = protocol_.TakeFileDecision();
                }

                if (!valid)
                {
                    // Send after releasing protocolMutex_, before shutdown.
                    // A failed notice still follows the normal stop/join path.
                    if (!fileError.empty()) SendFrame(MessageType::FileError, fileError);
                    std::cerr << "Protocol error\n";
                    break;
                }

                // FILE_ACCEPT / FILE_REJECT 처리
                if (decision != Protocol::FileDecision::None)
                {
                    {
                        std::lock_guard<std::mutex> lock(approvalMutex_);

                        if (waitingForApproval_)
                        {
                            approved_ =
                                decision == Protocol::FileDecision::Accept;

                            approvalReceived_ = true;
                        }
                    }

                    // 승인 대기 중인 SendFile() 깨우기
                    approvalCv_.notify_one();
                }

                // 수신 파일 저장 완료 후 ACK 전송
                for (const auto& fileName : completedFiles)
                {
                    if (!SendFrame(MessageType::FileAck, fileName))
                    {
                        std::cerr << "Failed to send ACK\n";
                        RequestStop();
                        break;
                    }
                }
            }
            else if (n == 0)
            {
                std::cout << "\nPeer disconnected.\n";
                break;
            }
            else
            {
                // A quiet connection is valid. Recheck stop periodically even
                // when shutdown does not release an already pending recv.
                if (receiveError == WSAETIMEDOUT || receiveError == WSAEWOULDBLOCK)
                    continue;
                std::cerr << "\nrecv failed: "
                    << receiveError << '\n';
                break;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Receive failed: " << e.what() << '\n';
    }
    catch (...)
    {
        std::cerr << "Receive failed: unknown exception\n";
    }
    RequestStop();
    std::lock_guard<std::mutex> lock(protocolMutex_);
    protocol_.ResetConnection();
}

void Network::RequestStop()
{
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        stopping_ = true;
        if (socket_ != INVALID_SOCKET)
            shutdown(socket_, SD_BOTH);
    }
    CancelPendingApproval();
}

void Network::Disconnect()
{
    RequestStop();
    // Never close a handle while a send/recv still uses it.
    std::scoped_lock ioLock(sendMutex_, receiveMutex_);
    std::lock_guard<std::mutex> lock(socketMutex_);
    if (socket_ != INVALID_SOCKET)
    {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

SOCKET Network::ConnectedSocket()
{
    std::lock_guard<std::mutex> lock(socketMutex_);
    return stopping_ ? INVALID_SOCKET : socket_;
}

bool Network::AdoptSocket(SOCKET connected)
{
    const DWORD receiveTimeoutMs = 250;
    const DWORD sendTimeoutMs = 2000;
    if (setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&receiveTimeoutMs), sizeof(receiveTimeoutMs)) == SOCKET_ERROR ||
        setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs)) == SOCKET_ERROR)
    {
        std::cerr << "Socket timeout setup failed: " << WSAGetLastError() << '\n';
        closesocket(connected);
        return false;
    }
    // Connection setup is performed before launching workers.
    std::lock_guard<std::mutex> lock(socketMutex_);
    if (socket_ != INVALID_SOCKET)
    {
        closesocket(connected);
        return false;
    }
    socket_ = connected;
    stopping_ = false;
    std::lock_guard<std::mutex> approvalLock(approvalMutex_);
    approvalCancelled_ = false;
    return true;
}

bool Network::SendFrame(
    MessageType type,
    std::string_view payload)
{
    std::lock_guard<std::mutex> lock(sendMutex_);

    const SOCKET connected = ConnectedSocket();
    if (connected == INVALID_SOCKET)
        return false;

    std::vector<char> frame;

    try
    {
        frame = Protocol::MakeFrame(type, payload);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return false;
    }

    size_t total = 0;

    while (total < frame.size())
    {
        int remaining = static_cast<int>(
            frame.size() - total);

        int n = send(
            connected,
            frame.data() + total,
            remaining,
            0);

        if (n == SOCKET_ERROR || n == 0)
        {
            std::cerr << "send failed: "
                << WSAGetLastError() << '\n';
            RequestStop();
            return false;
        }

        total += static_cast<size_t>(n);
    }

    return true;
}


bool Network::SendFile(const std::string& path)
{
    std::lock_guard<std::mutex> fileLock(fileSendMutex_);

    namespace fs = std::filesystem;

    constexpr size_t CHUNK_SIZE = 64 * 1024;

    std::error_code ec;
    fs::path filePath(path);

    if (!fs::is_regular_file(filePath, ec) || ec)
    {
        std::cerr << "Not a regular file\n";
        return false;
    }

    uintmax_t fileSize = fs::file_size(filePath, ec);

    if (ec || fileSize > Protocol::MAX_FILE_SIZE)
    {
        std::cerr << "File too large or inaccessible\n";
        return false;
    }

    std::ifstream file(filePath, std::ios::binary);

    if (!file)
    {
        std::cerr << "Cannot open file\n";
        return false;
    }

    // 이번 버전은 ASCII 파일명만 지원
    std::string fileName;

    try
    {
        fileName = filePath.filename().string();
    }
    catch (const std::exception&)
    {
        std::cerr << "Unsupported file name\n";
        return false;
    }

    std::string metadata;

    try
    {
        metadata = Protocol::MakeFileBegin(
            static_cast<uint64_t>(fileSize),
            fileName);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return false;
    }

    // 이 잠금은 SendFile() 함수 시작에서 획득해야 함.
// std::lock_guard<std::mutex> fileLock(fileSendMutex_);

    {
        std::lock_guard<std::mutex> lock(approvalMutex_);

        if (approvalCancelled_) return false;
        waitingForApproval_ = true;
        approvalReceived_ = false;
        approved_ = false;

    }

    // 파일 전송 요청
    if (!SendFrame(MessageType::FileRequest, metadata))
    {
        std::lock_guard<std::mutex> lock(approvalMutex_);
        waitingForApproval_ = false;
        return false;
    }

    std::cout << "\nWaiting for file approval...\n" << std::flush;

    {
        std::unique_lock<std::mutex> lock(approvalMutex_);

        bool signaled = approvalCv_.wait_for(
            lock,
            std::chrono::seconds(60),
            [this]
            {
                return approvalReceived_ || approvalCancelled_;
            });

        bool canSend =
            signaled && !approvalCancelled_ && approved_;

        waitingForApproval_ = false;

        if (!canSend)
        {
            std::cerr << "File rejected, cancelled, or timed out\n";
            return false;
        }
    }

    // 승인된 경우에만 파일 본문 전송
    if (!SendFrame(MessageType::FileBegin, metadata))
        return false;

    std::array<char, CHUNK_SIZE> buffer;
    uint64_t sentBytes = 0;

    while (sentBytes < fileSize)
    {
        size_t toRead = static_cast<size_t>(
            std::min<uint64_t>(
                CHUNK_SIZE, fileSize - sentBytes));

        file.read(buffer.data(), toRead);

        std::streamsize bytesRead = file.gcount();

        if (bytesRead != static_cast<std::streamsize>(toRead))
        {
            std::cerr << "File read failed\n";
            return false;
        }

        std::string_view chunk(
            buffer.data(),
            static_cast<size_t>(bytesRead));

        if (!SendFrame(MessageType::FileChunk, chunk))
            return false;

        sentBytes += static_cast<uint64_t>(bytesRead);

        std::cout << "\rSending: "
            << sentBytes << "/" << fileSize
            << " bytes" << std::flush;
    }

    // 파일 전송 완료 메시지
    if (!SendFrame(MessageType::FileEnd, {}))
        return false;

    std::cout << "\n[Upload finished, waiting for ACK] "
        << fileName << '\n';

    return true;
}


bool Network::AcceptFile()
{
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);

        if (!protocol_.ApproveFileRequest())
        {
            std::cout << "No pending file request\n";
            return false;
        }
    }

    return SendFrame(MessageType::FileAccept, {});
}

bool Network::RejectFile()
{
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);

        Protocol::IncomingFileRequest request;

        if (!protocol_.TakeFileRequest(request))
        {
            std::cout << "No pending file request\n";
            return false;
        }

        protocol_.RejectFileRequest();
    }

    return SendFrame(MessageType::FileReject, {});
}

void Network::CancelPendingApproval()
{
    {
        std::lock_guard<std::mutex> lock(approvalMutex_);

        approvalCancelled_ = true;
    }

    approvalCv_.notify_all();
}
