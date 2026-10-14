#define NOMINMAX

#include "Network.h"

#include <iostream>
#include <syncstream>
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
    endpointHost_ = true; endpointPort_ = port; endpointIp_ = bindIp;
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

    std::osyncstream(std::cout) << "Waiting for connection on port " << port
        << " (bind " << bindIp << ")...\n" << std::flush;

    if (reconnectAttempt_ && !WaitForSocket(listenSocket, false, std::chrono::seconds(2)))
    {
        closesocket(listenSocket);
        return false;
    }
    SOCKET accepted = accept(listenSocket, nullptr, nullptr);

    closesocket(listenSocket);

    if (accepted == INVALID_SOCKET)
    {
        std::cerr << "accept failed: " << WSAGetLastError() << '\n';
        return false;
    }

    if (!AdoptSocket(accepted, true)) return false;
    std::osyncstream(std::cout) << "Client connected.\n";
    return true;
}

bool Network::Connect(const std::string& ip, uint16_t port)
{
    endpointHost_ = false; endpointPort_ = port; endpointIp_ = ip;
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

    u_long nonblocking = reconnectAttempt_ ? 1 : 0;
    if (nonblocking && ioctlsocket(candidate, FIONBIO, &nonblocking) == SOCKET_ERROR)
    {
        closesocket(candidate);
        return false;
    }
    const int result = connect(candidate, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        bool connected = false;
        if (reconnectAttempt_ && error == WSAEWOULDBLOCK &&
            WaitForSocket(candidate, true, std::chrono::seconds(2)))
        {
            int status = 0, length = sizeof(status);
            connected = getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&status), &length) == 0 && status == 0;
        }
        if (!connected)
        {
            std::cerr << "connect failed: " << error << '\n';
            closesocket(candidate);
            return false;
        }
    }
    nonblocking = 0;
    if (ioctlsocket(candidate, FIONBIO, &nonblocking) == SOCKET_ERROR)
    {
        closesocket(candidate);
        return false;
    }
    sockaddr_in local{}, remote{};
    int localSize = sizeof(local), remoteSize = sizeof(remote);
    if (getsockname(candidate, reinterpret_cast<sockaddr*>(&local), &localSize) != 0 ||
        getpeername(candidate, reinterpret_cast<sockaddr*>(&remote), &remoteSize) != 0 ||
        (local.sin_port == remote.sin_port && local.sin_addr.s_addr == remote.sin_addr.s_addr))
    {
        std::cerr << "Invalid or self-connected endpoint\n";
        closesocket(candidate);
        return false;
    }
    if (!AdoptSocket(candidate, false)) return false;
    std::osyncstream(std::cout) << "Connected to " << ip << ':' << port << '\n';
    return true;
}


bool Network::SendText(const std::string& text)
{
    return SendFrame(MessageType::Text, text);
}



void Network::ReceiveSession()
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
                StopConnection();
                std::osyncstream(std::cout) << "\nPeer disconnected.\n" << std::flush;
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
    StopConnection();
    std::lock_guard<std::mutex> lock(protocolMutex_);
    protocol_.ResetConnection();
}

