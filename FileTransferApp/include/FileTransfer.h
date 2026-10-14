#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// Per-direction, constant-space progress tracker; no file or network ownership.
class FileTransfer {
public:
    struct Progress {
        std::string fileName;
        uint64_t requestId = 0, total = 0, completed = 0;
        double percent = 0, mibPerSecond = 0, elapsedSeconds = 0;
        bool sending = false;
    };
    static constexpr size_t CHUNK_SIZE = 64 * 1024;
    void Start(std::string name, uint64_t id, uint64_t total, bool sending);
    std::optional<Progress> Update(uint64_t completed, bool force = false);
    static void Print(const Progress& progress);
private:
    using Clock = std::chrono::steady_clock;
    Progress current_;
    Clock::time_point started_{}, last_{};
    uint64_t lastBytes_ = 0;
};
