
#include "Protocol.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <system_error>
#ifdef FILETRANSFER_TESTING
#include "../../tests/Stage4Faults.h"
#endif

namespace fs = std::filesystem;

namespace
{
    // 실습용 안전한 파일명 검사
    bool IsSafeFileName(std::string_view name)
    {
        if (name.empty() || name.size() > 100)
            return false;

        if (name.front() == '.' || name.back() == '.')
            return false;

        for (unsigned char ch : name)
        {
            bool allowed =
                (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '.' || ch == '_' || ch == '-';

            if (!allowed)
                return false;
        }

        std::string base(name.substr(0, name.find('.')));
        std::transform(base.begin(), base.end(), base.begin(),
            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            (base.size() == 4 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") &&
                base[3] >= '1' && base[3] <= '9'))
            return false;
        return true;
    }

    void AppendU64(std::string& out, uint64_t value)
    {
        for (int i = 7; i >= 0; --i)
            out.push_back(
                static_cast<char>((value >> (i * 8)) & 0xFF));
    }

    uint64_t ReadU64(std::string_view data)
    {
        uint64_t value = 0;

        for (int i = 0; i < 8; ++i)
        {
            value = (value << 8) |
                static_cast<unsigned char>(data[i]);
        }

        return value;
    }
}

std::vector<char> Protocol::MakeFrame(
    MessageType type,
    std::string_view payload)
{
    if (payload.size() > MAX_PAYLOAD)
        throw std::length_error("Payload too large");

    uint32_t len = static_cast<uint32_t>(payload.size());

    std::vector<char> frame;
    frame.reserve(HEADER_SIZE + payload.size());

    frame.push_back(static_cast<char>(type));
    frame.push_back(static_cast<char>((len >> 24) & 0xFF));
    frame.push_back(static_cast<char>((len >> 16) & 0xFF));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));

    frame.insert(
        frame.end(), payload.begin(), payload.end());

    return frame;
}

std::string Protocol::MakeFileBegin(
    uint64_t fileSize,
    const std::string& fileName)
{
    if (fileSize > MAX_FILE_SIZE ||
        !IsSafeFileName(fileName))
        throw std::invalid_argument("Invalid file metadata");

    std::string payload;
    payload.reserve(8 + fileName.size());

    AppendU64(payload, fileSize);
    payload += fileName;

    return payload;
}

std::string Protocol::WithRequestId(uint64_t id, std::string_view payload)
{
    if (!id) throw std::invalid_argument("Zero request ID");
    std::string result;
    AppendU64(result, id);
    result.append(payload);
    return result;
}

void Protocol::Configure(bool host)
{
    host_ = host;
    nextId_ = host ? 1 : 2;
}

uint64_t Protocol::StartOutgoing(const std::string& name)
{
    if (sendState_ == SendState::AwaitAccept || sendState_ == SendState::Sending ||
        sendState_ == SendState::AwaitAck || nextId_ == 0) return 0;
    outgoingId_ = nextId_;
    // Never wrap and reuse IDs on this connection.
    nextId_ = nextId_ > UINT64_MAX - 2 ? 0 : nextId_ + 2;
    outgoingName_ = name;
    sendState_ = SendState::AwaitAccept;
    sendDeadline_ = Clock::now() + APPROVAL_TIMEOUT;
    return outgoingId_;
}

bool Protocol::AwaitAck()
{
    if (sendState_ != SendState::Sending) return false;
    sendState_ = SendState::AwaitAck;
    sendDeadline_ = Clock::now() + TRANSFER_TIMEOUT;
    return true;
}

void Protocol::FailOutgoing(const char* reason)
{
    if (sendState_ == SendState::AwaitAccept || sendState_ == SendState::Sending ||
        sendState_ == SendState::AwaitAck)
    {
        sendState_ = SendState::Failed;
        replies_.push_back({ MessageType::FileError, outgoingId_, reason });
    }
}

bool Protocol::TakeFileRequest(IncomingFileRequest& out)
{
    if (receiveState_ != ReceiveState::PendingApproval) return false;
    out = incoming_;
    return true;
}

uint64_t Protocol::ApproveFileRequest()
{
    if (receiveState_ != ReceiveState::PendingApproval) return 0;
    receiveState_ = ReceiveState::AwaitBegin;
    receiveDeadline_ = Clock::now() + TRANSFER_TIMEOUT;
    return incomingId_;
}

uint64_t Protocol::RejectFileRequest()
{
    if (receiveState_ != ReceiveState::PendingApproval) return 0;
    const auto id = incomingId_;
    ClearIncoming();
    return id;
}

