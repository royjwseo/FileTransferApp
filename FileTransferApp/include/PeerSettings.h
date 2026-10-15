#pragma once
#include "TrustedPeers.h"
#include <sstream>
#include <set>

// Worker-thread-only settings. The old download-folder.txt and .trusted are untouched.
class PeerSettings {
    std::set<std::string> consumedLegacyIps_;
    std::filesystem::path File(const wchar_t* suffix)const{auto p=GuiSettings::SettingsFile();p+=suffix;return p;}
    static std::string Decode(std::string_view s){
        if(s.size()%2||s.size()>65536||s.find_first_not_of("0123456789abcdef")!=s.npos)throw std::runtime_error("Invalid encoded setting");
        auto n=[](char c){return c<='9'?c-'0':c-'a'+10;};std::string out;
        for(size_t i=0;i<s.size();i+=2)out+=static_cast<char>((n(s[i])<<4)|n(s[i+1]));
        if(out.find('\0')!=out.npos)throw std::runtime_error("Invalid setting text");FileName::ToWide(out);return out;
    }
    static std::string Encode(const std::string& s){return TrustedPeers::Encode(reinterpret_cast<const unsigned char*>(s.data()),s.size());}
    void Save(){
        if(!valid)throw std::runtime_error("Multiclient settings are damaged; preserve the file and repair before saving");
        std::string plain="FTA-MULTI-1\n"+std::string(server?"S":"C")+"\n"+Encode(name)+"\n";
        for(auto& ip:consumedLegacyIps_)plain+="L|"+ip+"\n";
        for(auto& [id,p]:paths)plain+="P|"+id+"|"+Encode(p)+"\n";
        for(auto& [id,t]:keys)plain+="K|"+id+"|"+t.ip+"|"+t.key+"\n";
        if(plain.size()>1048576||paths.size()>256||keys.size()>256||consumedLegacyIps_.size()>1024)throw std::runtime_error("Multiclient settings capacity exceeded");
        DATA_BLOB in{static_cast<DWORD>(plain.size()),reinterpret_cast<BYTE*>(plain.data())},out{};
        if(!CryptProtectData(&in,L"FileTransferApp multiclient settings",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))throw std::runtime_error("Cannot protect multiclient settings");
        auto p=File(L".multiclient");std::filesystem::create_directories(p.parent_path());auto temp=p;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L".new";
        OwnedHandle h(CreateFileW(temp.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD written=0;
        bool ok=h&&WriteFile(h.value,out.pbData,out.cbData,&written,nullptr)&&written==out.cbData&&FlushFileBuffers(h.value);
        SecureZeroMemory(plain.data(),plain.size());LocalFree(out.pbData);h.Reset();
        if(!ok||!MoveFileExW(temp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());throw std::runtime_error("Cannot save multiclient settings");}
    }
public:
    struct Trust {std::string ip,key;};
    std::string peerId,name;bool server=false,valid=true;
    std::map<std::string,std::string> paths;
    std::map<std::string,Trust> keys;
    static bool Id(std::string_view id){return TrustedPeers::Hex(id,32);}
    static bool Name(const std::string& s){
        if(s.empty()||s.size()>96||s.find('|')!=s.npos||s.find_first_not_of(' ')==s.npos)return false;
        try{for(auto c:FileName::ToWide(s))if(c<32||c==127||(c>=0x202a&&c<=0x202e)||(c>=0x2066&&c<=0x2069))return false;return true;}catch(...){return false;}
    }
    void Load(){
        auto identity=File(L".peer-id");std::filesystem::create_directories(identity.parent_path());
        OwnedHandle created(CreateFileW(identity.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(created){peerId=TrustedPeers::Generate().substr(0,32);DWORD written=0;if(!WriteFile(created.value,peerId.data(),32,&written,nullptr)||written!=32||!FlushFileBuffers(created.value))throw std::runtime_error("Cannot persist PeerId");created.Reset();}
        else{std::ifstream in(identity,std::ios::binary);std::string bytes(33,'\0');in.read(bytes.data(),bytes.size());bytes.resize(static_cast<size_t>(in.gcount()));if(!in||!Id(bytes)){if(!Id(bytes))throw std::runtime_error("Invalid persistent PeerId; do not replace existing identity");}peerId=bytes;}
        wchar_t computer[256]{};DWORD n=256;GetComputerNameW(computer,&n);name=FileName::ToUtf8(computer);if(!Name(name))name="Windows PC";
        auto p=File(L".multiclient");if(!std::filesystem::exists(p))return;
        try{
            std::ifstream input(p,std::ios::binary);std::string bytes(1048577,'\0');input.read(bytes.data(),bytes.size());bytes.resize(static_cast<size_t>(input.gcount()));
            if(input.bad()||bytes.empty()||bytes.size()>1048576)throw std::runtime_error("Invalid multiclient settings size");
            DATA_BLOB in{static_cast<DWORD>(bytes.size()),reinterpret_cast<BYTE*>(bytes.data())},out{};
            if(!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))throw std::runtime_error("Cannot decrypt multiclient settings");
            std::string plain(reinterpret_cast<char*>(out.pbData),out.cbData);SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);
            std::istringstream lines(plain);std::string line,role,newName;std::getline(lines,line);if(line!="FTA-MULTI-1")throw std::runtime_error("Invalid settings version");
            std::getline(lines,role);if(role!="S"&&role!="C")throw std::runtime_error("Invalid saved role");std::getline(lines,line);newName=Decode(line);if(!Name(newName))throw std::runtime_error("Invalid saved display name");
            std::map<std::string,std::string> newPaths;std::map<std::string,Trust> newKeys;std::set<std::string> consumed;
            while(std::getline(lines,line)){
                if(line.starts_with("L|")){auto ip=line.substr(2);if(!TrustedPeers::Ip(ip)||consumed.contains(ip)||consumed.size()>=1024)throw std::runtime_error("Invalid legacy registration marker");consumed.insert(ip);continue;}
                if(line.size()<35||line[1]!='|'||line[34]!='|'||!Id(line.substr(2,32)))throw std::runtime_error("Invalid peer setting");auto id=line.substr(2,32),data=line.substr(35);
                if(line[0]=='P'){auto path=Decode(data);if(!std::filesystem::path(FileName::ToWide(path)).is_absolute()||newPaths.contains(id)||newPaths.size()>=256)throw std::runtime_error("Invalid peer path");newPaths[id]=path;}
                else if(line[0]=='K'){auto bar=data.find('|');if(bar==data.npos||!TrustedPeers::Ip(data.substr(0,bar))||!TrustedPeers::Hex(data.substr(bar+1))||newKeys.contains(id)||newKeys.size()>=256)throw std::runtime_error("Invalid peer trust");newKeys[id]={data.substr(0,bar),data.substr(bar+1)};}
                else throw std::runtime_error("Unknown peer setting");
            }
            for(auto& [id,t]:newKeys)consumed.insert(t.ip);
            if(consumed.size()>1024)throw std::runtime_error("Legacy registration marker capacity exceeded");
            name=newName;server=role=="S";paths=std::move(newPaths);keys=std::move(newKeys);consumedLegacyIps_=std::move(consumed);SecureZeroMemory(plain.data(),plain.size());
        }catch(...){valid=false;paths.clear();keys.clear();consumedLegacyIps_.clear();throw;}
    }
    void Configure(bool role,const std::string& display){if(!Name(display))throw std::runtime_error("Display name must be valid UTF-8, 1-96 bytes, without separators/control characters");auto oldRole=server;auto oldName=name;server=role;name=display;try{Save();}catch(...){server=oldRole;name=oldName;throw;}}
    void SetPath(const std::string& id,const std::string& p){if(!Id(id)||(!p.empty()&&!std::filesystem::path(FileName::ToWide(p)).is_absolute()))throw std::runtime_error("Invalid peer folder setting");auto old=paths;if(p.empty())paths.erase(id);else paths[id]=p;try{Save();}catch(...){paths=std::move(old);throw;}}
    void Register(const std::string& id,const std::string& ip,const std::string& key){if(!Id(id)||!TrustedPeers::Ip(ip)||!TrustedPeers::Hex(key))throw std::runtime_error("Invalid peer registration");auto old=keys;auto markers=consumedLegacyIps_;keys[id]={ip,key};consumedLegacyIps_.insert(ip);try{Save();}catch(...){keys=std::move(old);consumedLegacyIps_=std::move(markers);throw;}}
    void Forget(const std::string& id,const std::string& ip={}){if(!Id(id)||(!ip.empty()&&!TrustedPeers::Ip(ip)))throw std::runtime_error("Invalid peer revocation");auto old=keys;auto markers=consumedLegacyIps_;auto i=keys.find(id);if(i!=keys.end())consumedLegacyIps_.insert(i->second.ip);if(!ip.empty())consumedLegacyIps_.insert(ip);keys.erase(id);try{Save();}catch(...){keys=std::move(old);consumedLegacyIps_=std::move(markers);throw;}}
    std::string Key(const std::string& id)const{auto i=keys.find(id);return i==keys.end()?std::string():i->second.key;}
    std::string PeerAt(const std::string& ip)const{std::string found;for(auto& [id,t]:keys)if(t.ip==ip){if(!found.empty())throw std::runtime_error("Multiple registered peers at this IP; select a discovered server");found=id;}return found;}
    bool HasIp(const std::string& ip)const{if(consumedLegacyIps_.contains(ip))return true;for(auto& [id,t]:keys)if(t.ip==ip)return true;return false;}
    std::filesystem::path Destination(const std::filesystem::path& base,const std::string& id)const{
        if(!valid||!Id(id))throw std::runtime_error("Peer folder settings unavailable");auto i=paths.find(id);if(i!=paths.end())return std::filesystem::path(FileName::ToWide(i->second));
        if(base.empty()||!base.is_absolute())throw std::runtime_error("Choose a valid default download folder");return base/L"peers"/FileName::ToWide(id);
    }
};
