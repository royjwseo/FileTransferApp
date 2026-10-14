#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class OwnedHandle {
public:
    void* value = nullptr;
    OwnedHandle() = default;
    explicit OwnedHandle(void* h);
    ~OwnedHandle();
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    OwnedHandle(OwnedHandle&& other) noexcept;
    OwnedHandle& operator=(OwnedHandle&& other) noexcept;
    explicit operator bool() const { return value != nullptr; }
    void Reset();
};

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void Add(std::string_view bytes);
    std::string Hex() const; // snapshot; leaves the streaming state usable
private:
    void* algorithm_ = nullptr;
    void* hash_ = nullptr;
};

struct TransferSpec {
    uint64_t size = 0;
    std::string name, digest, transferId;
    uint8_t mode = 0; // 0: fresh, 1: explicit resume, 2: completion query
    std::string Encode() const;
    std::string Identity() const;
    static TransferSpec Decode(std::string_view bytes);
};

std::unique_ptr<Sha256> HashPrefix(void* file, uint64_t count,
    const std::function<bool()>& cancelled = {});
OwnedHandle OpenSource(const std::filesystem::path& path, uint64_t& size);
void SeekFile(void* file, uint64_t offset);

// Exclusive handles pin the directory, data object and authenticated manifest.
// Destruction preserves resumable state. Only Abort(true) deletes owned data.
class TransferStorage {
public:
    static std::unique_ptr<TransferStorage> Create(const TransferSpec& spec, const std::filesystem::path& folder = {});
    static std::unique_ptr<TransferStorage> Recover(const TransferSpec& spec,
        const std::function<bool()>& cancelled, const std::filesystem::path& folder = {});
    ~TransferStorage();
    static void ValidateDirectory(const std::filesystem::path& folder);
    void Write(std::string_view bytes);
    void Commit();
    void Abort(bool remove);
    uint64_t Offset() const { return offset_; }
    std::string PrefixDigest() const { return hash_->Hex(); }
    bool Complete() const { return complete_; }
private:
    explicit TransferStorage(TransferSpec spec, std::filesystem::path folder = {});
    TransferSpec spec_;
    OwnedHandle directory_, data_, manifest_;
    std::filesystem::path folder_, final_, part_, manifestPath_;
    std::unique_ptr<Sha256> hash_;
    uint64_t offset_ = 0;
    bool complete_ = false, ownsData_ = false;
    void PinDirectory(bool create);
};
