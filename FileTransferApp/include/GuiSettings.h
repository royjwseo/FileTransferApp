#pragma once
#include "TransferStorage.h"
#include "FileName.h"
#include <shlobj.h>
#include <fstream>
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"shell32.lib")

namespace GuiSettings {
inline std::filesystem::path Known(REFKNOWNFOLDERID id) {
    PWSTR value=nullptr;
    if(FAILED(SHGetKnownFolderPath(id,0,nullptr,&value)))throw std::runtime_error("Cannot locate Windows user folder");
    std::filesystem::path path(value);CoTaskMemFree(value);return path;
}
inline std::filesystem::path SettingsFile() {
    wchar_t overridePath[32768]{};
    DWORD n=GetEnvironmentVariableW(L"FILETRANSFER_GUI_SETTINGS",overridePath,32768);
    if(n){if(n>=32768||!std::filesystem::path(overridePath).is_absolute())throw std::runtime_error("Settings file must be absolute");return overridePath;}
    return Known(FOLDERID_LocalAppData)/L"FileTransferApp"/L"download-folder.txt";
}
inline std::filesystem::path Load() {
    auto file=SettingsFile();
    if(!std::filesystem::exists(file))return Known(FOLDERID_Downloads);
    std::ifstream in(file,std::ios::binary);
    std::string bytes(65537,'\0');in.read(bytes.data(),bytes.size());bytes.resize(static_cast<size_t>(in.gcount()));
    if(in.bad()||bytes.empty()||bytes.size()>65536)throw std::runtime_error("Invalid download settings");
    if(bytes.starts_with("\xef\xbb\xbf"))bytes.erase(0,3);
    if(bytes.find('\0')!=std::string::npos)throw std::runtime_error("Invalid settings path");
    auto path=std::filesystem::path(FileName::ToWide(bytes));
    if(!path.is_absolute())throw std::runtime_error("Download folder must be absolute");return path;
}
inline void Save(const std::filesystem::path& folder) {
    auto file=SettingsFile();std::filesystem::create_directories(file.parent_path());
    auto temporary=file;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L".new";
    OwnedHandle out(CreateFileW(temporary.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!out)throw std::runtime_error("Cannot create download settings");
    auto bytes=FileName::ToUtf8(folder.wstring());DWORD written=0;
    bool ok=WriteFile(out.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(out.value);
    out.Reset();
    if(!ok||!MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(temporary.c_str());throw std::runtime_error("Cannot save download settings");
    }
}
}
