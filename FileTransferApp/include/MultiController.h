#pragma once
// Included after the existing per-connection Controller and GUI command definitions.
// Reuses Controller/Network/Protocol rather than duplicating the transfer state machine.
class MultiController {
public:
    explicit MultiController(std::shared_ptr<EventQueue> events):events_(std::move(events)),worker_([this]{Loop();}){}
    ~MultiController(){Close();if(worker_.joinable())worker_.join();}
    bool Post(Command command){std::lock_guard<std::mutex> lock(mutex_);if(stop_||commands_.size()>=128)return false;commands_.push_back(std::move(command));return true;}
    void Close(){stop_=true;if(worker_.joinable())CancelSynchronousIo(worker_.native_handle());}
    bool Done()const{return done_;}
private:
    struct Session {
        std::string peerId,name,ip,folder;
        std::shared_ptr<EventQueue> events;
        std::unique_ptr<Controller> worker;
    };
    std::shared_ptr<EventQueue> events_;
    std::shared_ptr<PeerSettings> settings_=std::make_shared<PeerSettings>();
    std::unique_ptr<LanService> lan_;
    std::map<uint64_t,Session> sessions_;
    std::deque<Command> commands_;
    std::mutex mutex_;
    std::atomic<bool> stop_{false},done_{false};
    uint64_t nextSession_=0;
    std::filesystem::path base_;
    std::thread worker_;
    void Log(std::string text){AppEvent e;e.text=std::move(text);events_->Push(std::move(e));}
    void Configuration(){AppEvent e;e.kind=AppEvent::Kind::Configuration;e.peerId=settings_->peerId;e.name=settings_->name;e.total=settings_->server?1:0;e.text=settings_->valid?"":"Settings damaged: automatic server reception disabled";events_->Push(std::move(e));}
    void DefaultFolder(){AppEvent e;e.kind=AppEvent::Kind::Download;e.text=FileName::ToUtf8(base_.wstring());events_->Push(std::move(e));}
    void Removed(uint64_t id,Session& session){AppEvent e;e.kind=AppEvent::Kind::SessionRemoved;e.session=id;e.peerId=session.peerId;events_->Push(std::move(e));}
    void StopAll(){
        if(lan_)lan_->CancelMulti();
        for(auto& [id,s]:sessions_)s.worker->Close();
        for(auto& [id,s]:sessions_){s.worker.reset();Removed(id,s);}sessions_.clear();
    }
    void SetFolder(uint64_t id,Session& s){
        try{
            auto folder=settings_->server?settings_->Destination(base_,s.peerId):base_;
            s.folder=FileName::ToUtf8(folder.wstring());
            Command c;c.type=SetDestination;c.session=id;c.path=s.folder;c.id=settings_->server&&!settings_->paths.contains(s.peerId)?1:0;s.worker->Post(std::move(c));
            AppEvent e;e.kind=AppEvent::Kind::Download;e.session=id;e.peerId=s.peerId;e.name=s.folder;e.text=s.folder;events_->Push(std::move(e));
        }catch(const std::exception& e){Command c;c.type=SetDestination;s.worker->Post(std::move(c));Log(std::string("Peer destination unavailable: ")+e.what());}
    }
    void Adopt(LanService::Approved a){
        struct Guard {SOCKET socket;~Guard(){if(socket!=INVALID_SOCKET)closesocket(socket);}} guard{a.socket};
        if(sessions_.size()>=(settings_->server?LanService::MAX_CLIENTS:1)){Log("Connection limit reached");return;}
        for(auto& [id,s]:sessions_)if(s.peerId==a.peerId){Log("Duplicate active PeerId refused: "+a.peerId);return;}
        std::filesystem::path folder;
        try{folder=settings_->server?settings_->Destination(base_,a.peerId):base_;}catch(const std::exception& e){Log(e.what());}
        uint64_t id=++nextSession_;Session s;s.peerId=a.peerId;s.name=a.name;s.ip=a.ip;s.folder=FileName::ToUtf8(folder.wstring());s.events=std::make_shared<EventQueue>();
        AppEvent added;added.kind=AppEvent::Kind::SessionAdded;added.session=id;added.peerId=a.peerId;added.name=a.name;added.text=a.ip;added.transferId=s.folder;added.total=a.authenticated?1:0;events_->Push(std::move(added));
        s.worker=std::make_unique<Controller>(s.events,std::move(a),id,folder,settings_->server&&!settings_->paths.contains(s.peerId)&&!folder.empty());
        guard.socket=INVALID_SOCKET;sessions_.emplace(id,std::move(s));
    }
    void Process(Command c){
        if(c.type==ApplyProfile){
            settings_->Configure(c.id!=0,c.text);StopAll();lan_.reset();Configuration();lan_=std::make_unique<LanService>(events_,settings_);return;
        }
        if(c.type==RefreshDevices){if(lan_)lan_->Refresh();return;}
        if(c.type==RegisterPeer||c.type==ForgetPeer){
            if(c.type==RegisterPeer)settings_->Register(c.transferId,c.text,c.path);else settings_->Forget(c.transferId,c.text);
            if(lan_)lan_->RevokeMulti(c.transferId,c.text);
            Log(c.type==RegisterPeer?"Registered N:1 peer saved: "+c.transferId:"Registered N:1 peer removed: "+c.transferId);return;
        }
        if(c.type==ConsentAccept||c.type==ConsentReject){if(lan_)lan_->DecideMulti(c.id,c.type==ConsentAccept);return;}
        if(c.type==RequestConnect||c.type==DirectConsent){
            if(!settings_->server&&sessions_.empty()&&lan_){if(!lan_->RequestMulti(c.text,c.port,c.transferId,c.path))Log("Connection request refused: busy, invalid endpoint or role");}
            else Log("Connection request refused: role or existing server session");return;
        }
        if(c.type==ChangeFolder){
            auto folder=std::filesystem::path(FileName::ToWide(c.path));TransferStorage::ValidateDirectory(folder);GuiSettings::Save(folder);base_=std::move(folder);DefaultFolder();
            for(auto& [id,s]:sessions_)if(!settings_->server||!settings_->paths.contains(s.peerId))SetFolder(id,s);return;
        }
        auto i=sessions_.find(c.session);
        if(c.type==Disconnect&&i==sessions_.end()){if(lan_)lan_->CancelMulti();return;}
        if(i==sessions_.end()){Log("No active matching session; stale command ignored");return;}
        auto& s=i->second;
        if(c.type==ChangePeerFolder||c.type==ResetPeerFolder){
            if(!settings_->server)throw std::runtime_error("Per-peer destinations apply only to server reception");
            // Physical path validation belongs to this peer's worker. A slow
            // remote filesystem must not stall routing to every other client.
            settings_->SetPath(s.peerId,c.type==ResetPeerFolder?std::string():c.path);SetFolder(i->first,s);return;
        }
        if(c.type==Disconnect){s.worker->Close();return;}
        if(!s.worker->Post(std::move(c)))Log("Peer command queue is full or closing");
    }
    void Loop(){
        try{
            try{settings_->Load();}catch(const std::exception& e){Log(std::string("Settings error: ")+e.what());if(settings_->peerId.empty())throw;}
            try{base_=GuiSettings::Load();TransferStorage::ValidateDirectory(base_);}catch(const std::exception& e){Log(std::string("Default folder error: ")+e.what());base_.clear();}
            Configuration();DefaultFolder();lan_=std::make_unique<LanService>(events_,settings_);
            while(!stop_){
                for(auto i=sessions_.begin();i!=sessions_.end();){
                    for(auto& e:i->second.events->Drain()){e.peerId=i->second.peerId;e.session=i->first;events_->Push(std::move(e));}
                    if(i->second.worker->Done()){
                        for(auto& e:i->second.events->Drain()){e.peerId=i->second.peerId;e.session=i->first;events_->Push(std::move(e));}
                        Removed(i->first,i->second);i=sessions_.erase(i);
                    }else ++i;
                }
                lan_->PollMulti(sessions_.size());while(auto a=lan_->TakeMultiApproved())Adopt(std::move(*a));
                for(int count=0;count<32;++count){Command c;{std::lock_guard<std::mutex> lock(mutex_);if(commands_.empty())break;c=std::move(commands_.front());commands_.pop_front();}
                    try{Process(std::move(c));}catch(const std::exception& e){Log(std::string("Operation refused: ")+e.what());}
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }catch(const std::exception& e){Log(std::string("Multiclient controller stopped: ")+e.what());}
        StopAll();lan_.reset();done_=true;
    }
};
