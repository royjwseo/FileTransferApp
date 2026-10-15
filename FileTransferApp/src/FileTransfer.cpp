#include "FileTransfer.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <syncstream>
#include <utility>
#include "TransferStorage.h"
#include "FileName.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
#ifdef FILETRANSFER_TESTING
#include "../../tests/Stage4Faults.h"
#endif
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

std::wstring FileName::ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > INT_MAX) throw std::invalid_argument("UTF-8 input too long");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!count) throw std::invalid_argument("Invalid UTF-8");
    std::wstring wide(count, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), wide.data(), count) != count)
        throw std::invalid_argument("Invalid UTF-8");
    return wide;
}
std::string FileName::ToUtf8(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    if (utf16.size() > INT_MAX) throw std::invalid_argument("UTF-16 input too long");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::invalid_argument("Invalid UTF-16");
    std::string utf8(count, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(),
        static_cast<int>(utf16.size()), utf8.data(), count, nullptr, nullptr) != count)
        throw std::invalid_argument("Invalid UTF-16");
    return utf8;
}
bool FileName::IsSafe(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > MAX_UTF8_BYTES) return false;
    std::wstring name;
    try { name = ToWide(utf8); } catch (const std::invalid_argument&) { return false; }
    if (name.size() > MAX_UTF16_UNITS || name.front() == L'.' || name.front() == L' ' ||
        name.back() == L'.' || name.back() == L' ') return false;
    for (wchar_t ch : name) {
        if (ch < 32 || (ch >= 0x7f && ch <= 0x9f) || ch == 0xfeff ||
            ch == 0x2028 || ch == 0x2029 || (ch >= 0x202a && ch <= 0x202e) ||
            (ch >= 0x2066 && ch <= 0x2069) ||
            std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos) return false;
    }
    std::wstring base = name.substr(0, name.find(L'.'));
    while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) base.pop_back();
    for (auto& ch : base) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
        base == L"CLOCK$" || base == L"CONIN$" || base == L"CONOUT$") return false;
    if (base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT")) {
        const auto digit = base[3];
        if ((digit >= L'1' && digit <= L'9') || digit == 0xb9 || digit == 0xb2 || digit == 0xb3) return false;
    }
    return true; // Preserve exact Unicode sequence; no normalization or rewriting.
}

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
    std::string FileIdentity(void* file, bool directory = false) {
        BY_HANDLE_FILE_INFORMATION info{};
        Require(GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (directory ? (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 :
                info.nNumberOfLinks == 1 && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)),
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
    Require(bytes.size() >= 138 && bytes.size() <= 137 + FileName::MAX_UTF8_BYTES, "Invalid file metadata");
    TransferSpec s;
    s.transferId = bytes.substr(0, 64); s.digest = bytes.substr(64, 64);
    s.mode = static_cast<uint8_t>(bytes[128]); s.size = Decode64(bytes.substr(129, 8));
    s.name = bytes.substr(137);
    Require(FileName::IsSafe(s.name) && s.mode <= 2 && HexDigest(s.digest) && HexDigest(s.transferId) &&
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
TransferStorage::TransferStorage(TransferSpec spec, std::filesystem::path folder) : spec_(std::move(spec)), folder_(std::move(folder)) {}
TransferStorage::~TransferStorage() { try { Abort(false); } catch (...) {} }
void TransferStorage::PinDirectory(bool create) {
    const auto folder = folder_.empty() ? std::filesystem::absolute("received") : folder_;
    if (create && folder_.empty()) std::filesystem::create_directories(folder);
    directory_ = OwnedHandle(CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Require(bool(directory_), "Cannot open receive directory");
    BY_HANDLE_FILE_INFORMATION info{};
    Require(GetFileInformationByHandle(directory_.value, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Unsafe receive directory");
    Require(FileName::IsSafe(spec_.name), "Invalid file name");
    final_ = folder / FileName::ToWide(spec_.name); part_ = final_; part_ += ".part";
    manifestPath_ = part_; manifestPath_ += ".fta";
}
void TransferStorage::PinMetadataDirectory() {
    wchar_t overridePath[32768]{};DWORD n=GetEnvironmentVariableW(L"FILETRANSFER_METADATA_ROOT",overridePath,32768);
    std::filesystem::path root;
    if(n){Require(n<32768&&std::filesystem::path(overridePath).is_absolute(),"Metadata root must be absolute");root=overridePath;}
    else{PWSTR value=nullptr;Require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&value)),"Cannot locate metadata directory");root=std::filesystem::path(value)/L"FileTransferApp"/L"transfer-metadata";CoTaskMemFree(value);}
    auto native=root.wstring();
    if(!native.starts_with(L"\\\\?\\"))root=native.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+native.substr(2):L"\\\\?\\"+native;
    std::filesystem::create_directories(root);
    metadataDirectory_=OwnedHandle(CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    Require(bool(metadataDirectory_),"Cannot open metadata directory");FileIdentity(metadataDirectory_.value,true);
    // Directory identity avoids path aliases/case and separates identical transfer IDs in different destinations.
    Require(HexDigest(spec_.transferId),"Invalid metadata transfer identity");
    Sha256 key;key.Add(FileIdentity(directory_.value,true));key.Add(spec_.transferId);
    manifestPath_=root/(key.Hex()+".fta");
}
std::unique_ptr<TransferStorage> TransferStorage::Create(const TransferSpec& spec, const std::filesystem::path& folder) {
    auto store = std::unique_ptr<TransferStorage>(new TransferStorage(spec, folder));
    try {
        store->PinDirectory(true);
        store->PinMetadataDirectory();
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
        const auto plain = spec.transferId + FileIdentity(store->data_.value) +
            (folder.empty() ? std::string{} : FileIdentity(store->directory_.value, true));
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
    const std::function<bool()>& cancelled, const std::filesystem::path& folder) {
    auto store = std::unique_ptr<TransferStorage>(new TransferStorage(spec, folder));
    store->PinDirectory(false);
    const auto legacy=store->manifestPath_;
    store->PinMetadataDirectory();
    store->manifest_ = OwnedHandle(CreateFileW(store->manifestPath_.c_str(), GENERIC_READ | DELETE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const DWORD metadataError=store->manifest_?ERROR_SUCCESS:GetLastError();
    bool migrating=false;
    // Only a missing central record permits the legacy lookup. Corruption/access failures never downgrade.
    if(!store->manifest_&&(metadataError==ERROR_FILE_NOT_FOUND||metadataError==ERROR_PATH_NOT_FOUND)){
        store->manifest_=OwnedHandle(CreateFileW(legacy.c_str(),GENERIC_READ|DELETE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        migrating=bool(store->manifest_);
    }
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
    Require((plain.size() == 80 || plain.size() == 96) && plain.substr(0, 64) == spec.transferId, "Resume transfer identity mismatch");
    if (plain.size() == 96) Require(plain.substr(80) == FileIdentity(store->directory_.value, true), "Resume directory ownership mismatch");
    store->complete_ = std::filesystem::exists(std::filesystem::symlink_status(store->final_));
    Require(spec.mode != 2 || store->complete_, "Transfer completion unknown");
    const auto path = store->complete_ ? store->final_ : store->part_;
    store->data_ = OwnedHandle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    Require(bool(store->data_), "Cannot open owned transfer object");
    Require(FileIdentity(store->data_.value) == plain.substr(64,16), "Resume file ownership mismatch");
    store->ownsData_ = true;
    Require(GetFileSizeEx(store->data_.value, &length) && length.QuadPart >= 0 &&
        uint64_t(length.QuadPart) <= spec.size, "Invalid resume offset");
    store->offset_ = static_cast<uint64_t>(length.QuadPart);
    store->hash_ = HashPrefix(store->data_.value, store->offset_, cancelled);
    if (store->complete_) Require(store->offset_ == spec.size && store->hash_->Hex() == spec.digest,
        "Committed file integrity mismatch");
    if(migrating){
        // Copy protected bytes after full ownership/integrity validation; supports different volumes.
        // Publication precedes removing the old owned record, so a crash never loses both copies.
        auto publishing=store->manifestPath_;publishing+=L".new";
        OwnedHandle updated(CreateFileW(publishing.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        Require(bool(updated),"Cannot migrate resume metadata");DWORD written=0;
        try{
            Require(WriteFile(updated.value,sealed.data(),static_cast<DWORD>(sealed.size()),&written,nullptr)&&written==sealed.size()&&FlushFileBuffers(updated.value),"Cannot write migrated metadata");
            RenameOwned(updated.value,store->manifestPath_);
        }catch(...){DeleteOwned(updated.value);throw;}
        DeleteOwned(store->manifest_.value);store->manifest_=std::move(updated);
    }
    return store;
}
void TransferStorage::ValidateDirectory(const std::filesystem::path& folder) {
    Require(folder.is_absolute(), "Download folder must be absolute");
    TransferStorage probe(TransferSpec{0,"probe.bin"}, folder);
    probe.PinDirectory(false);
    static std::atomic<uint64_t> serial{0};
    auto path = folder / (L".fta-write-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial));
    OwnedHandle file(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    Require(bool(file), "Download folder is not writable");
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
    data_.Reset(); manifest_.Reset(); metadataDirectory_.Reset(); directory_.Reset(); ownsData_ = false;
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