void Network::StopConnection()
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
    StopConnection();
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
    if (socket_ != INVALID_SOCKET || userStopping_)
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
            StopConnection();
            return false;
        }
        offset += sent;
    }
    return true;
}
bool Network::SendFile(const std::string& path, const std::string& transferId, uint8_t mode)
{
    std::lock_guard<std::mutex> fileLock(fileSendMutex_);
    uint64_t fileSize = 0;
    OwnedHandle file;
    TransferSpec spec;
    std::string metadata;
    try {
        const std::filesystem::path source(path);
        file = OpenSource(source, fileSize);
        if (fileSize > Protocol::MAX_FILE_SIZE) throw std::runtime_error("File too large or inaccessible");
        auto fullHash = HashPrefix(file.value, fileSize, [this] { return approvalCancelled_.load(); });
        spec = { fileSize, source.filename().string(), fullHash->Hex(), {}, mode };
        spec.transferId = spec.Identity();
        if (mode && transferId != spec.transferId) throw std::runtime_error("Source changed or transferId mismatch");
        Protocol::MakeFileBegin(fileSize, spec.name); // existing safe-name and size validation
        metadata = spec.Encode();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return false; }
    std::osyncstream(std::cout) << "\n[Transfer identity] transferId=" << spec.transferId << " sha256=" << spec.digest << '\n' << std::flush;
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        if (approvalCancelled_) return false;
        id = protocol_.StartOutgoing(spec.name, spec);
    }
    if (!id) return false;
    const auto identified = Protocol::WithRequestId(id, metadata);
    if (!SendFrame(MessageType::FileRequest, identified)) return false;
    std::osyncstream(std::cout) << "\nWaiting for file approval...\n" << std::flush;
    if (!WaitForState(Protocol::SendState::AwaitAccept, Protocol::APPROVAL_TIMEOUT)) {
        std::cerr << "File rejected, cancelled, or timed out\n"; return false;
    }
    uint64_t offset;
    std::string prefix;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        if (protocol_.OutgoingState() == Protocol::SendState::Completed) {
            std::osyncstream(std::cout) << "\n[Transfer completed] " << spec.name << " [verified existing commit]\n" << std::flush;
            return true;
        }
        if (approvalCancelled_ || protocol_.OutgoingState() != Protocol::SendState::Sending) return false;
        offset = protocol_.ResumeOffset(); prefix = protocol_.ResumeDigest();
    }
    std::unique_ptr<Sha256> streamHash;
    try {
        streamHash = HashPrefix(file.value, offset, [this] {
            std::lock_guard<std::mutex> lock(protocolMutex_);
            return approvalCancelled_ || protocol_.OutgoingState() != Protocol::SendState::Sending;
        });
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        protocol_.FailOutgoing("Resume prefix scan failed or cancelled");
        std::cerr << e.what() << '\n'; return false;
    }
    if (streamHash->Hex() != prefix) {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        protocol_.FailOutgoing("Resume prefix SHA-256 mismatch"); return false;
    }
    std::osyncstream(std::cout) << "[Resume offset] " << offset << '\n' << std::flush;
    if (!SendFrame(MessageType::FileBegin, identified)) return false;
    std::array<char, FileTransfer::CHUNK_SIZE> buffer;
    uint64_t sentBytes = offset;
    FileTransfer progress;
    progress.Start(spec.name, id, fileSize, true, offset);
    FileTransfer::Print(*progress.Update(offset, true));
    while (sentBytes < fileSize) {
        const DWORD wanted = static_cast<DWORD>(std::min<uint64_t>(buffer.size(), fileSize - sentBytes));
        DWORD read = 0;
        bool readable = true;
#ifdef FILETRANSFER_TESTING
        readable = !Stage4Faults::Fail("FTA_TEST_READ_FAIL_AFTER", sentBytes);
#endif
        if (!readable || !ReadFile(file.value, buffer.data(), wanted, &read, nullptr) || read != wanted) {
            std::lock_guard<std::mutex> lock(protocolMutex_);
            protocol_.FailOutgoing("File read failed"); std::cerr << "File read failed\n"; return false;
        }
        const std::string_view chunk(buffer.data(), read);
        streamHash->Add(chunk);
        {
            std::lock_guard<std::mutex> lock(protocolMutex_);
            if (approvalCancelled_ || protocol_.OutgoingState() != Protocol::SendState::Sending) return false;
        }
        if (!SendParts(MessageType::FileChunk, id, chunk)) return false;
        sentBytes += read;
        if (auto p = progress.Update(sentBytes)) FileTransfer::Print(*p);
    }
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        if (streamHash->Hex() != spec.digest) { protocol_.FailOutgoing("Source SHA-256 mismatch"); return false; }
        if (approvalCancelled_ || !protocol_.AwaitAck()) return false;
    }
    if (fileSize) FileTransfer::Print(*progress.Update(sentBytes, true));
    if (!SendFrame(MessageType::FileEnd, Protocol::WithRequestId(id))) return false;
    std::osyncstream(std::cout) << "\n[Upload finished, waiting for ACK] " << spec.name << '\n' << std::flush;
    if (!WaitForState(Protocol::SendState::AwaitAck, Protocol::TRANSFER_TIMEOUT)) return false;
    std::osyncstream(std::cout) << "\n[Transfer completed] " << spec.name << "\n> " << std::flush;
    return true;
}
bool Network::WaitForState(Protocol::SendState waiting, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(protocolMutex_);
    while (!approvalCancelled_ && (protocol_.OutgoingState() == waiting ||
        (waiting == Protocol::SendState::AwaitAck && protocol_.OutgoingState() == Protocol::SendState::CancelPending)))
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
        (state == Protocol::SendState::Sending || state == Protocol::SendState::Completed) : state == Protocol::SendState::Completed;
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
    Protocol::IncomingFileRequest request;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        protocol_.CheckTimeouts();
        if (!protocol_.TakeFileRequest(request)) { std::osyncstream(std::cout) << "No pending file request\n"; return false; }
    }
    std::unique_ptr<TransferStorage> prepared;
    std::string error;
    if (request.spec.mode) {
        try { prepared = TransferStorage::Recover(request.spec, [this, &request] {
            std::lock_guard<std::mutex> lock(protocolMutex_);
            Protocol::IncomingFileRequest current;
            return approvalCancelled_ || !protocol_.TakeFileRequest(current) ||
                current.requestId != request.requestId || current.generation != request.generation;
        }); }
        catch (const std::exception& e) { error = e.what(); }
    }
    uint64_t id;
    std::vector<Protocol::Reply> replies;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        id = protocol_.ApproveFileRequest(request, std::move(prepared), error);
        replies = protocol_.TakeReplies();
    }
    if (!id) return false;
    return SendReplies(std::move(replies));
}
bool Network::RejectFile()
{
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        protocol_.CheckTimeouts();
        id = protocol_.RejectFileRequest();
    }
    if (!id) { std::osyncstream(std::cout) << "No pending file request\n"; return false; }
    return SendFrame(MessageType::FileReject, Protocol::WithRequestId(id));
}

