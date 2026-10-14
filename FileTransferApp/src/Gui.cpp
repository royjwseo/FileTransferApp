#define NOMINMAX
#include "Network.h"
#include "Gui.h"
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
    TransferId, Resume, Query };
struct Command {
    int type = 0; std::string text, path, transferId;
    uint64_t id = 0, session = 0; uint16_t port = 9000;
};

class Controller {
public:
    explicit Controller(std::shared_ptr<EventQueue> events) : events_(std::move(events)), worker_([this]{ Loop(); }) {}
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
        receiver_=std::thread([this,command] {
            try {
                const bool connected=command.type==Host ? network_->StartServer(command.port,"0.0.0.0") : network_->Connect(command.text,command.port);
                if(connected)network_->ReceiveLoop();
                else if(!endingSession_&&!stop_)Emit(AppEvent::Kind::Connection,"Failed: connection/listen setup");
            }catch(const std::exception& e){Emit(AppEvent::Kind::Connection,std::string("Failed: ")+e.what());}
        });
    }
    void Action(Command c) {
        if (acting_) { Emit(AppEvent::Kind::Log,"An approval/message operation is still running."); return; }
        if (action_.joinable()) action_.join();
        acting_ = true;
        action_ = std::thread([this,c=std::move(c)] {
            try {
                bool ok = c.type == Accept ? network_->AcceptFile(c.id) :
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
    void Process(Command c) {
        if (c.type == Host || c.type == Connect) { Begin(c); return; }
        if (c.type == Reconnect) { if(endpoint_.type!=Host&&endpoint_.type!=Connect){Emit(AppEvent::Kind::Log,"No previous endpoint.");return;} Emit(AppEvent::Kind::Log,"Reconnecting to previous endpoint"); Begin(endpoint_); return; }
        if (c.type == Disconnect) { EndSession(); Emit(AppEvent::Kind::Connection,"Disconnected"); return; }
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
        else Action(std::move(c));
    }
    void Loop() {
        try {
            while (!stop_) {
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
        EndSession(); done_=true;
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
    HWND hwnd=nullptr; HINSTANCE instance=nullptr;
    std::shared_ptr<EventQueue> events=std::make_shared<EventQueue>();
    std::unique_ptr<Controller> controller;
    uint64_t session=0, incoming=0, outgoing=0;
    uint64_t selection=0, sendTerminal=0, receiveTerminal=0;
    bool closing=false, selectedValid=false;
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
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    }
    static LRESULT CALLBACK EditProc(HWND edit,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
        if(msg==WM_GETDLGCODE && l && reinterpret_cast<MSG*>(l)->wParam==VK_RETURN)
            return DefSubclassProc(edit,msg,w,l)|DLGC_WANTMESSAGE;
        if(msg==WM_KEYDOWN && w==VK_RETURN){SendMessageW(GetParent(edit),WM_COMMAND,SendText,0);return 0;}
        return DefSubclassProc(edit,msg,w,l);
    }
    void Create() {
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
        DragAcceptFiles(hwnd,TRUE); SetTimer(hwnd,1,50,nullptr);
        controller=std::make_unique<Controller>(events);
    }
    void Select(std::wstring file) {
        path=std::move(file);selectedValid=false;Set(Selected,L"파일 정보 확인 중...");
        Command c;c.type=Pick;c.path=FileName::ToUtf8(path);c.id=++selection;if(!controller->Post(std::move(c)))Log(L"작업 대기열이 가득 찼습니다.");
    }
    void CommandClick(int id) {
        if(closing)return;
        if(id==Pick){
            wchar_t buffer[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=hwnd;dialog.lpstrFile=buffer;dialog.nMaxFile=32768;
            dialog.lpstrFilter=L"모든 파일\0*.*\0\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(GetOpenFileNameW(&dialog))Select(buffer);return;
        }
        Command c;c.type=id;c.session=session;
        if(id==Host || id==Connect){
            const auto portText=Text(Get(Port));
            if(portText.empty()||portText.find_first_not_of(L"0123456789")!=std::wstring::npos){Log(L"포트는 1~65535입니다.");return;}
            unsigned long port=std::stoul(portText);if(!port||port>65535){Log(L"포트는 1~65535입니다.");return;}c.port=static_cast<uint16_t>(port);
            c.text=FileName::ToUtf8(Text(Get(Ip)));in_addr address{};
            if(id==Connect && inet_pton(AF_INET,c.text.c_str(),&address)!=1){Log(L"올바른 IPv4 주소를 입력하세요.");return;}
        } else if(id==SendText){c.text=FileName::ToUtf8(Text(Get(Message)));if(c.text.empty())return;Set(Message,L"");}
        else if(id==SendFile||id==Resume||id==Query){if(!selectedValid){Log(L"먼저 파일을 선택하세요.");return;}c.path=FileName::ToUtf8(path);c.transferId=FileName::ToUtf8(Text(Get(TransferId)));if(id!=SendFile&&c.transferId.size()!=64){Log(L"원래 전송 ID 64자리를 입력하세요.");return;}}
        else if(id==Accept||id==Reject||id==CancelReceive){c.id=incoming;if(!c.id){Log(L"활성 수신 요청이 없습니다.");return;}}
        else if(id==CancelSend){c.id=outgoing;}
        if(!controller->Post(std::move(c)))Log(L"작업 대기열이 가득 찼습니다.");
    }
    void Tick() {
        std::wstring textBatch;
        for(auto& e:events->Drain()) {
            if(e.kind==AppEvent::Kind::Selection){if(e.requestId!=selection)continue;path=Wide(e.text);selectedValid=true;Set(Selected,Wide(e.name)+L" — "+std::to_wstring(e.total)+L" bytes");continue;}
            if(e.kind==AppEvent::Kind::Connection && e.session>=session){if(e.session!=session||e.text=="Disconnected"){incoming=outgoing=0;sendTerminal=receiveTerminal=0;Set(Request,L"수신 요청 없음");}session=e.session;Set(Status,Wide(e.text));Log(Wide(e.text));}
            else if(e.session!=session)continue;
            else if(e.kind==AppEvent::Kind::Text){textBatch+=L"상대: "+Wide(e.text)+L"\r\n";if(textBatch.size()>32000){textBatch.erase(0,textBatch.size()-32000);if(!textBatch.empty()&&textBatch.front()>=0xdc00&&textBatch.front()<=0xdfff)textBatch.erase(0,1);}}
            else if(e.kind==AppEvent::Kind::Request){incoming=e.requestId;receiveTerminal=0;receiveMetrics.clear();receiveState.clear();Set(Request,Wide(e.name)+L" / "+std::to_wstring(e.total)+L" bytes / requestId="+std::to_wstring(incoming));Log(L"수신 요청: "+Wide(e.name));}
            else if(e.kind==AppEvent::Kind::Identity){outgoing=e.requestId;sendTerminal=0;sendMetrics.clear();sendState.clear();Set(TransferId,Wide(e.transferId));Log(L"전송 ID: "+Wide(e.transferId));}
            else if(e.kind==AppEvent::Kind::Progress){
                auto& p=e.progress;std::wostringstream text;
                text<<(p.sending?L"송신 ":L"수신 ")<<Wide(p.fileName)<<L" requestId="<<p.requestId<<L" "<<p.completed<<L"/"<<p.total<<L" bytes "<<p.percent<<L"% "<<p.mibPerSecond<<L" MiB/s "<<p.elapsedSeconds<<L"s";
                auto& metrics=p.sending?sendMetrics:receiveMetrics;metrics=text.str();
                if(p.requestId!=(p.sending?sendTerminal:receiveTerminal)){auto& state=p.sending?sendState:receiveState;state=p.sending?L"Sending data":L"Receiving data";}
                Set(p.sending?SendDetails:ReceiveDetails,metrics+L"\r\n"+(p.sending?sendState:receiveState));
                SendMessageW(Get(p.sending?SendProgress:ReceiveProgress),PBM_SETPOS,static_cast<WPARAM>(p.percent*10),0);
                if(p.requestId!=(p.sending?sendTerminal:receiveTerminal)){if(p.sending)outgoing=p.requestId;else incoming=p.requestId;}
            }
            else if(e.kind==AppEvent::Kind::State){
                auto& state=e.sending?sendState:receiveState;state=Wide(e.text);
                if(e.sending&&e.requestId==0&&e.text.find("Preparing")==0){sendMetrics.clear();outgoing=sendTerminal=0;}
                Set(e.sending?SendDetails:ReceiveDetails,(e.sending?sendMetrics:receiveMetrics)+L"\r\n"+Wide(e.name)+L" "+state);Log(state);
                if(!e.sending){receiveTerminal=e.requestId;incoming=0;Set(Request,L"수신 요청 없음");}
                else if(e.text.find("Completed")==0||e.text.find("Cancelled")==0||e.text.find("Failed")==0){sendTerminal=e.requestId;outgoing=0;}
            }            else if(e.kind==AppEvent::Kind::Log)Log(Wide(e.text));
        }
        if(!textBatch.empty())Log(std::move(textBatch));
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
int RunGui(HINSTANCE instance,int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    WNDCLASSEXW klass{sizeof(klass)};klass.hInstance=instance;klass.lpfnWndProc=WndProc;klass.lpszClassName=L"FileTransferAppGui";
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    if(!RegisterClassExW(&klass))return 1;
    Window window;window.instance=instance;
    HWND hwnd=CreateWindowExW(0,klass.lpszClassName,L"FileTransferApp — LAN 파일 전송",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,850,745,nullptr,nullptr,instance,&window);
    if(!hwnd)return 1;
    ShowWindow(hwnd,show);UpdateWindow(hwnd);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(hwnd,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    return 0;
}
