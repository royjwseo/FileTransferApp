
#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

enum class MessageType : uint8_t
{
    Text = 1,
    FileBegin = 2,
    FileChunk = 3,
    FileEnd = 4,
    FileAck = 5,

    FileRequest = 6,
    FileAccept = 7,
    FileReject = 8,
    FileError = 9 // ASCII failure reason; fatal until request IDs are introduced.
};


class Protocol
{
public:
    Protocol() = default;
    Protocol(const Protocol&) = delete;
    Protocol& operator=(const Protocol&) = delete;
    struct IncomingFileRequest
    {
        std::string fileName;
        uint64_t fileSize = 0;
    };

    enum class FileDecision
    {
        None,
        Accept,
        Reject
    };

    // 수신된 요청을 메인 스레드에 전달
    bool TakeFileRequest(IncomingFileRequest& out);

    // 사용자가 승인 또는 거절할 때 호출
    bool ApproveFileRequest();
    void RejectFileRequest();

    // 상대방이 보낸 승인/거절 결과 회수
    FileDecision TakeFileDecision();
private:
    bool hasPendingRequest_ = false;
    IncomingFileRequest pendingRequest_;

    bool hasApprovedRequest_ = false;
    IncomingFileRequest approvedRequest_;

    FileDecision fileDecision_ = FileDecision::None;
public:
    static constexpr size_t HEADER_SIZE = 5;
    static constexpr uint32_t MAX_PAYLOAD = 1024 * 1024;
    static constexpr uint64_t MAX_FILE_SIZE =
        100ULL * 1024 * 1024;

    static std::vector<char> MakeFrame(
        MessageType type,
        std::string_view payload);

    static std::string MakeFileBegin(
        uint64_t fileSize,
        const std::string& fileName);

    bool Feed(std::string_view bytes);
    void ResetConnection();
    std::string TakeFileError();

    std::vector<std::string> TakeCompletedFiles();

    ~Protocol();

private:
    std::vector<char> buffer_;

    std::vector<std::string> completedFiles_;

    // Windows handles, kept opaque to avoid imposing windows.h on callers.
    void* file_ = nullptr;
    void* directory_ = nullptr;
    bool ownsTemp_ = false;
    std::string fileError_;
    std::filesystem::path tempPath_;
    std::filesystem::path finalPath_;

    uint64_t expectedSize_ = 0;
    uint64_t receivedSize_ = 0;
    bool receivingFile_ = false;

    bool ProcessMessage(
        MessageType type,
        std::string_view payload);

    bool BeginFile(std::string_view payload);
    bool WriteFileChunk(std::string_view payload);
    bool EndFile();
    void AbortFile();
    bool FailFile(const char* reason);
};
