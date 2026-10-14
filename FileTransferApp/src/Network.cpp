#define NOMINMAX

#include "Network.h"

#include <iostream>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <fstream>
#include <array>
#include <algorithm>
#ifdef FILETRANSFER_TESTING
#include "../../tests/Stage4Faults.h"
#endif


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

bool Network::StartServer(uint16_t port, const std::string& bindIp)
{
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listenSocket == INVALID_SOCKET)
    {
        std::cerr << "socket failed: " << WSAGetLastError() << '\n';
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, bindIp.c_str(), &addr.sin_addr) != 1)
    {
        std::cerr << "Invalid bind IPv4 address\n";
        closesocket(listenSocket);
        return false;
    }
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

    std::cout << "Waiting for connection on port " << port
        << " (bind " << bindIp << ")...\n" << std::flush;

    SOCKET accepted = accept(listenSocket, nullptr, nullptr);

    closesocket(listenSocket);

    if (accepted == INVALID_SOCKET)
    {
        std::cerr << "accept failed: " << WSAGetLastError() << '\n';
        return false;
    }

    if (!AdoptSocket(accepted, true)) return false;
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

    if (!AdoptSocket(candidate, false)) return false;
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
        std::array<char, FileTransfer::CHUNK_SIZE> buffer;

        while (true)
        {
            int n;
            int receiveError = 0;
            {
                std::lock_guard<std::mutex> ioLock(receiveMutex_);
                const SOCKET connected = ConnectedSocket();
                if (connected == INVALID_SOCKET) break;
                n = recv(connected, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (n == SOCKET_ERROR) receiveError = WSAGetLastError();
            }

            if (n > 0)
            {
                bool valid;
                std::optional<FileTransfer::Progress> progress;
                std::vector<Protocol::Reply> replies;
                {
                    std::lock_guard<std::mutex> lock(protocolMutex_);
                    // Expire requests before processing new bytes as well as
                    // on recv timeouts: continuous text cannot prevent expiry.
                    protocol_.CheckTimeouts();
                    valid = protocol_.Feed(std::string_view(buffer.data(), static_cast<size_t>(n)));
                    progress = protocol_.TakeProgress();
                    replies = protocol_.TakeReplies();
                }
                approvalCv_.notify_all();
                if (progress) FileTransfer::Print(*progress);
                if (!SendReplies(std::move(replies))) break;
                if (!valid)
                {
                    std::cerr << "Protocol error\n";
                    break;
                }
            }
            else if (n == 0)
            {
                std::cout << "\nPeer disconnected.\n" << std::flush;
                break;
            }
            else
            {
                // A quiet connection is valid. Recheck stop periodically even
                // when shutdown does not release an already pending recv.
                if (receiveError == WSAETIMEDOUT || receiveError == WSAEWOULDBLOCK)
                {
                    std::vector<Protocol::Reply> replies;
                    {
                        std::lock_guard<std::mutex> lock(protocolMutex_);
                        protocol_.CheckTimeouts();
                        replies = protocol_.TakeReplies();
                    }
                    if (!SendReplies(std::move(replies))) break;
                    continue;
                }
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

bool Network::AdoptSocket(SOCKET connected, bool host)
{
    const int socketBufferBytes = 64 * 1024;
    const DWORD receiveTimeoutMs = 250;
    const DWORD sendTimeoutMs = 2000;
    if (setsockopt(connected, SOL_SOCKET, SO_SNDBUF,
        reinterpret_cast<const char*>(&socketBufferBytes), sizeof(socketBufferBytes)) == SOCKET_ERROR ||
        setsockopt(connected, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char*>(&socketBufferBytes), sizeof(socketBufferBytes)) == SOCKET_ERROR ||
        setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO,
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
    protocol_.Configure(host);
    approvalCancelled_ = false;
    return true;
}

bool Network::SendFrame(MessageType type, std::string_view payload)
{
    return SendParts(type, 0, payload);
}

bool Network::SendParts(MessageType type, uint64_t id, std::string_view payload)
{
    if (payload.size() > Protocol::MAX_PAYLOAD - (id ? 8 : 0)) return false;
    std::array<char, 13> prefix{};
    const uint32_t length = static_cast<uint32_t>(payload.size() + (id ? 8 : 0));
    prefix[0] = static_cast<char>(type);
    for (int i = 0; i < 4; ++i) prefix[1 + i] = static_cast<char>(length >> ((3 - i) * 8));
    if (id) for (int i = 0; i < 8; ++i) prefix[5 + i] = static_cast<char>(id >> ((7 - i) * 8));
    const size_t prefixSize = id ? 13 : 5;
    const size_t totalSize = prefixSize + payload.size();
    std::lock_guard<std::mutex> lock(sendMutex_);
    const SOCKET connected = ConnectedSocket();
    if (connected == INVALID_SOCKET) return false;
    size_t offset = 0;
    while (offset < totalSize)
    {
        WSABUF parts[2]{};
        DWORD count = 0;
        if (offset < prefixSize)
        {
            parts[count++] = { static_cast<ULONG>(prefixSize - offset), prefix.data() + offset };
            if (!payload.empty()) parts[count++] = {
                static_cast<ULONG>(payload.size()), const_cast<char*>(payload.data()) };
        }
        else parts[count++] = { static_cast<ULONG>(totalSize - offset),
            const_cast<char*>(payload.data()) + (offset - prefixSize) };
#ifdef FILETRANSFER_TESTING
        // Force multiple actual socket writes, including a split header/ID.
        const auto slice = Stage4Faults::Value("FTA_TEST_SEND_SLICE", UINT64_MAX);
        if (slice < parts[0].len) { parts[0].len = static_cast<ULONG>(slice); count = 1; }
        else if (count == 2 && slice < uint64_t(parts[0].len) + parts[1].len)
            parts[1].len = static_cast<ULONG>(slice - parts[0].len);
#endif
        DWORD sent = 0;
        if (WSASend(connected, parts, count, &sent, 0, nullptr, nullptr) == SOCKET_ERROR ||
            sent == 0 || sent > totalSize - offset)
        {
            std::cerr << "send failed: " << WSAGetLastError() << '\n';
            RequestStop();
            return false;
        }
        offset += sent;
    }
    return true;
}
bool Network::SendFile(const std::string& path)
{
    std::lock_guard<std::mutex> fileLock(fileSendMutex_);

    namespace fs = std::filesystem;

    constexpr size_t CHUNK_SIZE = FileTransfer::CHUNK_SIZE;

    std::error_code ec;
    fs::path filePath(path);

    if (!fs::is_regular_file(filePath, ec) || ec)
    {
        std::cerr << "Not a regular file\n";
        return false;
    }

    const uint64_t fileSize = fs::file_size(filePath, ec);

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

    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        if (approvalCancelled_) return false;
        id = protocol_.StartOutgoing(fileName);
    }
    if (!id) return false;
    const auto identifiedMetadata = Protocol::WithRequestId(id, metadata);
    if (!SendFrame(MessageType::FileRequest, identifiedMetadata)) return false;
    std::cout << "\nWaiting for file approval...\n" << std::flush;
    if (!WaitForState(Protocol::SendState::AwaitAccept, Protocol::APPROVAL_TIMEOUT))
    {
        std::cerr << "File rejected, cancelled, or timed out\n";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        if (protocol_.OutgoingState() != Protocol::SendState::Sending || approvalCancelled_)
            return false;
    }
    if (!SendFrame(MessageType::FileBegin, identifiedMetadata)) return false;
    std::array<char, CHUNK_SIZE> buffer;
    uint64_t sentBytes = 0;
    FileTransfer progress;
    progress.Start(fileName, id, fileSize, true);
    FileTransfer::Print(*progress.Update(0, true));

    while (sentBytes < fileSize)
    {
        size_t toRead = static_cast<size_t>(
            std::min<uint64_t>(
                CHUNK_SIZE, fileSize - sentBytes));

#ifdef FILETRANSFER_TESTING
        if (Stage4Faults::Fail("FTA_TEST_READ_FAIL_AFTER", sentBytes)) file.setstate(std::ios::badbit);
#endif
        file.read(buffer.data(), static_cast<std::streamsize>(toRead));

        std::streamsize bytesRead = file.gcount();

        if (bytesRead != static_cast<std::streamsize>(toRead))
        {
            {
                std::lock_guard<std::mutex> lock(protocolMutex_);
                protocol_.FailOutgoing("File read failed");
            }
            std::cerr << "File read failed\n";
            return false;
        }

        std::string_view chunk(
            buffer.data(),
            static_cast<size_t>(bytesRead));

        {
            std::lock_guard<std::mutex> lock(protocolMutex_);
            if (approvalCancelled_ || protocol_.OutgoingState() != Protocol::SendState::Sending)
                return false;
        }
        // Never hold the state lock during socket I/O: the peer may be sending
        // concurrently and needs our receiver to drain the other direction.
        if (!SendParts(MessageType::FileChunk, id, chunk))
            return false;

        sentBytes += static_cast<uint64_t>(bytesRead);

        if (auto p = progress.Update(sentBytes)) FileTransfer::Print(*p);
    }

    {
        // Publish ACK-wait before END so an immediate response cannot be lost.
        // No state lock is held during I/O, including the small END frame.
        std::lock_guard<std::mutex> lock(protocolMutex_);
        if (approvalCancelled_ || !protocol_.AwaitAck()) return false;
    }
    if (sentBytes != 0) FileTransfer::Print(*progress.Update(sentBytes, true));
    if (!SendFrame(MessageType::FileEnd, Protocol::WithRequestId(id))) return false;
    std::cout << "\n[Upload finished, waiting for ACK] " << fileName << '\n' << std::flush;
    if (!WaitForState(Protocol::SendState::AwaitAck, Protocol::TRANSFER_TIMEOUT)) return false;
    std::cout << "\n[Transfer completed] " << fileName << "\n> " << std::flush;
    return true;
}

bool Network::WaitForState(Protocol::SendState waiting, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(protocolMutex_);
    while (!approvalCancelled_ && protocol_.OutgoingState() == waiting)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            protocol_.FailOutgoing(waiting == Protocol::SendState::AwaitAccept ?
                "Approval timed out" : "ACK timed out");
            auto replies = protocol_.TakeReplies();
            lock.unlock();
            SendReplies(std::move(replies));
            return false;
        }
        // Stop notification does not need the state lock (SendFrame may hold it).
        // Bound the possible notification race to the receive polling interval.
        approvalCv_.wait_until(lock, std::min(deadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250)));
    }
    const auto state = protocol_.OutgoingState();
    // A subsequent FIN/stop cannot undo a matching storage ACK already parsed.
    // SendFile reaches this wait only after END was successfully sent.
    if (waiting == Protocol::SendState::AwaitAck && state == Protocol::SendState::Completed)
        return true;
    if (approvalCancelled_) return false;
    return waiting == Protocol::SendState::AwaitAccept ?
        state == Protocol::SendState::Sending : state == Protocol::SendState::Completed;
}

bool Network::SendReplies(std::vector<Protocol::Reply> replies)
{
    for (const auto& reply : replies)
        if (!SendFrame(reply.type, Protocol::WithRequestId(reply.requestId, reply.payload)))
            return false;
    return true;
}

bool Network::AcceptFile()
{
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        protocol_.CheckTimeouts();
        id = protocol_.ApproveFileRequest();
    }
    if (!id) { std::cout << "No pending file request\n"; return false; }
    return SendFrame(MessageType::FileAccept, Protocol::WithRequestId(id));
}

bool Network::RejectFile()
{
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        protocol_.CheckTimeouts();
        id = protocol_.RejectFileRequest();
    }
    if (!id) { std::cout << "No pending file request\n"; return false; }
    return SendFrame(MessageType::FileReject, Protocol::WithRequestId(id));
}

void Network::CancelPendingApproval()
{
    approvalCancelled_ = true;
    approvalCv_.notify_all();
}
