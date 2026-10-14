#pragma once
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "FileTransfer.h"

enum class MessageType : uint8_t {
    Text = 1, FileBegin = 2, FileChunk = 3, FileEnd = 4, FileAck = 5,
    FileRequest = 6, FileAccept = 7, FileReject = 8, FileError = 9
};

// All instance methods are serialized by Network::protocolMutex_.
// Keep framing and owned-handle storage together to avoid duplicated state.
class Protocol {
public:
    Protocol() = default;
    ~Protocol();
    Protocol(const Protocol&) = delete;
    Protocol& operator=(const Protocol&) = delete;
    enum class SendState { Idle, AwaitAccept, Sending, AwaitAck, Completed, Failed };
    enum class ReceiveState { Idle, PendingApproval, AwaitBegin, Receiving };
    struct IncomingFileRequest {
        std::string fileName;
        uint64_t fileSize = 0;
        uint64_t requestId = 0;
    };
    struct Reply { MessageType type; uint64_t requestId; std::string payload; };
    static constexpr uint32_t WIRE_VERSION = 4;
    static constexpr size_t HEADER_SIZE = 5;
    static constexpr uint32_t MAX_PAYLOAD = 1024 * 1024;
    static constexpr uint64_t MAX_FILE_SIZE = 1ULL << 40; // 1 TiB, includes >4 GiB.
    static constexpr size_t MAX_BUFFER_SIZE = HEADER_SIZE + MAX_PAYLOAD;
    static constexpr auto APPROVAL_TIMEOUT = std::chrono::seconds(60);
    static constexpr auto TRANSFER_TIMEOUT = std::chrono::seconds(15);
    static std::vector<char> MakeFrame(MessageType type, std::string_view payload);
    static std::string MakeFileBegin(uint64_t fileSize, const std::string& fileName);
    static std::string WithRequestId(uint64_t id, std::string_view payload = {});
    void Configure(bool host);
    uint64_t StartOutgoing(const std::string& name);
    SendState OutgoingState() const { return sendState_; }
    bool AwaitAck();
    void FailOutgoing(const char* reason);
    bool TakeFileRequest(IncomingFileRequest& out);
    uint64_t ApproveFileRequest();
    uint64_t RejectFileRequest();
    bool Feed(std::string_view bytes);
    void CheckTimeouts();
    std::vector<Reply> TakeReplies();
    std::optional<FileTransfer::Progress> TakeProgress();
    void ResetConnection();
private:
    using Clock = std::chrono::steady_clock;
    SendState sendState_ = SendState::Idle;
    ReceiveState receiveState_ = ReceiveState::Idle;
    uint64_t nextId_ = 2, outgoingId_ = 0, incomingId_ = 0, lastIncomingId_ = 0;
    bool host_ = false;
    std::string outgoingName_;
    IncomingFileRequest incoming_;
    Clock::time_point receiveDeadline_{}, sendDeadline_{};
    std::vector<Reply> replies_;
    std::vector<char> buffer_;
    FileTransfer receiveProgress_;
    std::optional<FileTransfer::Progress> pendingProgress_;
    // Only handles created by this receiver may be deleted.
    void* file_ = nullptr;
    void* directory_ = nullptr;
    bool ownsTemp_ = false;
    std::filesystem::path tempPath_, finalPath_;
    uint64_t expectedSize_ = 0, receivedSize_ = 0;
    bool receivingFile_ = false;
    bool ProcessMessage(MessageType type, std::string_view payload);
    bool BeginFile(std::string_view payload);
    bool WriteFileChunk(std::string_view payload);
    bool EndFile();
    void AbortFile();
    void ClearIncoming();
    bool FailFile(const char* reason);
};
