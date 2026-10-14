#define NOMINMAX
#include "Network.h"
#include "Gui.h"
#include "GuiSettings.h"
#include "LanService.h"
#include "FileName.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <atomic>
#include <deque>
#include <sstream>
#include <thread>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
enum Id { Ip=101, Port, Host, Connect, Disconnect, Reconnect, Status, Transcript,
    Message, SendText, Pick, Selected, SendFile, DropSelect, Request, Accept, Reject,
    CancelSend, CancelReceive, SendProgress, ReceiveProgress, SendDetails, ReceiveDetails,
    TransferId, Resume, Query, DownloadPath, ChangeFolder, ConnectionGroup, MessageGroup, FileGroup, ProgressGroup, LanGroup, Devices, RefreshDevices, RequestConnect, DirectConsent, ConsentText, ConsentAccept, ConsentReject, LanStatus };
struct Command {
    int type = 0; std::string text, path, transferId;
    uint64_t id = 0, session = 0; uint16_t port = 9000;
};

class Controller {
public:
    explicit Controller(std::shared_ptr<EventQueue> events, bool legacy) : events_(std::move(events)), legacy_(legacy), worker_([this]{ Loop(); }) {}
    ~Controller() { Close(); if (worker_.joinable()) worker_.join(); }
    bool Post(Command command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || commands_.size() >= 64) return false;
        commands_.push_back(std::move(command)); cv_.notify_one(); return true;
    }
    void Close() { stop_ = true; cv_.notify_all(); }
    bool Done() const { return done_; }
