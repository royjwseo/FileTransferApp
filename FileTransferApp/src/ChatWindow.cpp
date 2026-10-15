#define NOMINMAX
#include "ChatWindow.h"
#include <commctrl.h>
#include <algorithm>
#include <stdexcept>

namespace {
constexpr int Transcript=108,Message=109,SendText=110;
}
ChatWindow::~ChatWindow(){if(IsWindow(hwnd_))DestroyWindow(hwnd_);if(font_)DeleteObject(font_);}
void ChatWindow::Create(HWND owner,HINSTANCE instance){
    owner_=owner;
    WNDCLASSEXW klass{sizeof(klass)};klass.hInstance=instance;klass.lpfnWndProc=Proc;
    klass.lpszClassName=L"FileTransferAppChat";klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    if(!RegisterClassExW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Cannot register chat window");
    dpi_=GetDpiForWindow(owner);
    if(!CreateWindowExW(0,klass.lpszClassName,L"FileTransferApp — 대화",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,Scale(650),Scale(520),owner,nullptr,instance,this))throw std::runtime_error("Cannot create chat window");
}
void ChatWindow::Open(){ShowWindow(hwnd_,IsIconic(hwnd_)?SW_RESTORE:SW_SHOW);SetForegroundWindow(hwnd_);SetFocus(Control(Message));}
void ChatWindow::SetConnected(bool connected){if(stateSet_&&connected_==connected)return;connected_=connected;stateSet_=true;EnableWindow(Control(SendText),connected);auto title=(peerName_.empty()?L"FileTransferApp":peerName_)+std::wstring(connected?L" — 대화 (연결됨)":L" — 대화 (연결되지 않음)");SetWindowTextW(hwnd_,title.c_str());}
void ChatWindow::Append(const std::wstring& value){
    HWND edit=Control(Transcript);
    if(GetWindowTextLengthW(edit)>60000){SendMessageW(edit,EM_SETSEL,0,20000);SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L""));}
    auto line=value+L"\r\n";
    SendMessageW(edit,EM_SETSEL,-1,-1);SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(line.c_str()));
}
void ChatWindow::Font(){
    HFONT next=CreateFontW(-Scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for(int id:{Transcript,Message,SendText})SendMessageW(Control(id),WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);
    if(font_)DeleteObject(font_);font_=next;
}
void ChatWindow::Layout(){
    RECT client{};GetClientRect(hwnd_,&client);
    int margin=Scale(12),gap=Scale(8),input=Scale(85),button=Scale(100),row=std::max(margin,static_cast<int>(client.bottom)-margin-input);
    HRGN dirty=CreateRectRgn(0,0,0,0);
    auto place=[&](int id,int x,int y,int width,int height){
        HWND child=Control(id);RECT old{};GetWindowRect(child,&old);MapWindowPoints(HWND_DESKTOP,hwnd_,reinterpret_cast<POINT*>(&old),2);
        RECT next{x,y,x+std::max(1,width),y+std::max(1,height)};
        if(EqualRect(&old,&next))return;
        SetWindowPos(child,nullptr,next.left,next.top,next.right-next.left,next.bottom-next.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        for(const RECT& area:{old,next}){HRGN part=CreateRectRgnIndirect(&area);CombineRgn(dirty,dirty,part,RGN_OR);DeleteObject(part);}
    };
    place(Transcript,margin,margin,client.right-2*margin,row-margin-gap);
    place(Message,margin,row,client.right-2*margin-button-gap,input);
    place(SendText,client.right-margin-button,row,button,input);
    RedrawWindow(hwnd_,nullptr,dirty,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);DeleteObject(dirty);
}
LRESULT CALLBACK ChatWindow::EditProc(HWND edit,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
    auto* chat=reinterpret_cast<ChatWindow*>(data);
    if(msg==WM_IME_STARTCOMPOSITION)chat->composing_=true;
    if(msg==WM_IME_ENDCOMPOSITION)chat->composing_=false;
    if(msg==WM_GETDLGCODE && l && reinterpret_cast<MSG*>(l)->wParam==VK_RETURN)return DefSubclassProc(edit,msg,w,l)|DLGC_WANTMESSAGE;
    if(msg==WM_KEYDOWN && w==VK_RETURN){
        // WM_CHAR can arrive after Shift-up. Use the modifier state at key-down.
        chat->suppressReturnChar_=!chat->composing_ && !(GetKeyState(VK_SHIFT)&0x8000);
        if(chat->suppressReturnChar_){SendMessageW(chat->owner_,WM_COMMAND,SendText,reinterpret_cast<LPARAM>(chat->hwnd_));return 0;}
    }
    if(msg==WM_CHAR && w==L'\r' && chat->suppressReturnChar_)return 0;
    return DefSubclassProc(edit,msg,w,l);
}
LRESULT CALLBACK ChatWindow::Proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
    auto* chat=reinterpret_cast<ChatWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE){chat=static_cast<ChatWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);chat->hwnd_=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(chat));}
    if(!chat)return DefWindowProcW(hwnd,msg,w,l);
    switch(msg){
    case WM_CREATE:{
        auto instance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd,GWLP_HINSTANCE));
        auto edit=[&](int id,DWORD style){return CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS|ES_MULTILINE|WS_VSCROLL|style,0,0,0,0,hwnd,reinterpret_cast<HMENU>(INT_PTR(id)),instance,nullptr);};
        HWND transcript=edit(Transcript,ES_READONLY|ES_AUTOVSCROLL);
        HWND input=edit(Message,ES_WANTRETURN|ES_AUTOVSCROLL);
        HWND button=CreateWindowExW(0,L"BUTTON",L"전송\r\nEnter 전송\r\nShift+Enter 줄바꿈",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS|BS_MULTILINE,0,0,0,0,hwnd,reinterpret_cast<HMENU>(INT_PTR(SendText)),instance,nullptr);
        if(!transcript||!input||!button)return -1;
        SendMessageW(transcript,EM_SETLIMITTEXT,80000,0);SendMessageW(input,EM_SETLIMITTEXT,4096,0);
        SetWindowSubclass(input,EditProc,1,reinterpret_cast<DWORD_PTR>(chat));
        chat->dpi_=GetDpiForWindow(hwnd);chat->Font();chat->Layout();chat->SetConnected(false);return 0;
    }
    case WM_SIZE:if(w!=SIZE_MINIMIZED)chat->Layout();return 0;
    case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={chat->Scale(420),chat->Scale(300)};return 0;}
    case WM_DPICHANGED:{chat->dpi_=HIWORD(w);chat->Font();auto* r=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);chat->Layout();return 0;}
    case WM_COMMAND:if(LOWORD(w)==SendText&&HIWORD(w)==BN_CLICKED)SendMessageW(chat->owner_,WM_COMMAND,SendText,reinterpret_cast<LPARAM>(hwnd));return 0;
    case WM_CLOSE:ShowWindow(hwnd,SW_HIDE);return 0;
    case WM_DESTROY:chat->hwnd_=nullptr;return 0;
    }
    return DefWindowProcW(hwnd,msg,w,l);
}
