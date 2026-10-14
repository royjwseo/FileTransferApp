
#include "Protocol.h"
#include "FileName.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <syncstream>
#include <stdexcept>
#include <system_error>
#ifdef FILETRANSFER_TESTING
#include "../../tests/Stage4Faults.h"
#endif

namespace fs = std::filesystem;

namespace
{
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
        !FileName::IsSafe(fileName))
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

uint64_t Protocol::StartOutgoing(const std::string& name, const TransferSpec& spec)
{
    if (sendState_ == SendState::AwaitAccept || sendState_ == SendState::Sending ||
        sendState_ == SendState::AwaitAck || sendState_ == SendState::CancelPending || nextId_ == 0) return 0;
    outgoingId_ = nextId_;
    // Never wrap and reuse IDs on this connection.
    nextId_ = nextId_ > UINT64_MAX - 2 ? 0 : nextId_ + 2;
    outgoingName_ = name; outgoingSpec_ = spec; resumeOffset_ = 0; resumeDigest_.clear();
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
        sendState_ == SendState::AwaitAck || sendState_ == SendState::CancelPending)
    {
        sendState_ = SendState::Failed;
        replies_.push_back({ MessageType::FileError, outgoingId_, reason });
    }
}

bool Protocol::Cancel(uint64_t id, bool notify)
{
    const bool sending = sendState_ == SendState::AwaitAccept ||
        sendState_ == SendState::Sending || sendState_ == SendState::AwaitAck;
    const bool receiving = receiveState_ == ReceiveState::PendingApproval ||
        receiveState_ == ReceiveState::AwaitBegin || receiveState_ == ReceiveState::Receiving;
    if ((!id || id == outgoingId_) && sending)
    {
        id = outgoingId_;
        sendState_ = notify && sendState_ == SendState::AwaitAck ? SendState::CancelPending : SendState::Cancelled;
    }
    else if ((!id || id == incomingId_) && receiving)
    {
        id = incomingId_;
        cancelledIncoming_.push_back(id);
        if (cancelledIncoming_.size() > 64) cancelledIncoming_.erase(cancelledIncoming_.begin());
        ClearIncoming();
        receiveState_ = ReceiveState::Cancelled;
    }
    else return false; // Completed, unknown and duplicate cancellation is inert.
    if (notify) replies_.push_back({ MessageType::FileCancel, id, {} });
    std::osyncstream(std::cout) << ((id == outgoingId_ && sendState_ == SendState::CancelPending) ?
        "\n[Cancel requested; awaiting outcome] requestId=" : "\n[Transfer cancelled] requestId=") << id << '\n' << std::flush;
    return true;
}

bool Protocol::TakeFileRequest(IncomingFileRequest& out)
{
    if (receiveState_ != ReceiveState::PendingApproval) return false;
    out = incoming_;
    return true;
}