private:
    std::shared_ptr<EventQueue> events_;
    std::mutex mutex_; std::condition_variable cv_; std::deque<Command> commands_;
    std::atomic<bool> stop_{false}, done_{false}, sending_{false}, acting_{false};
    std::atomic<bool> preparationCancelled_{false};
    std::atomic<bool> endingSession_{false};
    std::unique_ptr<Network> network_;
    std::thread receiver_, sender_, action_;
    uint64_t session_ = 0;
    Command endpoint_;
    std::filesystem::path downloadFolder_;
    bool legacy_=false;
    std::unique_ptr<LanService> lan_;
    std::atomic<bool> legacyRunning_{false};
    void Adopt(LanService::Approved approved) {
        struct SocketGuard {SOCKET value;~SocketGuard(){if(value!=INVALID_SOCKET)closesocket(value);}} guard{approved.socket};
        EndSession();++session_;network_=std::make_unique<Network>();
        network_->SetEvents(events_,session_);
        if(!network_->Initialize()){Emit(AppEvent::Kind::Connection,"Failed: initialization");return;}
        guard.value=INVALID_SOCKET; // Network consumes the socket, including setup failures.
        if(!network_->AdoptApprovedSocket(approved.socket,approved.host)){Emit(AppEvent::Kind::Connection,"Failed: approved socket setup");Emit(AppEvent::Kind::LanStatus,"Connection request failed: socket setup");return;}
        endingSession_=false;
        receiver_=std::thread([this]{network_->ReceiveLoop();});
    }
    std::thread worker_; // Start only after every field used by Loop is initialized.
    void Emit(AppEvent::Kind kind, std::string text, bool sending = false) {
        AppEvent e; e.kind = kind; e.text = std::move(text); e.session = session_; e.sending = sending; events_->Push(std::move(e));
    }
    void EndSession() {
        endingSession_ = true;
        if (network_) network_->RequestStop();
        if (sender_.joinable()) sender_.join();
        if (action_.joinable()) action_.join();
        if (receiver_.joinable()) receiver_.join();
        if (network_) network_->Disconnect();
        network_.reset(); sending_ = acting_ = false;
    }
    void Begin(const Command& command) {
        EndSession(); endpoint_ = command; ++session_;
        network_ = std::make_unique<Network>();
        network_->SetEvents(events_, session_); network_->EnableInterruptibleSetup();
        if (!network_->Initialize()) { Emit(AppEvent::Kind::Connection,"Failed: Winsock initialization"); network_.reset(); return; }
        endingSession_ = false;
        Emit(AppEvent::Kind::Connection, command.type==Host ? "Waiting for peer" : "Connecting");
        legacyRunning_=true;
        receiver_=std::thread([this,command] {
            try {
                const bool connected=command.type==Host ? network_->StartServer(command.port,"0.0.0.0") : network_->Connect(command.text,command.port);
                if(connected)network_->ReceiveLoop();
                else if(!endingSession_&&!stop_)Emit(AppEvent::Kind::Connection,"Failed: connection/listen setup");
            }catch(const std::exception& e){Emit(AppEvent::Kind::Connection,std::string("Failed: ")+e.what());}
            legacyRunning_=false;
        });
    }
    void Action(Command c) {
        if (acting_) { Emit(AppEvent::Kind::Log,"An approval/message operation is still running."); return; }
        if (action_.joinable()) action_.join();
        acting_ = true;
        action_ = std::thread([this,c=std::move(c)] {
            try {
                bool ok = c.type == Accept ? network_->AcceptFile(c.id, std::filesystem::path(FileName::ToWide(c.path))) :
                    c.type == Reject ? network_->RejectFile(c.id) : network_->SendText(c.text);
                if (c.type == SendText && ok) Emit(AppEvent::Kind::Log, "Sent: " + c.text);
                if (!ok) Emit(AppEvent::Kind::Log,"Operation failed or request is no longer active.");
            } catch (const std::exception& e) { Emit(AppEvent::Kind::Log,std::string("Operation failed: ")+e.what()); }
            acting_ = false;
        });
    }
    void File(Command c) {
        if (sending_) { Emit(AppEvent::Kind::Log,"A file send is still running."); return; }
        if (sender_.joinable()) sender_.join();
        sending_ = true;
        preparationCancelled_ = false;
        Emit(AppEvent::Kind::State,"Preparing: hashing source",true);
        sender_ = std::thread([this,c=std::move(c)] {
            try {
                uint8_t mode = c.type == Resume ? 1 : c.type == Query ? 2 : 0;
                if (!network_->SendFile(c.path,c.transferId,mode,&preparationCancelled_)) {
                    const bool cancelled=preparationCancelled_||network_->OutgoingState()==Protocol::SendState::Cancelled;
                    Emit(AppEvent::Kind::State,cancelled?"Cancelled":"Failed: success not confirmed",true);
                }
            } catch (const std::exception& e) { Emit(AppEvent::Kind::State,std::string("Failed: ")+e.what(),true); network_->RequestStop(); }
            sending_ = false;
        });
    }
    void DownloadEvent() {
        AppEvent e;e.kind=AppEvent::Kind::Download;e.text=FileName::ToUtf8(downloadFolder_.wstring());events_->Push(std::move(e));
    }
    void Process(Command c) {
        if(c.type==RefreshDevices){if(lan_)lan_->Refresh();return;}
        if(c.type==ConsentAccept||c.type==ConsentReject){if(lan_)lan_->Decide(c.id,c.type==ConsentAccept);return;}
        if(c.type==RequestConnect||c.type==DirectConsent){
            if(lan_&&(!network_||!network_->IsConnected())&&!legacyRunning_)lan_->Request(c.text,c.port,c.transferId);
            else Emit(AppEvent::Kind::LanStatus,"Connection request refused: already connected or busy");return;
        }
        if(c.type==ChangeFolder) {
            try {
                auto folder=std::filesystem::path(FileName::ToWide(c.path));
                TransferStorage::ValidateDirectory(folder);GuiSettings::Save(folder);
                downloadFolder_=std::move(folder);DownloadEvent();
            }catch(const std::exception& e){Emit(AppEvent::Kind::Log,std::string("Download folder error: ")+e.what());}
            return;
        }
        if (c.type == Host || c.type == Connect) { if(legacy_)Begin(c);return; }
        if (c.type == Reconnect) { if(!legacy_){Emit(AppEvent::Kind::LanStatus,"Select a device and request approval again");return;} if(endpoint_.type!=Host&&endpoint_.type!=Connect){Emit(AppEvent::Kind::Log,"No previous endpoint.");return;} Emit(AppEvent::Kind::Log,"Reconnecting to previous endpoint"); Begin(endpoint_); return; }
        if (c.type == Disconnect) { if(lan_)lan_->Cancel();EndSession(); Emit(AppEvent::Kind::Connection,"Disconnected"); return; }
        if (c.type == Pick) {
            try {
                uint64_t size; auto file = OpenSource(std::filesystem::path(FileName::ToWide(c.path)),size);
                AppEvent e; e.kind=AppEvent::Kind::Selection; e.text=c.path; e.total=size;
                e.name=FileName::ToUtf8(std::filesystem::path(FileName::ToWide(c.path)).filename().wstring());e.requestId=c.id;
                if (!FileName::IsSafe(e.name)) throw std::runtime_error("Unsupported or unsafe filename");
                events_->Push(std::move(e));
            } catch (const std::exception& e) { Emit(AppEvent::Kind::Log,std::string("Cannot select file: ")+e.what()); }
            return;
        }
        if (!network_ || !network_->IsConnected() || c.session != session_) {
            Emit(AppEvent::Kind::Log,"Not connected, or command belongs to a previous session."); return;
        }
        if (c.type == CancelSend || c.type == CancelReceive) {
            if(c.type==CancelSend){preparationCancelled_=true;c.id=network_->OutgoingRequestId();}
            if (!c.id || !network_->CancelFile(c.id)) Emit(AppEvent::Kind::Log,"No matching active transfer to cancel.");
        } else if (c.type == SendFile || c.type == Resume || c.type == Query) File(std::move(c));
        else { if(c.type==Accept){ if(downloadFolder_.empty()){Emit(AppEvent::Kind::Log,"Choose a valid download folder first.");return;} c.path=FileName::ToUtf8(downloadFolder_.wstring()); } Action(std::move(c)); }
    }
    void Loop() {
        try {
            try {
                downloadFolder_=GuiSettings::Load();TransferStorage::ValidateDirectory(downloadFolder_);DownloadEvent();
            }catch(const std::exception& e){downloadFolder_.clear();DownloadEvent();Emit(AppEvent::Kind::Log,std::string("Download folder error: ")+e.what());}
            if(!legacy_){try{lan_=std::make_unique<LanService>(events_);}catch(const std::exception& e){Emit(AppEvent::Kind::LanStatus,std::string("LAN unavailable: ")+e.what());}}
            while (!stop_) {
                if(lan_){
                    lan_->Poll((network_&&network_->IsConnected())||legacyRunning_||sending_||acting_);
                    if(auto approved=lan_->TakeApproved()){lan_->Cancel();if(stop_)closesocket(approved->socket);else Adopt(*approved);}
                }
                Command c; bool found=false;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (commands_.empty()) cv_.wait_for(lock,std::chrono::milliseconds(100),[this]{return stop_ || !commands_.empty();});
                    if (stop_) break;
                    if (!commands_.empty()) { c=std::move(commands_.front()); commands_.pop_front(); found=true; }
                }
                if (found) Process(std::move(c));
            }
        } catch (const std::exception& e) { Emit(AppEvent::Kind::Log,std::string("Session error: ")+e.what()); }
        lan_.reset();EndSession(); done_=true;
    }
};