std::vector<Protocol::Reply> Protocol::TakeReplies()
{
    std::vector<Reply> result;
    result.swap(replies_);
    return result;
}

void Protocol::CheckTimeouts()
{
    if ((sendState_ == SendState::AwaitAccept || sendState_ == SendState::AwaitAck) &&
        Clock::now() >= sendDeadline_)
        FailOutgoing(sendState_ == SendState::AwaitAccept ? "Approval timed out" : "ACK timed out");
    if (receiveState_ != ReceiveState::Idle && Clock::now() >= receiveDeadline_)
    {
        replies_.push_back({ MessageType::FileError, incomingId_, "Receive request timed out" });
        std::cerr << "Receive request timed out\n" << std::flush;
        ClearIncoming();
    }
}

std::optional<FileTransfer::Progress> Protocol::TakeProgress()
{
    auto result = std::move(pendingProgress_);
    pendingProgress_.reset();
    return result;
}

bool Protocol::Feed(std::string_view bytes)
{
    // Accumulate exactly one frame; even a caller supplying many frames cannot
    // grow the buffer with the complete input. Capacity is reserved only once.
    if (buffer_.capacity() < MAX_BUFFER_SIZE) buffer_.reserve(MAX_BUFFER_SIZE);
    while (!bytes.empty())
    {
        if (buffer_.size() < HEADER_SIZE)
        {
            const auto n = std::min(HEADER_SIZE - buffer_.size(), bytes.size());
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + n);
            bytes.remove_prefix(n);
            if (buffer_.size() < HEADER_SIZE) break;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(buffer_.data());
        const uint32_t length = (uint32_t(p[1]) << 24) | (uint32_t(p[2]) << 16) |
            (uint32_t(p[3]) << 8) | uint32_t(p[4]);
        if (length > MAX_PAYLOAD) return false;
        const size_t frameSize = HEADER_SIZE + size_t(length);
        const auto n = std::min(frameSize - buffer_.size(), bytes.size());
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + n);
        bytes.remove_prefix(n);
        if (buffer_.size() < frameSize) break;
        if (!ProcessMessage(static_cast<MessageType>(p[0]),
            std::string_view(buffer_.data() + HEADER_SIZE, length))) return false;
        buffer_.clear();
    }
    return true;
}
bool Protocol::ProcessMessage(MessageType type, std::string_view payload)
{
    if (type == MessageType::Text)
    {
        std::cout << "\n[Received TEXT] ";
        std::cout.write(payload.data(), payload.size());
        std::cout << "\n> " << std::flush;
        return true;
    }
    if (type < MessageType::FileBegin || type > MessageType::FileError) return false;
    // IDs are mandatory for every file message; old wire format is not accepted.
    if (payload.size() < 8) return false;
    const uint64_t id = ReadU64(payload.substr(0, 8));
    payload.remove_prefix(8);
    if (!id) return false;

    if (type == MessageType::FileAccept || type == MessageType::FileReject ||
        type == MessageType::FileAck || type == MessageType::FileError)
    {
        const bool error = type == MessageType::FileError;
        if (error)
        {
            if (payload.empty() || payload.size() > 256 ||
                std::any_of(payload.begin(), payload.end(),
                    [](unsigned char c) { return c < 32 || c > 126; })) return false;
            if (id == incomingId_ && receiveState_ != ReceiveState::Idle)
            {
                std::cerr << "\n[Peer file error] " << payload << '\n' << std::flush;
                ClearIncoming();
                return true; // Never reply to FILE_ERROR.
            }
        }
        else if (!payload.empty()) return false;

        const bool active = sendState_ == SendState::AwaitAccept ||
            sendState_ == SendState::Sending || sendState_ == SendState::AwaitAck;
        if (id != outgoingId_ || !active)
        {
            std::cout << "\n[Ignored stale file response] " << id << '\n' << std::flush;
            return true;
        }
        if (error)
        {
            std::cerr << "\n[Peer file error] " << payload << '\n' << std::flush;
            sendState_ = SendState::Failed;
            return true;
        }
        if (type == MessageType::FileAccept && sendState_ == SendState::AwaitAccept)
            sendState_ = SendState::Sending;
        else if (type == MessageType::FileReject && sendState_ == SendState::AwaitAccept)
            sendState_ = SendState::Failed;
        else if (type == MessageType::FileAck && sendState_ == SendState::AwaitAck)
        {
            sendState_ = SendState::Completed;
        }
        else
        {
            // Same ID is insufficient: duplicate ACCEPT/early ACK/late REJECT
            // in an active transfer is a protocol violation.
            FailOutgoing("Invalid sender state transition");
            return false;
        }
        return true;
    }

    if (type == MessageType::FileRequest)
    {
        // The originator owns a parity namespace. A monotonic watermark prevents
        // replay without retaining an unbounded set of completed IDs.
        if ((id & 1) == (host_ ? 1ULL : 0ULL) || id <= lastIncomingId_)
        {
            replies_.push_back({ MessageType::FileError, id, "Invalid or replayed request ID" });
            return false;
        }
        if (receiveState_ != ReceiveState::Idle)
        {
            replies_.push_back({ MessageType::FileError, id, "Receiver busy" });
            return false;
        }
        incomingId_ = id;
        lastIncomingId_ = id;
        if (payload.size() < 9) return FailFile("Invalid file metadata");
        const auto size = ReadU64(payload.substr(0, 8));
        const std::string name(payload.substr(8));
        if (size > MAX_FILE_SIZE || !IsSafeFileName(name)) return FailFile("Invalid file metadata");
        incoming_ = { name, size, id };
        receiveState_ = ReceiveState::PendingApproval;
        receiveDeadline_ = Clock::now() + APPROVAL_TIMEOUT;
        std::cout << "\n[Incoming file request] " << name << " (" << size
            << " bytes)\nType /accept or /reject\n> " << std::flush;
        return true;
    }

    if (id != incomingId_ || receiveState_ == ReceiveState::Idle)
    {
        replies_.push_back({ MessageType::FileError, id, "No matching active receive request" });
        return false;
    }
    bool valid = false;
    switch (type)
    {
    case MessageType::FileBegin:
        if (receiveState_ != ReceiveState::AwaitBegin) return FailFile("Unexpected FILE_BEGIN");
        valid = BeginFile(payload);
        if (valid) receiveState_ = ReceiveState::Receiving;
        break;
    case MessageType::FileChunk:
        if (receiveState_ != ReceiveState::Receiving || payload.empty())
            return FailFile("Unexpected FILE_CHUNK");
        valid = WriteFileChunk(payload);
        break;
    case MessageType::FileEnd:
        if (receiveState_ != ReceiveState::Receiving || !payload.empty())
            return FailFile("Unexpected FILE_END");
        valid = EndFile();
        if (valid)
        {
            replies_.push_back({ MessageType::FileAck, id, {} });
            ClearIncoming();
        }
        break;
    default: return false;
    }
    if (valid && receiveState_ != ReceiveState::Idle)
        receiveDeadline_ = Clock::now() + TRANSFER_TIMEOUT;
    return valid;
}

