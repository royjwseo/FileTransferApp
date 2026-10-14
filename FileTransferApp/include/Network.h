
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

    bool SendFile(const std::string& path, const std::string& transferId = {}, uint8_t mode = 0);
public:
    bool AcceptFile();
    bool RejectFile();
    bool CancelFile(uint64_t requestId = 0);
    bool Reconnect();
    bool IsConnected();
    void EnableAutoReconnect(bool enabled) { autoReconnect_ = enabled; }
    bool AutoReconnectEnabled() const { return autoReconnect_; }

    void CancelPendingApproval();
private:

    bool SendFrame(
        MessageType type,
        std::string_view payload);
    bool SendParts(MessageType type, uint64_t id, std::string_view payload);

    SOCKET socket_ = INVALID_SOCKET;
    bool stopping_ = true;
    std::mutex socketMutex_;
    std::mutex receiveMutex_;
    bool AdoptSocket(SOCKET socket, bool host);
    SOCKET ConnectedSocket(); // Caller holds sendMutex_ or receiveMutex_.
    bool initialized_ = false;
    void ReceiveSession();
    void StopConnection();
    bool WaitForSocket(SOCKET socket, bool writable, std::chrono::seconds timeout);
    std::atomic<bool> userStopping_{ false }, autoReconnect_{ false };
    bool reconnectAttempt_ = false, endpointHost_ = false;
    uint16_t endpointPort_ = 0;
    std::string endpointIp_;
    std::mutex retryMutex_;
    std::condition_variable retryCv_;

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