std::wstring Text(HWND control) {
    const int n = GetWindowTextLengthW(control);
    std::wstring value(n+1,L'\0'); GetWindowTextW(control,value.data(),n+1); value.resize(n); return value;
}
std::wstring Wide(const std::string& value) {
    try { return FileName::ToWide(value); } catch (...) { return L"[Invalid UTF-8 message]"; }
}
struct Window {
    HWND hwnd=nullptr; HINSTANCE instance=nullptr; bool legacy=false;
    std::vector<DiscoveredPeer> peers; uint64_t consentToken=0;
    std::shared_ptr<EventQueue> events=std::make_shared<EventQueue>();
    std::unique_ptr<Controller> controller;
    uint64_t session=0, incoming=0, outgoing=0;
    uint64_t selection=0, sendTerminal=0, receiveTerminal=0;
    bool closing=false, selectedValid=false, connected=false, connecting=false, sendBusy=false, pendingApproval=false;
    UINT dpi=96; HFONT font=nullptr; int scrollY=0; bool downloadReady=false, lanPending=false;
    std::wstring path;
    std::wstring sendMetrics, receiveMetrics, sendState, receiveState;
    HWND Get(int id) { return GetDlgItem(hwnd,id); }
    void Set(int id,const std::wstring& text) { SetWindowTextW(Get(id),text.c_str()); }
    void Log(std::wstring text) {
        text += L"\r\n";
        auto edit=Get(Transcript);
        if (GetWindowTextLengthW(edit)>60000) { SendMessageW(edit,EM_SETSEL,0,20000); SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L"")); }
        SendMessageW(edit,EM_SETSEL,-1,-1); SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(text.c_str()));
    }
    void Control(int id,const wchar_t* type,const wchar_t* text,int x,int y,int w,int h,DWORD extra=0) {
        HWND control=CreateWindowExW(type==std::wstring_view(L"EDIT")?WS_EX_CLIENTEDGE:0,type,text,
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|extra,x,y,w,h,hwnd,reinterpret_cast<HMENU>(INT_PTR(id)),instance,nullptr);
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font ? font : GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    }
    static LRESULT CALLBACK EditProc(HWND edit,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
        if(msg==WM_GETDLGCODE && l && reinterpret_cast<MSG*>(l)->wParam==VK_RETURN)
            return DefSubclassProc(edit,msg,w,l)|DLGC_WANTMESSAGE;
        if(msg==WM_KEYDOWN && w==VK_RETURN){SendMessageW(GetParent(edit),WM_COMMAND,SendText,0);return 0;}
        return DefSubclassProc(edit,msg,w,l);
    }
    ~Window(){controller.reset();if(font)DeleteObject(font);}
    int Scale(int n) const {return MulDiv(n,dpi,96);}
    void Layout() {
        if(!Get(Ip))return;
        RECT r{};GetClientRect(hwnd,&r);int w=MulDiv(r.right,96,dpi),h=MulDiv(r.bottom,96,dpi);int top=legacy?0:225;h-=top;
        const int extra=std::max(0,h-795),width=w-40;
        SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS};si.nMax=Scale(795+top)-1;si.nPage=r.bottom;si.nPos=scrollY;SetScrollInfo(hwnd,SB_VERT,&si,TRUE);si.fMask=SIF_POS;GetScrollInfo(hwnd,SB_VERT,&si);scrollY=si.nPos;
        auto move=[&](int id,int x,int y,int cw,int ch){MoveWindow(Get(id),Scale(x),Scale(y+top)-scrollY,Scale(cw),Scale(ch),TRUE);};
        if(!legacy){
            auto lane=[&](int id,int x,int y,int cw,int ch){MoveWindow(Get(id),Scale(x),Scale(y)-scrollY,Scale(cw),Scale(ch),TRUE);};
            lane(LanGroup,10,5,w-20,215);lane(Devices,25,28,w-210,83);lane(RefreshDevices,w-175,28,145,29);lane(RequestConnect,w-175,66,145,29);
            lane(LanStatus,25,115,w-50,26);lane(ConsentText,25,148,w-250,45);lane(ConsentAccept,w-215,153,90,30);lane(ConsentReject,w-115,153,90,30);
            lane(DirectConsent,280,254,150,29);
        }
        move(ConnectionGroup,10,5,w-20,93);move(Ip,25,29,170,27);move(Port,205,29,65,27);
        move(Host,280,29,100,29);move(Connect,390,29,75,29);move(Disconnect,475,29,100,29);move(Reconnect,585,29,90,29);move(Status,25,65,width,24);
        move(MessageGroup,10,105,w-20,225+extra);move(Transcript,25,128,width,155+extra);move(Message,25,291+extra,w-175,28);move(SendText,w-140,291+extra,115,28);
        int f=340+extra;move(FileGroup,10,f,w-20,188);move(Pick,25,f+25,105,29);move(SendFile,140,f+25,115,29);
        move(Selected,25,f+61,width,35);move(Request,25,f+101,w-245,36);move(Accept,w-210,f+100,85,29);move(Reject,w-115,f+100,85,29);
        move(DownloadPath,25,f+145,w-175,35);move(ChangeFolder,w-140,f+143,115,29);
        int p=538+extra;move(ProgressGroup,10,p,w-20,247);move(CancelSend,25,p+25,105,29);move(CancelReceive,140,p+25,105,29);
        move(SendProgress,25,p+64,width,18);move(SendDetails,25,p+86,width,43);move(ReceiveProgress,25,p+132,width,18);move(ReceiveDetails,25,p+154,width,43);
        move(TransferId,25,p+205,w-270,28);move(Resume,w-235,p+205,100,29);move(Query,w-125,p+205,100,29);
    }
    void Font() {
        HFONT next=CreateFontW(-Scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for(HWND child=GetWindow(hwnd,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);
        if(font)DeleteObject(font);font=next;
    }
    void Buttons() {
        auto enable=[&](int id,bool on){EnableWindow(Get(id),on&&!closing);};
        enable(Host,legacy&&!connected&&!connecting);enable(Connect,legacy&&!connected&&!connecting);enable(Disconnect,connected||connecting||lanPending);enable(Reconnect,legacy&&!connected&&!connecting);
        enable(RequestConnect,!connected&&!lanPending&&consentToken==0);enable(DirectConsent,!connected&&!lanPending&&consentToken==0);enable(ConsentAccept,!connected&&consentToken!=0);enable(ConsentReject,consentToken!=0);
        enable(SendText,connected);enable(SendFile,connected&&selectedValid&&!sendBusy);enable(Resume,connected&&selectedValid&&!sendBusy);enable(Query,connected&&selectedValid&&!sendBusy);
        enable(Accept,connected&&pendingApproval&&downloadReady);enable(Reject,connected&&pendingApproval);enable(CancelReceive,connected&&incoming!=0);enable(CancelSend,connected&&sendBusy);
    }
    void Create() {
        dpi=GetDpiForWindow(hwnd);
        Control(LanGroup,L"BUTTON",L"LAN 기기 · 발견된 이름은 인증된 신원이 아닙니다",0,0,0,0,BS_GROUPBOX);
        Control(Devices,L"LISTBOX",L"",0,0,0,0,LBS_NOTIFY|WS_VSCROLL|WS_BORDER);
        Control(RefreshDevices,L"BUTTON",L"새로고침",0,0,0,0);Control(RequestConnect,L"BUTTON",L"선택 기기 연결 요청",0,0,0,0);
        Control(DirectConsent,L"BUTTON",L"IP 직접 연결 요청",0,0,0,0);
        Control(ConsentText,L"STATIC",L"연결 요청 없음",0,0,0,0);
        Control(ConsentAccept,L"BUTTON",L"연결 승인",0,0,0,0);Control(ConsentReject,L"BUTTON",L"연결 거절",0,0,0,0);
        Control(LanStatus,L"STATIC",L"LAN 시작 중",0,0,0,0);
        Control(ConnectionGroup,L"BUTTON",L"연결 · IPv4 / 포트",0,0,0,0,BS_GROUPBOX);
        Control(MessageGroup,L"BUTTON",L"메시지",0,0,0,0,BS_GROUPBOX);
        Control(FileGroup,L"BUTTON",L"파일 전송 · 한 번에 파일 하나",0,0,0,0,BS_GROUPBOX);
        Control(ProgressGroup,L"BUTTON",L"진행 상태 · 저장 ACK 확인 후 송신 성공",0,0,0,0,BS_GROUPBOX);
        Control(Ip,L"EDIT",L"127.0.0.1",15,15,170,25);
        Control(Port,L"EDIT",L"9000",195,15,65,25,ES_NUMBER);
        SendMessageW(Get(Port),EM_SETLIMITTEXT,5,0);SendMessageW(Get(Ip),EM_SETLIMITTEXT,15,0);
        Control(Host,L"BUTTON",L"연결 대기",275,15,95,27); Control(Connect,L"BUTTON",L"연결",380,15,75,27);
        Control(Disconnect,L"BUTTON",L"연결 해제",465,15,95,27); Control(Reconnect,L"BUTTON",L"재접속",570,15,85,27);
        Control(Status,L"STATIC",L"연결 안 됨",15,50,800,25);
        Control(Transcript,L"EDIT",L"",15,80,800,210,ES_MULTILINE|ES_READONLY|WS_VSCROLL|ES_AUTOVSCROLL);
        SendMessageW(Get(Transcript),EM_SETLIMITTEXT,80000,0);
        Control(Message,L"EDIT",L"",15,300,680,27,ES_AUTOHSCROLL); Control(SendText,L"BUTTON",L"메시지 전송",705,300,110,27);
        SendMessageW(Get(Message),EM_SETLIMITTEXT,4096,0);SetWindowSubclass(Get(Message),EditProc,1,0);
        Control(Pick,L"BUTTON",L"파일 선택",15,345,100,28); Control(SendFile,L"BUTTON",L"파일 보내기",125,345,110,28);
        Control(Selected,L"STATIC",L"파일을 선택하거나 하나의 파일을 창에 놓으세요.",15,380,800,38);
        Control(Request,L"STATIC",L"수신 요청 없음",15,425,600,35);
        Control(Accept,L"BUTTON",L"승인",625,425,85,28); Control(Reject,L"BUTTON",L"거절",720,425,85,28);
        Control(CancelSend,L"BUTTON",L"송신 취소",15,470,100,28);Control(CancelReceive,L"BUTTON",L"수신 취소",125,470,100,28);
        Control(SendProgress,PROGRESS_CLASSW,L"",15,510,800,20);Control(SendDetails,L"STATIC",L"송신 대기",15,535,800,40);
        Control(ReceiveProgress,PROGRESS_CLASSW,L"",15,580,800,20);Control(ReceiveDetails,L"STATIC",L"수신 대기",15,605,800,40);
        Control(TransferId,L"EDIT",L"",15,660,565,25,ES_AUTOHSCROLL); SendMessageW(Get(TransferId),EM_SETLIMITTEXT,64,0);
        Control(Resume,L"BUTTON",L"이어받기",590,660,100,28);Control(Query,L"BUTTON",L"완료 조회",700,660,110,28);
        for(int id:{SendProgress,ReceiveProgress})SendMessageW(Get(id),PBM_SETRANGE32,0,1000);
        Control(DownloadPath,L"STATIC",L"다운로드 폴더 확인 중...",0,0,0,0,SS_PATHELLIPSIS);
        Control(ChangeFolder,L"BUTTON",L"폴더 변경",0,0,0,0);
        Font();Layout();Buttons();
        DragAcceptFiles(hwnd,TRUE); SetTimer(hwnd,1,50,nullptr);
        controller=std::make_unique<Controller>(events,legacy);
        for(int id:{LanGroup,Devices,RefreshDevices,RequestConnect,DirectConsent,ConsentText,ConsentAccept,ConsentReject,LanStatus})ShowWindow(Get(id),legacy?SW_HIDE:SW_SHOW);
        if(!legacy){for(int id:{Host,Connect,Reconnect})ShowWindow(Get(id),SW_HIDE);SetWindowPos(Get(Disconnect),nullptr,Scale(475),Scale(254),Scale(100),Scale(29),SWP_NOZORDER);}
    }
    void Select(std::wstring file) {
        path=std::move(file);selectedValid=false;Set(Selected,L"파일 정보 확인 중...");
        Command c;c.type=Pick;c.path=FileName::ToUtf8(path);c.id=++selection;if(!controller->Post(std::move(c)))Log(L"작업 대기열이 가득 찼습니다.");
    }
    void CommandClick(int id) {
        if(closing)return;
        if(id==RequestConnect){
            int selected=static_cast<int>(SendMessageW(Get(Devices),LB_GETCURSEL,0,0));
            if(selected<0||selected>=static_cast<int>(peers.size())){Log(L"연결할 기기를 선택하세요.");return;}
            Command c;c.type=RequestConnect;c.text=peers[selected].ip;c.port=peers[selected].port;c.transferId=peers[selected].id;
            if(!controller->Post(std::move(c)))Log(L"작업 대기열이 가득 찼습니다.");return;
        }
        if(id==ConsentAccept||id==ConsentReject){Command c;c.type=id;c.id=consentToken;if(c.id)controller->Post(std::move(c));return;}
        if(id==ChangeFolder) {
            IFileOpenDialog* dialog=nullptr;
            if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
                DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
                dialog->SetTitle(L"다운로드 폴더 선택");
                if(SUCCEEDED(dialog->Show(hwnd))) {IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){Command c;c.type=ChangeFolder;c.path=FileName::ToUtf8(path);if(!controller->Post(std::move(c)))Log(L"작업 대기열이 가득 찼습니다.");CoTaskMemFree(path);}item->Release();}}
                dialog->Release();
            }else Log(L"폴더 선택 창을 열 수 없습니다.");return;
        }
        if(id==Pick){
            wchar_t buffer[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=hwnd;dialog.lpstrFile=buffer;dialog.nMaxFile=32768;
            dialog.lpstrFilter=L"모든 파일\0*.*\0\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(GetOpenFileNameW(&dialog))Select(buffer);return;
        }
        Command c;c.type=id;c.session=session;
        if(id==Host || id==Connect || id==DirectConsent){
            const auto portText=Text(Get(Port));
            if(portText.empty()||portText.find_first_not_of(L"0123456789")!=std::wstring::npos){Log(L"포트는 1~65535입니다.");return;}
            unsigned long port=std::stoul(portText);if(!port||port>65535){Log(L"포트는 1~65535입니다.");return;}c.port=static_cast<uint16_t>(port);
            c.text=FileName::ToUtf8(Text(Get(Ip)));in_addr address{};
            if((id==Connect||id==DirectConsent) && inet_pton(AF_INET,c.text.c_str(),&address)!=1){Log(L"올바른 IPv4 주소를 입력하세요.");return;}
        } else if(id==SendText){c.text=FileName::ToUtf8(Text(Get(Message)));if(c.text.empty())return;Set(Message,L"");}
        else if(id==SendFile||id==Resume||id==Query){if(!selectedValid){Log(L"먼저 파일을 선택하세요.");return;}c.path=FileName::ToUtf8(path);c.transferId=FileName::ToUtf8(Text(Get(TransferId)));if(id!=SendFile&&c.transferId.size()!=64){Log(L"원래 전송 ID 64자리를 입력하세요.");return;}}
        else if(id==Accept||id==Reject||id==CancelReceive){c.id=incoming;if(!c.id){Log(L"활성 수신 요청이 없습니다.");return;}}
        else if(id==CancelSend){c.id=outgoing;}
        if(!controller->Post(std::move(c)))Log(L"작업 대기열이 가득 찼습니다.");
    }
    void Tick() {
        std::wstring textBatch;
        for(auto& e:events->Drain()) {
            if(e.kind==AppEvent::Kind::Peers){
                std::string chosen;int old=static_cast<int>(SendMessageW(Get(Devices),LB_GETCURSEL,0,0));if(old>=0&&old<static_cast<int>(peers.size()))chosen=peers[old].id;
                peers=std::move(e.peers);SendMessageW(Get(Devices),LB_RESETCONTENT,0,0);
                for(size_t i=0;i<peers.size();++i){auto& p=peers[i];auto line=Wide(p.name)+L"  |  "+Wide(p.ip)+L":"+std::to_wstring(p.port)+L"  |  발견 (미인증) ["+Wide(p.id.substr(0,8))+L"]";SendMessageW(Get(Devices),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(line.c_str()));if(p.id==chosen)SendMessageW(Get(Devices),LB_SETCURSEL,i,0);}
                continue;
            }
            if(e.kind==AppEvent::Kind::Consent){consentToken=e.requestId;if(consentToken)lanPending=true;Set(ConsentText,e.text.empty()?L"연결 요청 없음":L"연결 요청: "+Wide(e.text));continue;}
            if(e.kind==AppEvent::Kind::LanStatus){lanPending=e.total!=0;Set(LanStatus,Wide(e.text));Log(Wide(e.text));continue;}
            if(e.kind==AppEvent::Kind::Download){downloadReady=!e.text.empty();Set(DownloadPath,downloadReady?Wide(e.text):L"저장 폴더를 선택하세요 (설정 오류)");continue;}
            if(e.kind==AppEvent::Kind::Selection){if(e.requestId!=selection)continue;path=Wide(e.text);selectedValid=true;Set(Selected,Wide(e.name)+L" — "+std::to_wstring(e.total)+L" bytes");continue;}
            if(e.kind==AppEvent::Kind::Connection && e.session>=session){if(e.session!=session||e.text=="Disconnected"){incoming=outgoing=0;sendTerminal=receiveTerminal=0;Set(Request,L"수신 요청 없음");}session=e.session;connected=e.text.starts_with("Connected:");if(connected||e.text=="Disconnected")lanPending=false;connecting=e.text=="Waiting for peer"||e.text=="Connecting";if(!connected){sendBusy=false;pendingApproval=false;}Set(Status,Wide(e.text));Log(Wide(e.text));}
            else if(e.session!=session)continue;
            else if(e.kind==AppEvent::Kind::Text){textBatch+=L"상대: "+Wide(e.text)+L"\r\n";if(textBatch.size()>32000){textBatch.erase(0,textBatch.size()-32000);if(!textBatch.empty()&&textBatch.front()>=0xdc00&&textBatch.front()<=0xdfff)textBatch.erase(0,1);}}
            else if(e.kind==AppEvent::Kind::Request){incoming=e.requestId;pendingApproval=true;receiveTerminal=0;receiveMetrics.clear();receiveState.clear();Set(Request,Wide(e.name)+L" / "+std::to_wstring(e.total)+L" bytes / requestId="+std::to_wstring(incoming));Log(L"수신 요청: "+Wide(e.name));}
            else if(e.kind==AppEvent::Kind::Identity){outgoing=e.requestId;sendTerminal=0;sendMetrics.clear();sendState.clear();Set(TransferId,Wide(e.transferId));Log(L"전송 ID: "+Wide(e.transferId));}
            else if(e.kind==AppEvent::Kind::Progress){
                auto& p=e.progress;if(!p.sending)pendingApproval=false;std::wostringstream text;
                text<<(p.sending?L"송신 ":L"수신 ")<<Wide(p.fileName)<<L" requestId="<<p.requestId<<L" "<<p.completed<<L"/"<<p.total<<L" bytes "<<p.percent<<L"% "<<p.mibPerSecond<<L" MiB/s "<<p.elapsedSeconds<<L"s";
                auto& metrics=p.sending?sendMetrics:receiveMetrics;metrics=text.str();
                if(p.requestId!=(p.sending?sendTerminal:receiveTerminal)){auto& state=p.sending?sendState:receiveState;state=p.sending?L"Sending data":L"Receiving data";}
                Set(p.sending?SendDetails:ReceiveDetails,metrics+L"\r\n"+(p.sending?sendState:receiveState));
                SendMessageW(Get(p.sending?SendProgress:ReceiveProgress),PBM_SETPOS,static_cast<WPARAM>(p.percent*10),0);
                if(p.requestId!=(p.sending?sendTerminal:receiveTerminal)){if(p.sending)outgoing=p.requestId;else incoming=p.requestId;}
            }
            else if(e.kind==AppEvent::Kind::State){
                auto& state=e.sending?sendState:receiveState;state=Wide(e.text);
                if(e.sending)sendBusy=!(e.text.starts_with("Completed")||e.text.starts_with("Cancelled")||e.text.starts_with("Failed"));
                if(e.sending&&e.requestId==0&&e.text.find("Preparing")==0){sendMetrics.clear();outgoing=sendTerminal=0;}
                Set(e.sending?SendDetails:ReceiveDetails,(e.sending?sendMetrics:receiveMetrics)+L"\r\n"+Wide(e.name)+L" "+state);Log(state);
                if(!e.sending){pendingApproval=false;receiveTerminal=e.requestId;incoming=0;Set(Request,L"수신 요청 없음");}
                else if(e.text.find("Completed")==0||e.text.find("Cancelled")==0||e.text.find("Failed")==0){sendTerminal=e.requestId;outgoing=0;}
            }            else if(e.kind==AppEvent::Kind::Log)Log(Wide(e.text));
        }
        if(!textBatch.empty())Log(std::move(textBatch));
        Buttons();
        if(closing && controller->Done()){KillTimer(hwnd,1);DestroyWindow(hwnd);}
    }
};
LRESULT CALLBACK WndProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    auto* window=reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE){window=static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);window->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(window));}
    if(!window)return DefWindowProcW(hwnd,message,w,l);
    try {
        switch(message){
        case WM_CREATE:window->Create();return 0;
        case WM_SIZE:window->Layout();return 0;
        case WM_VSCROLL:{SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(hwnd,SB_VERT,&si);int pos=si.nPos;switch(LOWORD(w)){case SB_LINEUP:pos-=window->Scale(25);break;case SB_LINEDOWN:pos+=window->Scale(25);break;case SB_PAGEUP:pos-=si.nPage;break;case SB_PAGEDOWN:pos+=si.nPage;break;case SB_THUMBTRACK:pos=si.nTrackPos;break;}window->scrollY=std::max(0,pos);window->Layout();return 0;}
        case WM_MOUSEWHEEL:window->scrollY=std::max(0,window->scrollY-MulDiv(GET_WHEEL_DELTA_WPARAM(w),window->Scale(60),WHEEL_DELTA));window->Layout();return 0;
        case WM_GETMINMAXINFO:{auto* limits=reinterpret_cast<MINMAXINFO*>(l);limits->ptMinTrackSize={window->Scale(850),window->Scale(450)};return 0;}
        case WM_DPICHANGED:{window->dpi=HIWORD(w);window->Font();auto* r=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);window->Layout();return 0;}
        case WM_COMMAND:if(HIWORD(w)==BN_CLICKED)window->CommandClick(LOWORD(w));return 0;
        case WM_TIMER:window->Tick();return 0;
        case WM_DROPFILES:{HDROP drop=reinterpret_cast<HDROP>(w);struct DropOwner{HDROP value;~DropOwner(){DragFinish(value);}} owner{drop};if(DragQueryFileW(drop,0xffffffff,nullptr,0)==1&&!window->closing){UINT size=DragQueryFileW(drop,0,nullptr,0);if(size>32768){window->Log(L"파일 경로가 너무 깁니다.");return 0;}std::wstring path(size+1,L'\0');DragQueryFileW(drop,0,path.data(),size+1);path.resize(size);window->Select(std::move(path));}else window->Log(L"한 번에 파일 하나를 놓으세요.");return 0;}
        case WM_CLOSE:if(!window->closing){window->closing=true;window->controller->Close();window->Set(Status,L"종료 중 — 작업 정리 대기");}return 0;
        case WM_DESTROY:PostQuitMessage(0);return 0;
        }
    }catch(const std::exception&){if(message==WM_CREATE)return -1;window->Log(L"입력을 처리할 수 없습니다.");return 0;}
    return DefWindowProcW(hwnd,message,w,l);
}
}
int RunGui(HINSTANCE instance,int show,bool legacy) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct ComOwner {HRESULT hr;~ComOwner(){if(SUCCEEDED(hr))CoUninitialize();}} comOwner{com};
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    WNDCLASSEXW klass{sizeof(klass)};klass.hInstance=instance;klass.lpfnWndProc=WndProc;klass.lpszClassName=L"FileTransferAppGui";
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    if(!RegisterClassExW(&klass))return 1;
    Window window;window.instance=instance;window.legacy=legacy;window.dpi=GetDpiForSystem();
    RECT desktop{};SystemParametersInfoW(SPI_GETWORKAREA,0,&desktop,0);
    HWND hwnd=CreateWindowExW(0,klass.lpszClassName,L"FileTransferApp — LAN 파일 전송",WS_OVERLAPPEDWINDOW|WS_VSCROLL,
        CW_USEDEFAULT,CW_USEDEFAULT,std::min(window.Scale(900),static_cast<int>(desktop.right-desktop.left)),std::min(window.Scale(850),static_cast<int>(desktop.bottom-desktop.top)),nullptr,nullptr,instance,&window);
    if(!hwnd)return 1;
    ShowWindow(hwnd,show);UpdateWindow(hwnd);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(hwnd,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    return 0;
}
