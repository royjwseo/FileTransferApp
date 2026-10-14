#include "FileTransfer.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <syncstream>
#include <utility>
#include "TransferStorage.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
#ifdef FILETRANSFER_TESTING
#include "../../tests/Stage4Faults.h"
#endif
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace {
    void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    void Append64(std::string& s, uint64_t n) {
        for (int i = 7; i >= 0; --i) s.push_back(static_cast<char>(n >> (8 * i)));
    }
    uint64_t Decode64(std::string_view s) {
        uint64_t n = 0;
        for (int i = 0; i < 8; ++i) n = (n << 8) | static_cast<unsigned char>(s[i]);
        return n;
    }
    bool HexDigest(std::string_view s) {
        return s.size() == 64 && std::all_of(s.begin(), s.end(),
            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }
    std::string FileIdentity(void* file) {
        BY_HANDLE_FILE_INFORMATION info{};
        Require(GetFileInformationByHandle(file, &info) && info.nNumberOfLinks == 1 &&
            !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
            "Unsafe data object");
        std::string id;
        Append64(id, info.dwVolumeSerialNumber);
        Append64(id, (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
        return id;
    }
    void DeleteOwned(void* file) {
        FILE_DISPOSITION_INFO info{ TRUE };
        Require(SetFileInformationByHandle(file, FileDispositionInfo, &info, sizeof(info)),
            "Cannot delete owned temporary file");
    }
    void RenameOwned(void* file, const std::filesystem::path& path) {
        const auto name = path.wstring();
        const DWORD bytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + bytes);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->ReplaceIfExists = FALSE; info->RootDirectory = nullptr; info->FileNameLength = bytes;
        std::memcpy(info->FileName, name.data(), bytes);
        Require(SetFileInformationByHandle(file, FileRenameInfo, info,
            static_cast<DWORD>(buffer.size())), "Cannot commit file; destination exists or rename failed");
    }
}

OwnedHandle::OwnedHandle(void* h) : value(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
OwnedHandle::~OwnedHandle() { Reset(); }
void OwnedHandle::Reset() { if (value) CloseHandle(value); value = nullptr; }
OwnedHandle::OwnedHandle(OwnedHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
OwnedHandle& OwnedHandle::operator=(OwnedHandle&& other) noexcept {
    if (this != &other) { Reset(); value = std::exchange(other.value, nullptr); } return *this;
}
Sha256::Sha256() {
    Require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
        "Cannot initialize SHA-256");
    if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr;
        throw std::runtime_error("Cannot create SHA-256 state");
    }
}
Sha256::~Sha256() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
void Sha256::Add(std::string_view bytes) {
    Require(bytes.size() <= ULONG_MAX && BCryptHashData(hash_,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) >= 0,
        "SHA-256 update failed");
}
std::string Sha256::Hex() const {
    BCRYPT_HASH_HANDLE copy = nullptr;
    Require(BCryptDuplicateHash(hash_, &copy, nullptr, 0, 0) >= 0, "Cannot snapshot SHA-256");
    std::array<unsigned char, 32> digest{};
    const auto result = BCryptFinishHash(copy, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(copy); Require(result >= 0, "Cannot finish SHA-256");
    static constexpr char hex[] = "0123456789abcdef";
    std::string text; text.reserve(64);
    for (auto b : digest) { text += hex[b >> 4]; text += hex[b & 15]; }
    return text;
}
std::string TransferSpec::Identity() const {
    std::string bytes("FTA5\0", 5); Append64(bytes, size); bytes += digest; bytes += name;
    Sha256 hash; hash.Add(bytes); return hash.Hex();
}
std::string TransferSpec::Encode() const {
    std::string bytes = transferId + digest;
    bytes += static_cast<char>(mode); Append64(bytes, size); bytes += name; return bytes;
}
TransferSpec TransferSpec::Decode(std::string_view bytes) {
    Require(bytes.size() >= 138 && bytes.size() <= 237, "Invalid file metadata");
    TransferSpec s;
    s.transferId = bytes.substr(0, 64); s.digest = bytes.substr(64, 64);
    s.mode = static_cast<uint8_t>(bytes[128]); s.size = Decode64(bytes.substr(129, 8));
    s.name = bytes.substr(137);
    Require(s.mode <= 2 && HexDigest(s.digest) && HexDigest(s.transferId) &&
        s.Identity() == s.transferId, "Invalid transfer identity");
    return s; // filename and upper size bound are validated by Protocol before I/O
}
void SeekFile(void* file, uint64_t offset) {
    Require(offset <= INT64_MAX, "Invalid file offset");
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    Require(SetFilePointerEx(file, position, nullptr, FILE_BEGIN), "File seek failed");
}
OwnedHandle OpenSource(const std::filesystem::path& path, uint64_t& size) {
    OwnedHandle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    Require(bool(handle), "Cannot open source or source is being modified");
    FileIdentity(handle.value);
    LARGE_INTEGER length{};
    Require(GetFileSizeEx(handle.value, &length) && length.QuadPart >= 0, "Cannot inspect source size");
    size = static_cast<uint64_t>(length.QuadPart); return handle;
}
std::unique_ptr<Sha256> HashPrefix(void* file, uint64_t count, const std::function<bool()>& cancelled) {
    SeekFile(file, 0); auto hash = std::make_unique<Sha256>();
    std::array<char, FileTransfer::CHUNK_SIZE> bytes;
    while (count) {
        Require(!cancelled || !cancelled(), "Hash scan cancelled");
        const auto wanted = static_cast<DWORD>(std::min<uint64_t>(count, bytes.size()));
        DWORD read = 0;
        Require(ReadFile(file, bytes.data(), wanted, &read, nullptr) && read == wanted, "File read failed");
        hash->Add(std::string_view(bytes.data(), read)); count -= read;
    }
    return hash;
}
TransferStorage::TransferStorage(TransferSpec spec) : spec_(std::move(spec)) {}
TransferStorage::~TransferStorage() { try { Abort(false); } catch (...) {} }
void TransferStorage::PinDirectory(bool create) {
    const auto folder = std::filesystem::absolute("received");
    if (create) std::filesystem::create_directories(folder);
    directory_ = OwnedHandle(CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Require(bool(directory_), "Cannot open receive directory");
    BY_HANDLE_FILE_INFORMATION info{};
    Require(GetFileInformationByHandle(directory_.value, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Unsafe receive directory");
    final_ = folder / spec_.name; part_ = final_; part_ += ".part";
    manifestPath_ = part_; manifestPath_ += ".fta";
}
std::unique_ptr<TransferStorage> TransferStorage::Create(const TransferSpec& spec) {
    auto store = std::unique_ptr<TransferStorage>(new TransferStorage(spec));
    try {
        store->PinDirectory(true);
        Require(!std::filesystem::exists(std::filesystem::symlink_status(store->final_)), "Destination already exists");
        ULARGE_INTEGER available{};
        Require(GetDiskFreeSpaceExW(store->final_.parent_path().c_str(), &available, nullptr, nullptr),
            "Cannot inspect available disk space");
#ifdef FILETRANSFER_TESTING
        available.QuadPart = Stage4Faults::Value("FTA_TEST_FREE_BYTES", available.QuadPart);
#endif
        Require(spec.size <= available.QuadPart, "Insufficient disk space");
        store->data_ = OwnedHandle(CreateFileW(store->part_.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(bool(store->data_), "Cannot create temporary file or temporary file exists");
        store->ownsData_ = true;
        const auto plain = spec.transferId + FileIdentity(store->data_.value);
        DATA_BLOB input{ static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data())) }, output{};
        Require(CryptProtectData(&input, L"FileTransferApp ownership", nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output), "Cannot seal resume metadata");
        // Immutable CREATE_NEW manifest; partial metadata after a crash fails closed.
        auto publishing = store->manifestPath_; publishing += ".new";
        store->manifest_ = OwnedHandle(CreateFileW(publishing.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        const bool saved = store->manifest_ && WriteFile(store->manifest_.value, output.pbData,
            output.cbData, &written, nullptr) && written == output.cbData && FlushFileBuffers(store->manifest_.value);
        LocalFree(output.pbData); Require(saved, "Cannot publish resume metadata");
        RenameOwned(store->manifest_.value, store->manifestPath_);
        store->hash_ = std::make_unique<Sha256>();
        return store;
    } catch (...) { store->Abort(true); throw; }
}
std::unique_ptr<TransferStorage> TransferStorage::Recover(const TransferSpec& spec,
    const std::function<bool()>& cancelled) {
    auto store = std::unique_ptr<TransferStorage>(new TransferStorage(spec));
    store->PinDirectory(false);
    store->manifest_ = OwnedHandle(CreateFileW(store->manifestPath_.c_str(), GENERIC_READ | DELETE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Require(bool(store->manifest_), "No owned resume metadata"); FileIdentity(store->manifest_.value);
    LARGE_INTEGER length{};
    Require(GetFileSizeEx(store->manifest_.value, &length) && length.QuadPart > 0 && length.QuadPart <= 4096,
        "Invalid resume metadata");
    std::vector<BYTE> sealed(static_cast<size_t>(length.QuadPart)); DWORD read = 0;
    Require(ReadFile(store->manifest_.value, sealed.data(), static_cast<DWORD>(sealed.size()), &read, nullptr) &&
        read == sealed.size(), "Cannot read resume metadata");
    DATA_BLOB input{ read, sealed.data() }, output{};
    Require(CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output),
        "Resume metadata authentication failed");
    const std::string plain(reinterpret_cast<char*>(output.pbData), output.cbData); LocalFree(output.pbData);
    Require(plain.size() == 80 && plain.substr(0, 64) == spec.transferId, "Resume transfer identity mismatch");
    store->complete_ = std::filesystem::exists(std::filesystem::symlink_status(store->final_));
    Require(spec.mode != 2 || store->complete_, "Transfer completion unknown");
    const auto path = store->complete_ ? store->final_ : store->part_;
    store->data_ = OwnedHandle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    Require(bool(store->data_), "Cannot open owned transfer object");
    Require(FileIdentity(store->data_.value) == plain.substr(64), "Resume file ownership mismatch");
    store->ownsData_ = true;
    Require(GetFileSizeEx(store->data_.value, &length) && length.QuadPart >= 0 &&
        uint64_t(length.QuadPart) <= spec.size, "Invalid resume offset");
    store->offset_ = static_cast<uint64_t>(length.QuadPart);
    store->hash_ = HashPrefix(store->data_.value, store->offset_, cancelled);
    if (store->complete_) Require(store->offset_ == spec.size && store->hash_->Hex() == spec.digest,
        "Committed file integrity mismatch");
    return store;
}
void TransferStorage::Write(std::string_view bytes) {
    Require(!complete_ && data_ && offset_ <= spec_.size && bytes.size() <= spec_.size - offset_,
        "File data exceeds approved size");
    size_t cursor = 0;
    while (cursor < bytes.size()) {
        DWORD wanted = static_cast<DWORD>(bytes.size() - cursor), written = 0;
#ifdef FILETRANSFER_TESTING
        Require(!Stage4Faults::Fail("FTA_TEST_WRITE_FAIL_AFTER", offset_), "File write failed");
        wanted = static_cast<DWORD>(std::min<uint64_t>(wanted, Stage4Faults::Value("FTA_TEST_WRITE_SLICE", wanted)));
#endif
        Require(WriteFile(data_.value, bytes.data() + cursor, wanted, &written, nullptr) && written && written <= wanted,
            "File write failed");
        hash_->Add(bytes.substr(cursor, written)); cursor += written; offset_ += written;
    }
}
void TransferStorage::Commit() {
    Require(data_ && !complete_ && offset_ == spec_.size, "File ended without matching approved size");
    Require(hash_->Hex() == spec_.digest, "SHA-256 mismatch");
#ifdef FILETRANSFER_TESTING
    Require(!Stage4Faults::Value("FTA_TEST_FLUSH_FAIL", 0), "File flush failed");
#endif
    Require(FlushFileBuffers(data_.value), "File flush failed");
    RenameOwned(data_.value, final_); complete_ = true;
}
void TransferStorage::Abort(bool remove) {
    if (remove && ownsData_ && !complete_ && data_) {
        DeleteOwned(data_.value);
        if (manifest_) DeleteOwned(manifest_.value);
    } else if (!remove && ownsData_ && !complete_ && data_) {
        Require(FlushFileBuffers(data_.value), "Cannot checkpoint owned partial file");
    }
    data_.Reset(); manifest_.Reset(); directory_.Reset(); ownsData_ = false;
}

void FileTransfer::Start(std::string name, uint64_t id, uint64_t total, bool sending, uint64_t initial)
{
    current_ = {};
    current_.fileName = std::move(name);
    current_.requestId = id;
    current_.total = total;
    current_.sending = sending;
    started_ = last_ = Clock::now();
    lastBytes_ = initial;
}

std::optional<FileTransfer::Progress> FileTransfer::Update(uint64_t completed, bool force)
{
    const auto now = Clock::now();
    const double interval = std::chrono::duration<double>(now - last_).count();
    if (!force && interval < 0.5) return std::nullopt;
    current_.completed = completed;
    current_.elapsedSeconds = std::chrono::duration<double>(now - started_).count();
    current_.mibPerSecond = interval > 0 ?
        static_cast<double>(completed - lastBytes_) / (1024.0 * 1024.0 * interval) : 0;
    // Cast before multiplication; never round incomplete data up to 100%.
    current_.percent = current_.total == 0 || completed == current_.total ? 100.0 :
        std::min(99.9, std::floor(1000.0 * static_cast<double>(completed) /
            static_cast<double>(current_.total)) / 10.0);
    last_ = now;
    lastBytes_ = completed;
    return current_;
}

void FileTransfer::Print(const Progress& p)
{
    std::osyncstream(std::cout) << (p.sending ? "\nSending: " : "\nSaving: ")
        << "file=" << p.fileName << " requestId=" << p.requestId
        << " bytes=" << p.completed << '/' << p.total
        << " progress=" << std::fixed << std::setprecision(1) << p.percent << '%'
        << " rate=" << std::setprecision(2) << p.mibPerSecond << " MiB/s"
        << " elapsed=" << p.elapsedSeconds << "s"
        << (p.completed == p.total ? (p.sending ? " [data sent; ACK required]" :
            " [data written; flush/commit required]") : "") << '\n' << std::flush;
}
