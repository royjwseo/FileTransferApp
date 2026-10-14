#pragma once
#include "FileTransfer.h"
#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct AppEvent {
    enum class Kind { Connection, Text, Request, State, Progress, Identity, Selection, Log };
    Kind kind = Kind::Log;
    std::string text, name, transferId;
    uint64_t requestId = 0, total = 0;
    uint64_t session = 0;
    bool sending = false;
    FileTransfer::Progress progress;
};

// No UI references or callbacks. The UI drains at 50ms; progress is coalesced.
class EventQueue {
public:
    static constexpr size_t CAPACITY = 256;
    void Push(AppEvent event) {
        if (event.text.size() > 16384) {
            event.text.resize(16384);
            while (!event.text.empty() && (static_cast<unsigned char>(event.text.back()) & 0xc0) == 0x80) event.text.pop_back();
            if (!event.text.empty() && static_cast<unsigned char>(event.text.back()) >= 0xc0) event.text.pop_back();
            event.text += " [truncated]";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (event.kind == AppEvent::Kind::Progress) {
            auto old = std::find_if(queue_.begin(), queue_.end(), [&](const AppEvent& e) {
                return e.kind == event.kind && e.sending == event.sending && e.requestId == event.requestId;
            });
            if (old != queue_.end()) { *old = std::move(event); return; }
        }
        if (queue_.size() == CAPACITY) {
            auto expendable = std::find_if(queue_.begin(), queue_.end(), [](const AppEvent& e) {
                return e.kind == AppEvent::Kind::Text || e.kind == AppEvent::Kind::Log || e.kind == AppEvent::Kind::Progress;
            });
            if (expendable != queue_.end()) queue_.erase(expendable);
            else if (event.kind == AppEvent::Kind::Text || event.kind == AppEvent::Kind::Log) { ++dropped_; return; }
            else queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(std::move(event));
    }
    std::vector<AppEvent> Drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<AppEvent> result;
        result.reserve(queue_.size() + (dropped_ ? 1 : 0));
        if (dropped_) { AppEvent e; e.text = "Some queued events were omitted (bounded queue)."; result.push_back(std::move(e)); dropped_ = 0; }
        while (!queue_.empty()) { result.push_back(std::move(queue_.front())); queue_.pop_front(); }
        return result;
    }
private:
    std::mutex mutex_;
    std::deque<AppEvent> queue_;
    size_t dropped_ = 0;
};
