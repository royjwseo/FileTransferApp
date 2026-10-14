
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>
#include <atomic>
#include <cstdint>
#include <string>
#include <condition_variable>
#include <chrono>
#include "Protocol.h"
#pragma comment(lib, "Ws2_32.lib")

class Network
{
public:
    Network() = default;
    ~Network();

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    bool Initialize();

    bool StartServer(uint16_t port, const std::string& bindIp = "0.0.0.0");
    bool Connect(const std::string& ip, uint16_t port);

    bool SendText(const std::string& text);
    void ReceiveLoop();
    // May be called by any worker. Interrupts I/O without closing the handle.
    void RequestStop();
    // Call after workers have been joined. Also serializes with in-flight I/O.
    void Disconnect();

    bool SendFile(const std::string& path);
public:
    bool AcceptFile();
    bool RejectFile();

    void CancelPendingApproval();
private:

    bool SendFrame(
        MessageType type,
        std::string_view payload);

    SOCKET socket_ = INVALID_SOCKET;
    bool stopping_ = true;
    std::mutex socketMutex_;
    std::mutex receiveMutex_;
    bool AdoptSocket(SOCKET socket, bool host);
    SOCKET ConnectedSocket(); // Caller holds sendMutex_ or receiveMutex_.
    bool initialized_ = false;

    Protocol protocol_;

    std::mutex sendMutex_;
private:
    // Protocol 상태 접근 보호
    std::mutex protocolMutex_;

    // State lives in Protocol under protocolMutex_. Cancellation is atomic so
    // SendFrame can request stop while the state lock is held.
    std::condition_variable approvalCv_;
    std::atomic<bool> approvalCancelled_{ true };
    bool SendReplies(std::vector<Protocol::Reply> replies);
    bool WaitForState(Protocol::SendState waiting, std::chrono::seconds timeout);
    // 송신 중 중복 파일 요청 방지
    std::mutex fileSendMutex_;
};