bool Protocol::BeginFile(std::string_view payload)
{
    // All remote-controlled state and metadata checks precede any disk change.
    if (receivingFile_ || file_ || ownsTemp_ || receiveState_ != ReceiveState::AwaitBegin)
        return FailFile("File transfer not approved or receiver busy");
    if (payload.size() < 9)
        return FailFile("Invalid file metadata");
    const uint64_t fileSize = ReadU64(payload.substr(0, 8));
    const std::string fileName(payload.substr(8));
    if (fileSize > MAX_FILE_SIZE || !IsSafeFileName(fileName))
        return FailFile("Invalid file metadata");
    if (fileSize != incoming_.fileSize || fileName != incoming_.fileName)
        return FailFile("File metadata mismatch");

    std::error_code ec;
    const fs::path folder = fs::absolute("received", ec);
    if (ec) return FailFile("Cannot resolve receive directory");
    fs::create_directories(folder, ec);
    if (ec) return FailFile("Cannot create receive directory");

    // Pin the directory against rename/deletion and reject redirected folders.
    HANDLE directory = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return FailFile("Cannot open receive directory");
    directory_ = directory;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(directory, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return FailFile("Unsafe receive directory");

    finalPath_ = folder / fileName;
    tempPath_ = finalPath_;
    tempPath_ += ".part";
    // symlink_status also detects dangling links; errors fail closed.
    const auto finalStatus = fs::symlink_status(finalPath_, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return FailFile("Cannot inspect destination");
    if (fs::exists(finalStatus)) return FailFile("Destination already exists");

    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(folder.c_str(), &available, nullptr, nullptr))
        return FailFile("Cannot inspect available disk space");
#ifdef FILETRANSFER_TESTING
    available.QuadPart = Stage4Faults::Value("FTA_TEST_FREE_BYTES", available.QuadPart);
#endif
    if (fileSize > available.QuadPart) return FailFile("Insufficient disk space");
    // CREATE_NEW atomically refuses existing files/links, including .part files.
    HANDLE created = CreateFileW(tempPath_.c_str(), GENERIC_WRITE | DELETE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE)
        return FailFile("Cannot create temporary file or temporary file exists");
    file_ = created;
    ownsTemp_ = true;
    expectedSize_ = fileSize;
    receivedSize_ = 0;
    receivingFile_ = true;
    receiveProgress_.Start(fileName, incomingId_, expectedSize_, false);
    pendingProgress_ = receiveProgress_.Update(0, true);
    std::cout << "\n[Receiving] " << fileName
        << " (" << expectedSize_ << " bytes)\n" << std::flush;
    return true;
}

bool Protocol::WriteFileChunk(std::string_view payload)
{
    if (!receivingFile_ || !file_ || !ownsTemp_)
        return FailFile("File data received without active approved transfer");
    if (receivedSize_ > expectedSize_ || payload.size() > expectedSize_ - receivedSize_)
        return FailFile("File data exceeds approved size");
    size_t offset = 0;
    while (offset < payload.size())
    {
        DWORD count = static_cast<DWORD>(payload.size() - offset);
#ifdef FILETRANSFER_TESTING
        if (Stage4Faults::Fail("FTA_TEST_WRITE_FAIL_AFTER", receivedSize_))
        {
            SetLastError(ERROR_DISK_FULL);
            return FailFile("File write failed");
        }
        count = static_cast<DWORD>(std::min<uint64_t>(count,
            Stage4Faults::Value("FTA_TEST_WRITE_SLICE", count)));
#endif
        DWORD written = 0;
        if (!WriteFile(file_, payload.data() + offset, count, &written, nullptr) ||
            written == 0 || written > count) return FailFile("File write failed");
        offset += written;
        receivedSize_ += written; // subtraction check above proves no overflow.
    }
    if (auto p = receiveProgress_.Update(receivedSize_)) pendingProgress_ = std::move(p);    return true;
}

bool Protocol::EndFile()
{
    if (!receivingFile_ || !file_ || !ownsTemp_ || receivedSize_ != expectedSize_)
        return FailFile("File ended without matching approved size");
    pendingProgress_ = receiveProgress_.Update(receivedSize_, true);
#ifdef FILETRANSFER_TESTING
    if (Stage4Faults::Value("FTA_TEST_FLUSH_FAIL", 0)) return FailFile("File flush failed");
#endif
    if (!FlushFileBuffers(file_)) return FailFile("File flush failed");

    // Rename the owned handle atomically, with no replacement of a destination
    // created by another process since BeginFile. Keep exclusive ownership open.
    const std::wstring destination = finalPath_.wstring();
    const DWORD nameBytes = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    const size_t bufferSize = sizeof(FILE_RENAME_INFO) + nameBytes;
    std::vector<unsigned char> storage(bufferSize);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = nameBytes;
    std::memcpy(rename->FileName, destination.data(), nameBytes);
    if (!SetFileInformationByHandle(file_, FileRenameInfo, rename, static_cast<DWORD>(bufferSize)))
        return FailFile("Cannot commit file; destination exists or rename failed");

    ownsTemp_ = false;
    CloseHandle(file_);
    file_ = nullptr;
    CloseHandle(directory_);
    directory_ = nullptr;
    receivingFile_ = false;

    std::cout << "\n[File saved] " << finalPath_.string() << "\n> " << std::flush;
    return true;
}

void Protocol::AbortFile()
{
    if (file_)
    {
        if (ownsTemp_)
        {
            // Delete the object we created, never an unrelated file at a path.
            FILE_DISPOSITION_INFO disposition{ TRUE };
            if (!SetFileInformationByHandle(file_, FileDispositionInfo,
                &disposition, sizeof(disposition)))
                std::cerr << "Cannot delete owned temporary file: " << GetLastError() << '\n';
        }
        CloseHandle(file_);
        file_ = nullptr;
    }
    if (directory_) { CloseHandle(directory_); directory_ = nullptr; }
    ownsTemp_ = false;
    receivingFile_ = false;
    expectedSize_ = receivedSize_ = 0;
    tempPath_.clear();
    finalPath_.clear();
}

void Protocol::ClearIncoming()
{
    AbortFile();
    receiveState_ = ReceiveState::Idle;
    incomingId_ = 0;
    incoming_ = {};
}

bool Protocol::FailFile(const char* reason)
{
    if (incomingId_) replies_.push_back({ MessageType::FileError, incomingId_, reason });
    ClearIncoming();
    std::cerr << "File receive failed: " << reason << '\n' << std::flush;
    return false;
}

Protocol::~Protocol() { AbortFile(); }

void Protocol::ResetConnection()
{
    ClearIncoming();
    if (sendState_ != SendState::Completed) sendState_ = SendState::Failed;
    pendingProgress_.reset();
    replies_.clear();
    buffer_.clear();
}