bool Network::CancelFile(uint64_t requestId)
{
    bool cancelled;
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        cancelled = protocol_.Cancel(requestId);
    }
    approvalCv_.notify_all();
    // The receiver drains the bounded control queue on its next poll. Do not
    // block the console on sendMutex_ while a file frame is in flight.
    if (!cancelled) std::osyncstream(std::cout) << "No matching active transfer\n" << std::flush;
    return cancelled;
}

void Network::CancelPendingApproval()
{
    approvalCancelled_ = true;
    approvalCv_.notify_all();
}

bool Network::IsConnected()
{
    std::lock_guard<std::mutex> lock(socketMutex_);
    return !stopping_ && socket_ != INVALID_SOCKET;
}

void Network::RequestStop()
{
    userStopping_ = true;
    retryCv_.notify_all();
    StopConnection();
}

bool Network::WaitForSocket(SOCKET socket, bool writable, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!userStopping_ && std::chrono::steady_clock::now() < deadline)
    {
        fd_set ready, failed;
        FD_ZERO(&ready); FD_ZERO(&failed);
        FD_SET(socket, &ready); FD_SET(socket, &failed);
        timeval wait{ 0, 250000 };
        const int result = select(0, writable ? nullptr : &ready,
            writable ? &ready : nullptr, &failed, &wait);
        if (result == SOCKET_ERROR || FD_ISSET(socket, &failed)) return false;
        if (result > 0 && FD_ISSET(socket, &ready)) return true;
    }
    return false;
}

bool Network::Reconnect()
{
    // No new socket can be adopted until an old SendFile has returned.
    std::lock_guard<std::mutex> fileLock(fileSendMutex_);
    if (userStopping_ || !endpointPort_) return false;
    Disconnect();
    {
        std::lock_guard<std::mutex> lock(protocolMutex_);
        protocol_.ResetSession();
    }
    reconnectAttempt_ = true;
    const bool host = endpointHost_;
    const auto ip = endpointIp_;
    const auto port = endpointPort_;
    const bool connected = host ? StartServer(port, ip) : Connect(ip, port);
    reconnectAttempt_ = false;
    return connected;
}

void Network::ReceiveLoop()
{
    while (!userStopping_)
    {
        ReceiveSession();
        if (!autoReconnect_ || userStopping_) break;
        bool connected = false;
        for (int attempt = 0; attempt < 3 && !userStopping_; ++attempt)
        {
            std::osyncstream(std::cout) << "\n[Reconnect attempt] " << attempt + 1 << "/3\n" << std::flush;
            std::unique_lock<std::mutex> lock(retryMutex_);
            if (retryCv_.wait_for(lock, std::chrono::milliseconds(250 << attempt),
                [this] { return userStopping_.load(); })) break;
            lock.unlock();
            if (Reconnect()) { connected = true; break; }
        }
        if (!connected)
        {
            if (!userStopping_) std::osyncstream(std::cout) << "Reconnect attempts exhausted\n" << std::flush;
            break;
        }
        std::osyncstream(std::cout) << "[Reconnected] new requestId session\n" << std::flush;
    }
}
