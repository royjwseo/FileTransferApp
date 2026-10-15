#pragma once
#include "Network.h"
#include "TrustedPeers.h"
#include <map>
#include <optional>

// GUI-only discovery/consent gate. Owned and polled by Controller's worker.
// No HWND, callbacks or file protocol parsing; sockets remain nonblocking until handoff.
class LanService {
public:
    explicit LanService(std::shared_ptr<EventQueue> events);
    ~LanService();
    void Poll(bool busy);
    void Refresh();
    bool Request(const std::string& ip,uint16_t port,const std::string& expectedId = {});
    void Decide(uint64_t token,bool accept);
    void Cancel();
    void RegisterPeer(const std::string& ip,const std::string& key);
    void ForgetPeer(const std::string& ip);
    bool Pending() const;
    struct Approved {SOCKET socket;bool host;};
    std::optional<Approved> TakeApproved();
    const std::string& Id() const {return id_;}
private:
    using Clock=std::chrono::steady_clock;
    struct Candidate {
        SOCKET socket=INVALID_SOCKET;std::string input,output,peerId,expectedId,nonce,ip;
        bool outgoing=false,connecting=false,prompt=false,accepted=false;
        uint64_t token=0;Clock::time_point deadline;
        std::string key,greeting,challenge,authentication;
        int authStage=0;
    };
    std::shared_ptr<EventQueue> events_;
    SOCKET udp_=INVALID_SOCKET,listener_=INVALID_SOCKET;
    std::optional<Candidate> inbound_,outbound_,approved_;
    bool initialized_=false,busy_=false;
    uint16_t port_=0;
    uint64_t nextToken_=0;
    std::string id_,name_;
    TrustedPeers::Store trusted_;
    sockaddr_in group_{};
    std::vector<in_addr> interfaces_;
    Clock::time_point nextSearch_{},nextReply_{},lastSearch_{};
    struct Peer {std::string name,ip;uint16_t port;Clock::time_point seen;};
    std::map<std::string,Peer> peers_;
    void Event(AppEvent::Kind kind,std::string text,uint64_t token=0);
    void Snapshot();
    void Datagram(std::string_view bytes,const sockaddr_in& source);
    void SendDiscovery(char kind);
    void Advance(std::optional<Candidate>& candidate);
    void Drop(std::optional<Candidate>& candidate,const char* reason);
};
