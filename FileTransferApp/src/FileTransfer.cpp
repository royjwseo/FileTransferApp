#include "FileTransfer.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <syncstream>
#include <utility>

void FileTransfer::Start(std::string name, uint64_t id, uint64_t total, bool sending)
{
    current_ = {};
    current_.fileName = std::move(name);
    current_.requestId = id;
    current_.total = total;
    current_.sending = sending;
    started_ = last_ = Clock::now();
    lastBytes_ = 0;
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
