#define NOMINMAX
#include "LanService.h"
#include <charconv>

namespace {
std::atomic<uint64_t> ApprovalTokens{0}; // Never reused when the role/listener changes in this process.
std::vector<std::string> Fields(std::string_view s){std::vector<std::string> f;size_t start=0;while(f.size()<10){auto end=s.find('|',start);f.emplace_back(s.substr(start,end==s.npos?s.size()-start:end-start));if(end==s.npos)return f;start=end+1;}return {};}
void CloseMulti(SOCKET& s){if(s!=INVALID_SOCKET){closesocket(s);s=INVALID_SOCKET;}}
bool NonblockMulti(SOCKET s){u_long yes=1;return ioctlsocket(s,FIONBIO,&yes)==0;}
std::string IpMulti(const sockaddr_in& a){char b[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&a.sin_addr,b,sizeof(b));return b;}
}
LanService::LanService(std::shared_ptr<EventQueue> events,std::shared_ptr<PeerSettings> settings):LanService(std::move(events)){
    if(listen(listener_,static_cast<int>(MAX_CLIENTS)+4)==SOCKET_ERROR)throw std::runtime_error("Cannot configure multiclient listen backlog");
    settings_=std::move(settings);name_=settings_->name;std::string ips;for(auto ip:interfaces_){sockaddr_in a{};a.sin_addr=ip;if(!ips.empty())ips+=",";ips+=IpMulti(a);}
    Event(AppEvent::Kind::LanStatus,"N:1 ready: "+id_+" TCP="+std::to_string(port_)+" IP="+ips+" PeerId="+settings_->peerId+(settings_->server?" role=server":" role=client"));
}
void LanService::MultiDiscovery(char kind){
    if(udp_==INVALID_SOCKET)return;
    const auto packet="FTA-DISC|2|"+std::string(1,kind)+"|"+id_+"|"+std::to_string(port_)+"|"+settings_->name+"|"+(settings_->server?"S":"C")+"|"+settings_->peerId;
    for(auto ip:interfaces_)if(!setsockopt(udp_,IPPROTO_IP,IP_MULTICAST_IF,reinterpret_cast<char*>(&ip),sizeof(ip)))sendto(udp_,packet.data(),static_cast<int>(packet.size()),0,reinterpret_cast<sockaddr*>(&group_),sizeof(group_));
}
void LanService::MultiDatagram(std::string_view bytes,const sockaddr_in& source){
    auto f=Fields(bytes);if(f.size()!=8||f[0]!="FTA-DISC"||f[1]!="2"||(f[2]!="Q"&&f[2]!="R")||!PeerSettings::Id(f[3])||f[3]==id_||!PeerSettings::Name(f[5])||(f[6]!="S"&&f[6]!="C")||!PeerSettings::Id(f[7])||f[7]==settings_->peerId)return;
    unsigned port=0;auto [end,ec]=std::from_chars(f[4].data(),f[4].data()+f[4].size(),port);if(ec!=std::errc{}||end!=f[4].data()+f[4].size()||!port||port>65535)return;
    if(f[2]=="Q"){if(Clock::now()>=nextReply_){MultiDiscovery('R');nextReply_=Clock::now()+std::chrono::seconds(1);}return;}
    if((settings_->server&&f[6]!="C")||(!settings_->server&&f[6]!="S"))return;
    if(peers_.size()>=64&&!peers_.contains(f[3]))return;
    peers_[f[3]]={f[5],IpMulti(source),static_cast<uint16_t>(port),Clock::now(),f[7],f[6]};Snapshot();
}
std::string LanService::MultiKey(const std::string& peer,const std::string& ip){
    auto key=settings_->Key(peer);if(!key.empty())return key;
    // Existing IP registration can bind once after a successful full proof.
    return settings_->HasIp(ip)?std::string():trusted_.Key(ip);
}
void LanService::BindLegacyKey(Candidate& c){if(c.authenticated&&settings_->Key(c.peerId).empty())settings_->Register(c.peerId,c.ip,c.key);}
void LanService::CancelMulti(){for(auto& c:candidates_)if(c)CloseMulti(c->socket);candidates_.clear();for(auto& a:ready_)CloseMulti(a.socket);ready_.clear();}
void LanService::RevokeMulti(const std::string& peer,const std::string& ip){
    for(auto& c:candidates_)if(c&&(c->peerId==peer||(c->outgoing&&c->peerId.empty()&&c->ip==ip))){
        if(c->prompt){AppEvent e;e.kind=AppEvent::Kind::Consent;e.requestId=c->token;events_->Push(std::move(e));}CloseMulti(c->socket);c.reset();}
    std::erase_if(candidates_,[](const auto& c){return !c;});
    for(auto i=ready_.begin();i!=ready_.end();)if(i->peerId==peer){CloseMulti(i->socket);i=ready_.erase(i);}else ++i;
}
bool LanService::RequestMulti(const std::string& ip,uint16_t port,const std::string& instance,const std::string& peer){
    if(settings_->server||!candidates_.empty()||!ready_.empty()||!port||!TrustedPeers::Ip(ip))return false;
    Candidate c;c.outgoing=true;c.ip=ip;c.expectedId=instance;c.peerId=peer;
    if(c.peerId.empty())c.peerId=settings_->PeerAt(ip);
    c.key=MultiKey(c.peerId,ip);c.nonce=TrustedPeers::Generate().substr(0,32);c.deadline=Clock::now()+std::chrono::seconds(15);
    c.greeting="FTA-C3|"+id_+"|"+c.nonce+"|"+settings_->name+"|"+settings_->peerId+"|C|"+(c.key.empty()?"M":"K");c.output=c.greeting+"\n";
    sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_port=htons(port);inet_pton(AF_INET,ip.c_str(),&remote.sin_addr);
    c.socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(c.socket==INVALID_SOCKET||!NonblockMulti(c.socket)){CloseMulti(c.socket);return false;}
    if(connect(c.socket,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))){if(WSAGetLastError()!=WSAEWOULDBLOCK){CloseMulti(c.socket);Event(AppEvent::Kind::LanStatus,"Connection request failed");return false;}c.connecting=true;}
    candidates_.push_back(std::move(c));Event(AppEvent::Kind::LanStatus,"Awaiting N:1 server connection approval");return true;
}
void LanService::DecideMulti(uint64_t token,bool accept){
    for(auto& candidate:candidates_)if(candidate&&candidate->prompt&&candidate->token==token){auto& c=*candidate;c.prompt=false;c.accepted=accept;
        c.output=std::string(accept?"FTA-A3|":"FTA-N3|")+id_+"|"+c.nonce+"|"+settings_->name+"|"+settings_->peerId+"|S|-\n";
        AppEvent e;e.kind=AppEvent::Kind::Consent;e.requestId=token;events_->Push(std::move(e));return;}
}
std::optional<LanService::Approved> LanService::TakeMultiApproved(){if(ready_.empty())return {};auto a=std::move(ready_.front());ready_.erase(ready_.begin());return a;}
void LanService::AdvanceMulti(std::optional<Candidate>& candidate){
    if(!candidate)return;auto& c=*candidate;
    auto fail=[&](const char* reason){if(c.prompt){AppEvent e;e.kind=AppEvent::Kind::Consent;e.requestId=c.token;events_->Push(std::move(e));}CloseMulti(c.socket);candidate.reset();Event(AppEvent::Kind::LanStatus,reason);};
    if(Clock::now()>=c.deadline){fail("Connection request timed out");return;}
    if(c.connecting){fd_set write,errors;FD_ZERO(&write);FD_ZERO(&errors);FD_SET(c.socket,&write);FD_SET(c.socket,&errors);timeval zero{};if(select(0,nullptr,&write,&errors,&zero)<=0)return;int error=0,len=sizeof(error);if(getsockopt(c.socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len)||error){fail("Connection request failed");return;}c.connecting=false;}
    if(!c.output.empty()){
        int sent=send(c.socket,c.output.data(),static_cast<int>(c.output.size()),0);if(sent>0)c.output.erase(0,sent);else if(sent==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)return;else{fail("Connection request closed");return;}
        if(!c.output.empty())return;
        if(!c.outgoing&&!c.prompt&&c.authStage!=1){
            if(!c.accepted){fail("Connection rejected");return;}
            ready_.push_back({c.socket,true,c.peerId,c.displayName,c.ip,c.authenticated});c.socket=INVALID_SOCKET;candidate.reset();return;
        }
    }
    for(int i=0;i<512;++i){char ch;int n=recv(c.socket,&ch,1,0);if(n==0){fail("Connection request closed");return;}if(n<0){if(WSAGetLastError()==WSAEWOULDBLOCK)return;fail("Connection request failed");return;}
        if(c.prompt){fail("Commands before approval are forbidden");return;}
        if(ch!='\n'){c.input+=ch;if(c.input.size()>512){fail("Invalid N:1 greeting length");return;}continue;}
        auto line=std::move(c.input);c.input.clear();auto f=Fields(line);
        if(c.outgoing){
            if(!c.key.empty()&&c.authStage==0){
                if(f.size()!=7||f[0]!="FTA-H3"||!PeerSettings::Id(f[1])||!PeerSettings::Id(f[2])||!PeerSettings::Name(f[3])||!PeerSettings::Id(f[4])||f[5]!="S"||(!c.peerId.empty()&&c.peerId!=f[4])||(!c.expectedId.empty()&&c.expectedId!=f[1])){fail("N:1 authentication/version mismatch");return;}
                c.peerId=f[4];c.displayName=f[3];c.instanceId=f[1];c.authentication=c.greeting+"|"+f[1]+"|"+f[2]+"|"+f[3]+"|"+f[4]+"|S";
                if(!TrustedPeers::Equal(f[6],TrustedPeers::Mac(c.key,"server|"+c.authentication))){fail("Registered PC authentication failed: key mismatch");return;}
                c.output="FTA-P3|"+TrustedPeers::Mac(c.key,"client|"+c.authentication)+"\n";c.authStage=2;return;
            }
            if(f.size()!=7||(f[0]!="FTA-A3"&&f[0]!="FTA-N3")||!PeerSettings::Id(f[1])||f[2]!=c.nonce||!PeerSettings::Name(f[3])||!PeerSettings::Id(f[4])||f[5]!="S"||f[4]==settings_->peerId||(!c.peerId.empty()&&c.peerId!=f[4])||(!c.expectedId.empty()&&c.expectedId!=f[1])){fail("Invalid N:1 approval/version");return;}
            if(f[0]=="FTA-N3"){fail("Connection rejected");return;}
            if(!c.key.empty()&&(c.authStage!=2||f[1]!=c.instanceId||!TrustedPeers::Equal(f[6],TrustedPeers::Mac(c.key,"accepted|"+c.authentication)))){fail("Registered PC authentication failed: invalid completion");return;}
            if(c.key.empty()&&f[6]!="-"){fail("Invalid manual approval");return;}
            c.peerId=f[4];c.displayName=f[3];c.authenticated=!c.key.empty();BindLegacyKey(c);
            ready_.push_back({c.socket,false,c.peerId,c.displayName,c.ip,c.authenticated});c.socket=INVALID_SOCKET;candidate.reset();return;
        }
        if(c.authStage==1){
            if(f.size()!=2||f[0]!="FTA-P3"||!TrustedPeers::Equal(f[1],TrustedPeers::Mac(c.key,"client|"+c.authentication))){fail("Registered PC authentication failed: key mismatch or replay");return;}
            c.authenticated=true;BindLegacyKey(c);c.accepted=true;c.authStage=3;c.output="FTA-A3|"+id_+"|"+c.nonce+"|"+settings_->name+"|"+settings_->peerId+"|S|"+TrustedPeers::Mac(c.key,"accepted|"+c.authentication)+"\n";return;
        }
        if(f.size()!=7||f[0]!="FTA-C3"||!PeerSettings::Id(f[1])||f[1]==id_||!PeerSettings::Id(f[2])||!PeerSettings::Name(f[3])||!PeerSettings::Id(f[4])||f[4]==settings_->peerId||f[5]!="C"||(f[6]!="M"&&f[6]!="K")){fail("Unsupported N:1 greeting; use explicit v1 compatibility mode for older peers");return;}
        c.instanceId=f[1];c.nonce=f[2];c.displayName=f[3];c.peerId=f[4];c.greeting=line;
        if(f[6]=="K"){
            c.key=MultiKey(c.peerId,c.ip);if(c.key.empty()){fail("Registered PC authentication failed: PC is not registered");return;}
            auto body=id_+"|"+TrustedPeers::Generate().substr(0,32)+"|"+settings_->name+"|"+settings_->peerId+"|S";c.authentication=c.greeting+"|"+body;
            c.output="FTA-H3|"+body+"|"+TrustedPeers::Mac(c.key,"server|"+c.authentication)+"\n";c.authStage=1;return;
        }
        if(!settings_->Key(c.peerId).empty()){fail("Registered peer must authenticate; manual downgrade refused");return;}
        c.prompt=true;c.token=++ApprovalTokens;AppEvent e;e.kind=AppEvent::Kind::Consent;e.requestId=c.token;e.peerId=c.peerId;e.name=c.displayName;e.text=c.displayName+" / "+c.ip+" / PeerId="+c.peerId+" (unverified; approval grants this ID's files/recovery)";events_->Push(std::move(e));return;
    }
}
void LanService::PollMulti(size_t active){
    auto now=Clock::now();if(now>=nextSearch_){MultiDiscovery('Q');lastSearch_=now;nextSearch_=now+std::chrono::seconds(3);}
    for(int i=0;udp_!=INVALID_SOCKET&&i<64;++i){char bytes[513];sockaddr_in source{};int len=sizeof(source);int n=recvfrom(udp_,bytes,sizeof(bytes),0,reinterpret_cast<sockaddr*>(&source),&len);if(n<0){if(WSAGetLastError()==WSAEMSGSIZE)continue;break;}if(n>0&&n<=512)MultiDatagram(std::string_view(bytes,n),source);}
    bool changed=false;for(auto i=peers_.begin();i!=peers_.end();)if(now-i->second.seen>std::chrono::seconds(8)){i=peers_.erase(i);changed=true;}else ++i;if(changed)Snapshot();
    for(int i=0;i<4;++i){sockaddr_in source{};int len=sizeof(source);SOCKET socket=accept(listener_,reinterpret_cast<sockaddr*>(&source),&len);if(socket==INVALID_SOCKET)break;
        if(!settings_->server||active+candidates_.size()+ready_.size()>=MAX_CLIENTS||!NonblockMulti(socket)){closesocket(socket);Event(AppEvent::Kind::LanStatus,"Connection refused: role or connection limit");continue;}
        Candidate c;c.socket=socket;c.ip=IpMulti(source);c.deadline=now+std::chrono::seconds(15);candidates_.push_back(std::move(c));
    }
    for(auto& c:candidates_)try{AdvanceMulti(c);}catch(const std::exception& e){if(c){CloseMulti(c->socket);c.reset();}Event(AppEvent::Kind::LanStatus,std::string("Candidate failed: ")+e.what());}
    std::erase_if(candidates_,[](const auto& c){return !c;});
}