uint64_t Protocol::ApproveFileRequest(const IncomingFileRequest& request,
    std::unique_ptr<TransferStorage> prepared, const std::string& error)
{
    if (receiveState_ != ReceiveState::PendingApproval || incomingId_ != request.requestId ||
        generation_ != request.generation) return 0;
    const auto id = incomingId_;
    if (!error.empty()) {
        replies_.push_back({ MessageType::FileError, id, error.substr(0, 256) });
        ClearIncoming(); return id;
    }
    storage_ = std::move(prepared);
    if (storage_ && storage_->Complete()) {
        replies_.push_back({ MessageType::FileComplete, id, incoming_.spec.transferId + incoming_.spec.digest });
        ClearIncoming(true); return id;
    }
    const uint64_t offset = storage_ ? storage_->Offset() : 0;
    Sha256 empty;
    std::string offer; AppendU64(offer, offset);
    offer += storage_ ? storage_->PrefixDigest() : empty.Hex();
    replies_.push_back({ MessageType::FileAccept, id, offer });
    receiveState_ = ReceiveState::AwaitBegin;
    receiveDeadline_ = Clock::now() + TRANSFER_TIMEOUT;
    return id;
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
    if ((sendState_ == SendState::AwaitAccept || sendState_ == SendState::AwaitAck || sendState_ == SendState::CancelPending) &&
        Clock::now() >= sendDeadline_)
        FailOutgoing(sendState_ == SendState::AwaitAccept ? "Approval timed out" : "ACK timed out");
    if ((receiveState_ == ReceiveState::PendingApproval || receiveState_ == ReceiveState::AwaitBegin ||
        receiveState_ == ReceiveState::Receiving) && Clock::now() >= receiveDeadline_)
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
        if (length > MAX_PAYLOAD) { ClearIncoming(); return false; }
        const size_t frameSize = HEADER_SIZE + size_t(length);
        const auto n = std::min(frameSize - buffer_.size(), bytes.size());
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + n);
        bytes.remove_prefix(n);
        if (buffer_.size() < frameSize) break;
        recoverableError_ = false;
        if (!ProcessMessage(static_cast<MessageType>(p[0]),
            std::string_view(buffer_.data() + HEADER_SIZE, length)) && !recoverableError_) {
            ClearIncoming(); return false;
        }
        buffer_.clear();
    }
    return true;
}
bool Protocol::ProcessMessage(MessageType type, std::string_view payload)
{
    if (type == MessageType::Text)
    {
        std::osyncstream output(std::cout); output << "\n[Received TEXT] ";
        output.write(payload.data(), payload.size()); output << "\n> " << std::flush;
        return true;
    }
    if (type < MessageType::FileBegin || type > MessageType::FileComplete) return false;
    // IDs are mandatory for every file message; old wire format is not accepted.
    if (payload.size() < 8) return false;
    const uint64_t id = ReadU64(payload.substr(0, 8));
    payload.remove_prefix(8);
    if (!id) return false;
    if (type == MessageType::FileCancel)
    {
        if (!payload.empty()) return false;
        if (id == outgoingId_ && sendState_ == SendState::CancelPending) {
            sendState_ = SendState::Cancelled;
            std::osyncstream(std::cout) << "\n[Transfer cancelled] requestId=" << id << '\n' << std::flush;
        } else if (Cancel(id, false)) {
            replies_.push_back({ MessageType::FileCancel, id, {} });
        } else {
            const auto committed = std::find_if(committedReplies_.begin(), committedReplies_.end(),
                [id](const Reply& r) { return r.requestId == id; });
            if (committed != committedReplies_.end()) replies_.push_back(*committed);
        }
        return true;
    }
    if ((type == MessageType::FileBegin || type == MessageType::FileChunk || type == MessageType::FileEnd) &&
        std::find(cancelledIncoming_.begin(), cancelledIncoming_.end(), id) != cancelledIncoming_.end())
        return true; // Frames already in flight before the cancellation notice.

    if (type == MessageType::FileAccept || type == MessageType::FileReject ||
        type == MessageType::FileAck || type == MessageType::FileError || type == MessageType::FileComplete)
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
        else if (type == MessageType::FileAccept) { if (payload.size() != 72) return false; }
        else if (type == MessageType::FileAck || type == MessageType::FileComplete) { if (payload.size() != 128) return false; }
        else if (!payload.empty()) return false;

        const bool active = sendState_ == SendState::AwaitAccept ||
            sendState_ == SendState::Sending || sendState_ == SendState::AwaitAck || sendState_ == SendState::CancelPending;
        if (id != outgoingId_ || !active)
        {
            std::osyncstream(std::cout) << "\n[Ignored stale file response] " << id << '\n' << std::flush;
            return true;
        }
        if (error)
        {
            std::cerr << "\n[Peer file error] " << payload << '\n' << std::flush;
            sendState_ = SendState::Failed;
            return true;
        }
        if (type == MessageType::FileAccept && sendState_ == SendState::AwaitAccept)
        {
            resumeOffset_ = ReadU64(payload.substr(0, 8)); resumeDigest_ = payload.substr(8);
            if (resumeOffset_ > outgoingSpec_.size || (outgoingSpec_.mode == 0 && resumeOffset_ != 0) || outgoingSpec_.mode == 2) {
                FailOutgoing("Invalid resume offset"); return false;
            }
            sendState_ = SendState::Sending;
        }
        else if (type == MessageType::FileReject && sendState_ == SendState::AwaitAccept)
            sendState_ = SendState::Failed;
        else if ((type == MessageType::FileAck && (sendState_ == SendState::AwaitAck || sendState_ == SendState::CancelPending)) ||
            (type == MessageType::FileComplete && sendState_ == SendState::AwaitAccept && outgoingSpec_.mode != 0))
        {
            if (payload != outgoingSpec_.transferId + outgoingSpec_.digest) { FailOutgoing("Completion identity mismatch"); return false; }
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
        if (receiveState_ != ReceiveState::Idle && receiveState_ != ReceiveState::Cancelled)
        {
            replies_.push_back({ MessageType::FileError, id, "Receiver busy" });
            return false;
        }
        incomingId_ = id;
        lastIncomingId_ = id;
        TransferSpec spec;
        try { spec = TransferSpec::Decode(payload); }
        catch (const std::exception& e) { return FailFile(e.what()); }
        const auto size = spec.size; const auto name = spec.name;
        if (size > MAX_FILE_SIZE || !FileName::IsSafe(name)) return FailFile("Invalid file metadata");
        incoming_ = { name, size, id, spec, generation_ };
        receiveState_ = ReceiveState::PendingApproval;
        receiveDeadline_ = Clock::now() + APPROVAL_TIMEOUT;
        std::osyncstream(std::cout) << "\n[Incoming file request] " << name << " (" << size
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
            replies_.push_back({ MessageType::FileAck, id, incoming_.spec.transferId + incoming_.spec.digest });
            committedReplies_.push_back(replies_.back());
            if (committedReplies_.size() > 64) committedReplies_.erase(committedReplies_.begin());
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
    if (receiveState_ != ReceiveState::AwaitBegin || receivingFile_)
        return FailFile("File transfer not approved or receiver busy");
    if (payload != incoming_.spec.Encode()) return FailFile("File metadata mismatch");
    try {
        if (!storage_) storage_ = TransferStorage::Create(incoming_.spec);
        expectedSize_ = incoming_.fileSize; receivedSize_ = storage_->Offset();
        receivingFile_ = true;
        receiveProgress_.Start(incoming_.fileName, incomingId_, expectedSize_, false, receivedSize_);
        pendingProgress_ = receiveProgress_.Update(receivedSize_, true);
        std::osyncstream(std::cout) << "\n[Receiving] " << incoming_.fileName << " (" << expectedSize_ << " bytes)\n" << std::flush;
        return true;
    } catch (const std::exception& e) { return FailFile(e.what(), false); }
}
bool Protocol::WriteFileChunk(std::string_view payload)
{
    if (!receivingFile_ || !storage_) return FailFile("File data received without active approved transfer");
    if (receivedSize_ > expectedSize_ || payload.size() > expectedSize_ - receivedSize_)
        return FailFile("File data exceeds approved size");
    try {
        storage_->Write(payload); receivedSize_ = storage_->Offset();
        if (auto p = receiveProgress_.Update(receivedSize_)) pendingProgress_ = std::move(p);
        return true;
    } catch (const std::exception& e) { return FailFile(e.what(), false); }
}
bool Protocol::EndFile()
{
    if (!receivingFile_ || !storage_ || receivedSize_ != expectedSize_) return FailFile("File ended without matching approved size");
    try {
        storage_->Commit();
        pendingProgress_ = receiveProgress_.Update(receivedSize_, true);
        receivingFile_ = false;
        std::osyncstream(std::cout) << "\n[File saved] received/" << incoming_.fileName << "\n> " << std::flush;
        return true;
    } catch (const std::exception& e) { return FailFile(e.what(), false); }
}
void Protocol::AbortFile()
{
    if (storage_) { try { storage_->Abort(true); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; } storage_.reset(); }
    receivingFile_ = false; expectedSize_ = receivedSize_ = 0;
}
void Protocol::ClearIncoming(bool preserve)
{
    if (preserve) storage_.reset();
    AbortFile(); receiveState_ = ReceiveState::Idle; incomingId_ = 0; incoming_ = {};
}
bool Protocol::FailFile(const char* reason, bool fatal)
{
    if (incomingId_) replies_.push_back({ MessageType::FileError, incomingId_, reason });
    if (!fatal && incomingId_) {
        cancelledIncoming_.push_back(incomingId_);
        if (cancelledIncoming_.size() > 64) cancelledIncoming_.erase(cancelledIncoming_.begin());
    }
    recoverableError_ = !fatal;
    ClearIncoming();
    std::cerr << "File receive failed: " << reason << '\n' << std::flush;
    return false;
}

Protocol::~Protocol() { AbortFile(); }

void Protocol::ResetConnection()
{
    ClearIncoming(true);
    if (sendState_ != SendState::Completed) sendState_ = SendState::Failed;
    pendingProgress_.reset();
    replies_.clear();
    buffer_.clear();
    cancelledIncoming_.clear();
    committedReplies_.clear();
}

void Protocol::ResetSession()
{
    ++generation_;
    ResetConnection();
    sendState_ = SendState::Idle;
    outgoingId_ = incomingId_ = lastIncomingId_ = 0;
    outgoingName_.clear();
}
