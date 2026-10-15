#pragma once
#include <windows.h>
#include <string>

// UI-thread-owned top-level window. Closing hides it; the main window owns
// connection lifetime and ultimately destroys this window.
class ChatWindow {
public:
    ~ChatWindow();
    void Create(HWND owner,HINSTANCE instance);
    void Open();
    HWND Handle() const {return hwnd_;}
    HWND Control(int id) const {return GetDlgItem(hwnd_,id);}
    void Append(const std::wstring& text);
    void SetConnected(bool connected);
    void SetPeerName(const std::wstring& name){peerName_=name;stateSet_=false;SetConnected(connected_);}
private:
    HWND hwnd_=nullptr,owner_=nullptr;
    HFONT font_=nullptr;
    UINT dpi_=96;
    bool composing_=false;
    bool suppressReturnChar_=false;
    bool connected_=false,stateSet_=false;
    std::wstring peerName_;
    int Scale(int value) const {return MulDiv(value,dpi_,96);}
    void Layout();
    void Font();
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK EditProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
};
