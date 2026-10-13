
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

std::vector<std::string> Protocol::TakeCompletedFiles()
{
    std::vector<std::string> result;
    result.swap(completedFiles_);
    return result;
}

bool Protocol::TakeFileRequest(IncomingFileRequest& out)
{
    if (!hasPendingRequest_)
        return false;

    out = pendingRequest_;
    return true;
}

bool Protocol::ApproveFileRequest()
{
    if (!hasPendingRequest_ || hasApprovedRequest_)
        return false;

    approvedRequest_ = pendingRequest_;
    hasApprovedRequest_ = true;
    hasPendingRequest_ = false;

    return true;
}

void Protocol::RejectFileRequest()
{
    hasPendingRequest_ = false;
    pendingRequest_ = {};
}

Protocol::FileDecision Protocol::TakeFileDecision()
{
    FileDecision result = fileDecision_;
    fileDecision_ = FileDecision::None;
    return result;
}

bool Protocol::Feed(std::string_view bytes)
{
    buffer_.insert(
        buffer_.end(), bytes.begin(), bytes.end());

    size_t offset = 0;

    while (buffer_.size() - offset >= HEADER_SIZE)
    {
        const auto* p =
            reinterpret_cast<const unsigned char*>(
                buffer_.data() + offset);

        MessageType type = static_cast<MessageType>(p[0]);

        uint32_t length =
            (uint32_t(p[1]) << 24) |
            (uint32_t(p[2]) << 16) |
            (uint32_t(p[3]) << 8) |
            uint32_t(p[4]);

        if (length > MAX_PAYLOAD)
            return false;

        size_t frameSize = HEADER_SIZE + size_t(length);

        if (buffer_.size() - offset < frameSize)
            break;

        std::string_view payload(
            buffer_.data() + offset + HEADER_SIZE,
            length);

        if (!ProcessMessage(type, payload))
            return false;

        offset += frameSize;
    }

    if (offset > 0)
    {
        buffer_.erase(
            buffer_.begin(),
            buffer_.begin() + offset);
    }

    return true;
}

bool Protocol::ProcessMessage(
    MessageType type,
    std::string_view payload)
{
    switch (type)
    {
    case MessageType::Text:
        std::cout << "\n[Received TEXT] ";
        std::cout.write(payload.data(), payload.size());
        std::cout << "\n> " << std::flush;
        return true;

    case MessageType::FileBegin:
        return BeginFile(payload);

    case MessageType::FileChunk:
        return WriteFileChunk(payload);

    case MessageType::FileEnd:
        return payload.empty() ? EndFile() : FailFile("Invalid FILE_END payload");

    case MessageType::FileError:
        if (payload.empty() || payload.size() > 256 ||
            std::any_of(payload.begin(), payload.end(),
                [](unsigned char c) { return c < 32 || c > 126; }))
            return false;
        std::cerr << "\n[Peer file error] " << payload << '\n';
        // No request IDs yet: terminate rather than misattribute the failure.
        return false;

    case MessageType::FileAck:
    {
        std::cout << "\n[Transfer completed] ";

        std::cout.write(
            payload.data(),
            static_cast<std::streamsize>(payload.size()));

        std::cout << "\n> " << std::flush;
        return true;
    }
    case MessageType::FileRequest:
    {
        if (receivingFile_ ||
            hasPendingRequest_ ||
            hasApprovedRequest_ ||
            payload.size() < 9)
            return FailFile("Invalid file request or receiver busy");

        uint64_t fileSize = ReadU64(payload.substr(0, 8));
        std::string fileName(payload.substr(8));

        if (fileSize > MAX_FILE_SIZE ||
            !IsSafeFileName(fileName))
            return FailFile("Invalid file metadata");

        pendingRequest_ = { fileName, fileSize };
        hasPendingRequest_ = true;

        std::cout << "\n[Incoming file request] "
            << fileName << " (" << fileSize << " bytes)\n"
            << "Type /accept or /reject\n> "
            << std::flush;

        return true;
    }

    case MessageType::FileAccept:
        if (!payload.empty())
            return false;
        fileDecision_ = FileDecision::Accept;
        return true;

    case MessageType::FileReject:
        if (!payload.empty())
            return false;
        fileDecision_ = FileDecision::Reject;
        return true;
    default:
        std::cerr << "Unknown message type\n";
        return false;
    }
}

bool Protocol::BeginFile(std::string_view payload)
{
    // All remote-controlled state and metadata checks precede any disk change.
    if (receivingFile_ || file_ || ownsTemp_ || !hasApprovedRequest_ || hasPendingRequest_)
        return FailFile("File transfer not approved or receiver busy");
    if (payload.size() < 9)
        return FailFile("Invalid file metadata");
    const uint64_t fileSize = ReadU64(payload.substr(0, 8));
    const std::string fileName(payload.substr(8));
    if (fileSize > MAX_FILE_SIZE || !IsSafeFileName(fileName))
        return FailFile("Invalid file metadata");
    if (fileSize != approvedRequest_.fileSize || fileName != approvedRequest_.fileName)
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
    hasApprovedRequest_ = false;
    approvedRequest_ = {};
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
    DWORD written = 0;
    if (!WriteFile(file_, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) ||
        written != payload.size())
        return FailFile("File write failed");
    receivedSize_ += written;
    return true;
}

bool Protocol::EndFile()
{
    if (!receivingFile_ || !file_ || !ownsTemp_ || receivedSize_ != expectedSize_)
        return FailFile("File ended without matching approved size");
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
    completedFiles_.push_back(finalPath_.filename().string());
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

bool Protocol::FailFile(const char* reason)
{
    fileError_ = reason;
    AbortFile();
    hasPendingRequest_ = hasApprovedRequest_ = false;
    pendingRequest_ = approvedRequest_ = {};
    std::cerr << "File receive failed: " << reason << '\n';
    return false;
}

std::string Protocol::TakeFileError()
{
    std::string result;
    result.swap(fileError_);
    return result;
}
Protocol::~Protocol()
{
    AbortFile();
}

void Protocol::ResetConnection()
{
    AbortFile();
    fileError_.clear();
    buffer_.clear();
    completedFiles_.clear();
    hasPendingRequest_ = false;
    pendingRequest_ = {};
    hasApprovedRequest_ = false;
    approvedRequest_ = {};
    fileDecision_ = FileDecision::None;
    expectedSize_ = receivedSize_ = 0;
}
