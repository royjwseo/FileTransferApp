#pragma once
#include "GuiSettings.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <map>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"crypt32.lib")

// Pair-specific random keys, protected for the current Windows user by DPAPI.
// IP selects the key; possession of the key, never discovery identity, authenticates.
namespace TrustedPeers {
inline bool Hex(std::string_view s,size_t n=64){return s.size()==n&&s.find_first_not_of("0123456789abcdef")==s.npos;}
inline std::string Encode(const unsigned char* bytes,size_t n){std::string s;for(size_t i=0;i<n;++i){s+="0123456789abcdef"[bytes[i]>>4];s+="0123456789abcdef"[bytes[i]&15];}return s;}
inline std::string Generate(){unsigned char b[32];if(BCryptGenRandom(nullptr,b,sizeof(b),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Cannot generate registration key");return Encode(b,sizeof(b));}
inline std::string Mac(const std::string& key,const std::string& data){
    if(!Hex(key))throw std::runtime_error("Invalid registration key");
    unsigned char bytes[32],digest[32];auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(size_t i=0;i<32;++i)bytes[i]=static_cast<unsigned char>((nibble(key[i*2])<<4)|nibble(key[i*2+1]));
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    NTSTATUS result=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if(result>=0)result=BCryptCreateHash(algorithm,&hash,nullptr,0,bytes,sizeof(bytes),0);
    if(result>=0)result=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),static_cast<ULONG>(data.size()),0);
    if(result>=0)result=BCryptFinishHash(hash,digest,sizeof(digest),0);
    if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);SecureZeroMemory(bytes,sizeof(bytes));
    if(result<0)throw std::runtime_error("Registration authentication failed");return Encode(digest,sizeof(digest));
}
inline bool Equal(std::string_view a,std::string_view b){if(a.size()!=b.size())return false;unsigned char d=0;for(size_t i=0;i<a.size();++i)d|=static_cast<unsigned char>(a[i]^b[i]);return d==0;}
inline bool Ip(const std::string& s){in_addr a{};return inet_pton(AF_INET,s.c_str(),&a)==1;}
class Store {
    std::map<std::string,std::string> keys_;
    std::filesystem::path Path()const{auto p=GuiSettings::SettingsFile();p+=L".trusted";return p;}
    void Save(const std::map<std::string,std::string>& keys){
        std::string plain="FTA-TRUST-1\n";for(auto& [ip,key]:keys)plain+=ip+"|"+key+"\n";
        DATA_BLOB input{static_cast<DWORD>(plain.size()),reinterpret_cast<BYTE*>(plain.data())},output{};
        if(!CryptProtectData(&input,L"FileTransferApp registered peers",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))throw std::runtime_error("Cannot protect registered peers");
        SecureZeroMemory(plain.data(),plain.size());
        auto file=Path();std::filesystem::create_directories(file.parent_path());auto temp=file;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L".new";
        OwnedHandle out(CreateFileW(temp.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD written=0;
        bool ok=out&&WriteFile(out.value,output.pbData,output.cbData,&written,nullptr)&&written==output.cbData&&FlushFileBuffers(out.value);
        LocalFree(output.pbData);out.Reset();
        if(!ok||!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());throw std::runtime_error("Cannot save registered peers");}
    }
public:
    void Load(){
        keys_.clear();auto p=Path();if(!std::filesystem::exists(p))return;
        std::ifstream in(p,std::ios::binary);std::string bytes(65537,'\0');in.read(bytes.data(),bytes.size());bytes.resize(static_cast<size_t>(in.gcount()));
        if(in.bad()||bytes.empty()||bytes.size()>65536)throw std::runtime_error("Invalid registered peers file");
        DATA_BLOB input{static_cast<DWORD>(bytes.size()),reinterpret_cast<BYTE*>(bytes.data())},output{};
        if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))throw std::runtime_error("Cannot decrypt registered peers");
        std::string plain(reinterpret_cast<char*>(output.pbData),output.cbData);SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);
        std::map<std::string,std::string> loaded;size_t start=12;
        if(!plain.starts_with("FTA-TRUST-1\n"))throw std::runtime_error("Invalid registered peers format");
        while(start<plain.size()){auto end=plain.find('\n',start),bar=plain.find('|',start);if(end==plain.npos||bar==plain.npos||bar>=end)throw std::runtime_error("Invalid registered peer");
            auto ip=plain.substr(start,bar-start),key=plain.substr(bar+1,end-bar-1);
            if(!Ip(ip)||!Hex(key)||loaded.contains(ip)||loaded.size()>=64)throw std::runtime_error("Invalid registered peer");loaded[ip]=key;start=end+1;
        }
        SecureZeroMemory(plain.data(),plain.size());keys_=std::move(loaded);
    }
    std::string Key(const std::string& ip)const{auto i=keys_.find(ip);return i==keys_.end()?std::string():i->second;}
    void Register(const std::string& ip,const std::string& key){if(!Ip(ip)||!Hex(key))throw std::runtime_error("Enter an IPv4 address and a 64-character lowercase registration key");auto next=keys_;if(!next.contains(ip)&&next.size()>=64)throw std::runtime_error("Registered peer limit reached");next[ip]=key;Save(next);keys_=std::move(next);}
    void Forget(const std::string& ip){auto next=keys_;next.erase(ip);Save(next);keys_=std::move(next);}
};
}
