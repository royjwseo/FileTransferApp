
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
    // Set before starting workers. Setup polls with a bounded wait for GUI use.
    void SetEvents(std::shared_ptr<EventQueue> events, uint64_t session = 0) { events_ = events; eventSession_ = session; protocol_.SetEvents(std::move(events), session); }
    void EnableInterruptibleSetup() { interruptibleSetup_ = true; }

    bool StartServer(uint16_t port, const std::string& bindIp = "0.0.0.0");
    bool Connect(const std::string& ip, uint16_t port);

    bool SendText(const std::string& text);
    void ReceiveLoop();
    // May be called by any worker. Interrupts I/O without closing the handle.
    void RequestStop();
    // Call after workers have been joined. Also serializes with in-flight I/O.
    void Disconnect();

    bool SendFile(const std::string& path, const std::string& transferId = {}, uint8_t mode = 0,
        const std::atomic<bool>* preparationCancelled = nullptr);
    uint64_t OutgoingRequestId();
    Protocol::SendState OutgoingState();
public:
    bool AcceptFile(uint64_t expectedRequestId = 0, const std::filesystem::path& folder = {});
    bool RejectFile(uint64_t expectedRequestId = 0);
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
    // Serialize console approval/rejection publication with socket replacement.
    // Reconnect acquires fileSendMutex_ first; never reverse that order.
    std::mutex controlMutex_;
    bool interruptibleSetup_ = false;
    std::shared_ptr<EventQueue> events_;
    uint64_t eventSession_ = 0;
    void Event(AppEvent::Kind kind, std::string text, bool sending = false,
        uint64_t id = 0, std::string name = {}, std::string transferId = {});
    void Progress(const FileTransfer::Progress& progress);
};
