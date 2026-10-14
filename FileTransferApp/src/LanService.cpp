#define NOMINMAX
#include "LanService.h"
#include "FileName.h"
#include <bcrypt.h>
#include <charconv>
#include <iphlpapi.h>
#pragma comment(lib,"iphlpapi.lib")
#pragma comment(lib,"bcrypt.lib")
namespace {
std::string RandomId(){unsigned char bytes[16];if(BCryptGenRandom(nullptr,bytes,sizeof(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Cannot generate instance identity");std::string s;for(auto b:bytes){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;}
bool Hex(std::string_view s){return s.size()==32&&s.find_first_not_of("0123456789abcdef")==s.npos;}
bool Name(std::string_view s){if(s.empty()||s.size()>96||s.find('|')!=s.npos)return false;try{auto w=FileName::ToWide(std::string(s));for(auto c:w)if(c<32||c==127||(c>=0x202a&&c<=0x202e)||(c>=0x2066&&c<=0x2069))return false;return true;}catch(...){return false;}}
std::vector<std::string> Split(std::string_view s){std::vector<std::string> result;size_t start=0;while(result.size()<8){auto end=s.find('|',start);result.emplace_back(s.substr(start,end==s.npos?s.size()-start:end-start));if(end==s.npos)return result;start=end+1;}return {};}
void Close(SOCKET& s){if(s!=INVALID_SOCKET){closesocket(s);s=INVALID_SOCKET;}}
bool Nonblock(SOCKET s){u_long yes=1;return ioctlsocket(s,FIONBIO,&yes)==0;}
std::string Address(const sockaddr_in& a){char text[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&a.sin_addr,text,sizeof(text));return text;}
}
LanService::LanService(std::shared_ptr<EventQueue> events):events_(std::move(events)) {
    WSADATA w{};if(WSAStartup(MAKEWORD(2,2),&w))throw std::runtime_error("Discovery Winsock initialization failed");initialized_=true;
    try {
        id_=RandomId();wchar_t computer[256]{};DWORD n=256;GetComputerNameW(computer,&n);
        name_=FileName::ToUtf8(computer);if(!Name(name_))name_="Windows PC";
        listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);BOOL exclusive=TRUE;
        if(listener_==INVALID_SOCKET||setsockopt(listener_,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof(exclusive)))throw std::runtime_error("Cannot create consent listener");
                unsigned listenPort=9000;wchar_t portSetting[6]{};DWORD count=GetEnvironmentVariableW(L"FILETRANSFER_LAN_PORT",portSetting,6);
        if(count){if(count>=6)throw std::runtime_error("Invalid consent listen port");for(DWORD i=0;i<count;++i)if(portSetting[i]<L'0'||portSetting[i]>L'9')throw std::runtime_error("Invalid consent listen port");listenPort=static_cast<unsigned>(wcstoul(portSetting,nullptr,10));if(listenPort>65535)throw std::runtime_error("Invalid consent listen port");}
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(static_cast<uint16_t>(listenPort));
        if(bind(listener_,reinterpret_cast<sockaddr*>(&local),sizeof(local))){local.sin_port=0;if(bind(listener_,reinterpret_cast<sockaddr*>(&local),sizeof(local)))throw std::runtime_error("Cannot bind consent listener");}
        int len=sizeof(local);getsockname(listener_,reinterpret_cast<sockaddr*>(&local),&len);port_=ntohs(local.sin_port);
        if(listen(listener_,4)||!Nonblock(listener_))throw std::runtime_error("Cannot listen for consent requests");
        try {
        udp_=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);BOOL reuse=TRUE;int ttl=1;
        if(udp_==INVALID_SOCKET||setsockopt(udp_,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<char*>(&reuse),sizeof(reuse)))throw std::runtime_error("Cannot create discovery socket");
        local.sin_port=htons(9001);local.sin_addr.s_addr=INADDR_ANY;
        if(bind(udp_,reinterpret_cast<sockaddr*>(&local),sizeof(local))||!Nonblock(udp_))throw std::runtime_error("UDP discovery port unavailable");
        group_.sin_family=AF_INET;group_.sin_port=htons(9001);inet_pton(AF_INET,"239.255.90.8",&group_.sin_addr);
        ULONG bytes=16384;std::vector<unsigned char> adapters(bytes);
        ULONG result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapters.data()),&bytes);
        if(result==ERROR_BUFFER_OVERFLOW&&bytes<=262144){adapters.resize(bytes);result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapters.data()),&bytes);}
        if(result==NO_ERROR)for(auto* a=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapters.data());a&&interfaces_.size()<16;a=a->Next){
            if(a->OperStatus!=IfOperStatusUp)continue;
            for(auto* u=a->FirstUnicastAddress;u&&interfaces_.size()<16;u=u->Next){if(u->Address.lpSockaddr->sa_family!=AF_INET)continue;
                auto ip=reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
                ip_mreq membership{};membership.imr_multiaddr=group_.sin_addr;membership.imr_interface=ip;
                if(!setsockopt(udp_,IPPROTO_IP,IP_ADD_MEMBERSHIP,reinterpret_cast<char*>(&membership),sizeof(membership)))interfaces_.push_back(ip);
            }
        }
        if(interfaces_.empty())throw std::runtime_error("No multicast-capable IPv4 interface");
        setsockopt(udp_,IPPROTO_IP,IP_MULTICAST_TTL,reinterpret_cast<char*>(&ttl),sizeof(ttl));
        }catch(const std::exception& e){Close(udp_);Event(AppEvent::Kind::LanStatus,std::string("Discovery unavailable; direct connection remains available: ")+e.what());}
        Event(AppEvent::Kind::LanStatus,"LAN ready: "+id_+" TCP="+std::to_string(port_));
    }catch(...){Close(udp_);Close(listener_);WSACleanup();initialized_=false;throw;}
}
LanService::~LanService(){if(inbound_)Close(inbound_->socket);if(outbound_)Close(outbound_->socket);if(approved_)Close(approved_->socket);Close(listener_);Close(udp_);if(initialized_)WSACleanup();}
void LanService::Event(AppEvent::Kind kind,std::string text,uint64_t token){AppEvent e;e.kind=kind;e.text=std::move(text);e.requestId=token;e.total=Pending()?1:0;events_->Push(std::move(e));}
void LanService::Snapshot(){AppEvent e;e.kind=AppEvent::Kind::Peers;for(auto& [id,p]:peers_)e.peers.push_back({id,p.name,p.ip,p.port});events_->Push(std::move(e));}
void LanService::SendDiscovery(char kind){const auto packet="FTA-DISC|1|"+std::string(1,kind)+"|"+id_+"|"+std::to_string(port_)+"|"+name_;if(udp_==INVALID_SOCKET)return;for(auto ip:interfaces_){if(!setsockopt(udp_,IPPROTO_IP,IP_MULTICAST_IF,reinterpret_cast<char*>(&ip),sizeof(ip)))sendto(udp_,packet.data(),static_cast<int>(packet.size()),0,reinterpret_cast<sockaddr*>(&group_),sizeof(group_));}}
void LanService::Refresh(){nextSearch_=std::max(Clock::now(),lastSearch_+std::chrono::seconds(1));}
void LanService::Datagram(std::string_view bytes,const sockaddr_in& source){
    if(bytes.size()>256)return;auto f=Split(bytes);if(f.size()!=6||f[0]!="FTA-DISC"||f[1]!="1"||(f[2]!="Q"&&f[2]!="R")||!Hex(f[3])||f[3]==id_||!Name(f[5]))return;
    unsigned port=0;auto [end,ec]=std::from_chars(f[4].data(),f[4].data()+f[4].size(),port);if(ec!=std::errc{}||end!=f[4].data()+f[4].size()||port==0||port>65535)return;
    if(f[2]=="Q"){if(Clock::now()>=nextReply_){SendDiscovery('R');nextReply_=Clock::now()+std::chrono::seconds(1);}return;}
    if(peers_.size()>=64&&!peers_.contains(f[3]))return;
    auto ip=Address(source);auto it=peers_.find(f[3]);bool changed=it==peers_.end()||it->second.ip!=ip||it->second.name!=f[5]||it->second.port!=port;
    peers_[f[3]]={f[5],ip,static_cast<uint16_t>(port),Clock::now()};if(changed)Snapshot();
}
bool LanService::Pending()const{return inbound_.has_value()||outbound_.has_value()||approved_.has_value();}
void LanService::Drop(std::optional<Candidate>& c,const char* reason){if(!c)return;if(c->prompt)Event(AppEvent::Kind::Consent,"",0);Close(c->socket);c.reset();if(reason)Event(AppEvent::Kind::LanStatus,reason);}
void LanService::Cancel(){Drop(inbound_,nullptr);Drop(outbound_,nullptr);if(approved_){Close(approved_->socket);approved_.reset();}}
bool LanService::Request(const std::string& ip,uint16_t port,const std::string& expectedId){
    if(busy_||outbound_||approved_||!port){Event(AppEvent::Kind::LanStatus,"Connection request refused: busy");return false;}
    if(!expectedId.empty()){auto it=peers_.find(expectedId);if(it==peers_.end()||it->second.ip!=ip||it->second.port!=port||Clock::now()-it->second.seen>std::chrono::seconds(8)){Event(AppEvent::Kind::LanStatus,"Selected device is offline or changed; refresh");return false;}}
    if(inbound_){
        if(inbound_->prompt){
            // A queued click can arrive just after the peer's greeting. Apply the
            // same election as two already-outstanding requests, only for the
            // explicitly selected instance (never substitute an arbitrary peer).
            if(expectedId.empty()||inbound_->peerId!=expectedId){Event(AppEvent::Kind::LanStatus,"Connection request refused: busy");return false;}
            if(id_>inbound_->peerId){Event(AppEvent::Kind::LanStatus,"Simultaneous request: using inbound approval");return true;}
            Drop(inbound_,"Simultaneous request: keeping outbound");
        }else if(inbound_->accepted||!inbound_->output.empty()){Event(AppEvent::Kind::LanStatus,"Connection request refused: busy");return false;}
    }
    sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_port=htons(port);if(inet_pton(AF_INET,ip.c_str(),&remote.sin_addr)!=1)return false;
    Candidate c;c.outgoing=true;c.ip=ip;c.expectedId=expectedId;c.nonce=RandomId();c.deadline=Clock::now()+std::chrono::seconds(15);
    c.output="FTA-C1|"+id_+"|"+c.nonce+"|"+name_+"\n";
    c.socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(c.socket==INVALID_SOCKET||!Nonblock(c.socket)){Close(c.socket);return false;}
    if(connect(c.socket,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))){if(WSAGetLastError()!=WSAEWOULDBLOCK){Close(c.socket);Event(AppEvent::Kind::LanStatus,"Connection request failed");return false;}c.connecting=true;}
    outbound_=std::move(c);Event(AppEvent::Kind::LanStatus,"Awaiting peer connection approval");return true;
}
void LanService::Decide(uint64_t token,bool accept){if(!inbound_||!inbound_->prompt||inbound_->token!=token)return;auto& c=*inbound_;c.prompt=false;c.accepted=accept;c.output=std::string(accept?"FTA-A1|":"FTA-N1|")+id_+"|"+c.nonce+"\n";Event(AppEvent::Kind::Consent,"",0);}
void LanService::Advance(std::optional<Candidate>& candidate){
    if(!candidate)return;auto& c=*candidate;
    if(Clock::now()>=c.deadline){Drop(candidate,"Connection request timed out");return;}
    if(c.connecting){fd_set write,errors;FD_ZERO(&write);FD_ZERO(&errors);FD_SET(c.socket,&write);FD_SET(c.socket,&errors);timeval zero{};int ready=select(0,nullptr,&write,&errors,&zero);if(ready<=0)return;int error=0,len=sizeof(error);if(getsockopt(c.socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len)||error){Drop(candidate,"Connection request failed");return;}c.connecting=false;}
    if(!c.output.empty()){
        int sent=send(c.socket,c.output.data(),static_cast<int>(c.output.size()),0);
        if(sent>0)c.output.erase(0,sent);else if(sent==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)return;else{Drop(candidate,"Connection request closed");return;}
        if(!c.output.empty())return;
        if(!c.outgoing&&!c.prompt){if(c.accepted){approved_=std::move(candidate);candidate.reset();}else Drop(candidate,"Connection rejected");return;}
    }
    // Read only the gate line, leaving post-approval file protocol bytes in TCP.
    for(int i=0;i<256;++i){char ch;int n=recv(c.socket,&ch,1,0);if(n==0){Drop(candidate,"Connection request closed");return;}if(n<0){if(WSAGetLastError()==WSAEWOULDBLOCK)return;Drop(candidate,"Connection request failed");return;}
        if(c.prompt){Drop(candidate,"Commands before approval are forbidden");return;}
        if(ch!='\n'){c.input+=ch;if(c.input.size()>256){Drop(candidate,"Invalid connection greeting");return;}continue;}
        auto f=Split(c.input);c.input.clear();
        if(c.outgoing){if(f.size()!=3||(f[0]!="FTA-A1"&&f[0]!="FTA-N1")||!Hex(f[1])||f[2]!=c.nonce||(!c.expectedId.empty()&&c.expectedId!=f[1])){Drop(candidate,"Invalid connection approval");return;}
            if(f[0]=="FTA-N1"){Drop(candidate,"Connection rejected");return;}
            approved_=std::move(candidate);candidate.reset();return;
        }
        if(c.prompt){Drop(candidate,"Commands before approval are forbidden");return;}
        if(f.size()!=4||f[0]!="FTA-C1"||!Hex(f[1])||f[1]==id_||!Hex(f[2])||!Name(f[3])){Drop(candidate,"Invalid connection greeting");return;}
        if(outbound_){if(id_<f[1]){Drop(candidate,"Simultaneous request: keeping outbound");return;}Drop(outbound_,"Simultaneous request: using inbound approval");}
        c.peerId=f[1];c.nonce=f[2];c.prompt=true;c.token=++nextToken_;
        Event(AppEvent::Kind::Consent,f[3]+" (unverified) / "+c.ip,c.token);return;
    }
}
void LanService::Poll(bool busy){
    busy_=busy;auto now=Clock::now();if(now>=nextSearch_){SendDiscovery('Q');lastSearch_=now;nextSearch_=now+std::chrono::seconds(3);}
    for(int i=0;udp_!=INVALID_SOCKET&&i<64;++i){char packet[257];sockaddr_in source{};int len=sizeof(source);int n=recvfrom(udp_,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&source),&len);if(n<0){if(WSAGetLastError()==WSAEMSGSIZE)continue;break;}if(n>0&&n<=256)Datagram(std::string_view(packet,n),source);}
    bool changed=false;for(auto it=peers_.begin();it!=peers_.end();){if(now-it->second.seen>std::chrono::seconds(8)){it=peers_.erase(it);changed=true;}else ++it;}if(changed)Snapshot();
    for(int i=0;i<4;++i){sockaddr_in source{};int len=sizeof(source);SOCKET s=accept(listener_,reinterpret_cast<sockaddr*>(&source),&len);if(s==INVALID_SOCKET)break;if(busy||inbound_||approved_||!Nonblock(s)){closesocket(s);continue;}inbound_.emplace();inbound_->socket=s;inbound_->ip=Address(source);inbound_->deadline=now+std::chrono::seconds(15);}
    Advance(inbound_);Advance(outbound_);
}
std::optional<LanService::Approved> LanService::TakeApproved(){if(!approved_)return {};auto c=std::move(*approved_);approved_.reset();return Approved{c.socket,!c.outgoing};}
