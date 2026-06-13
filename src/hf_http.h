//---------------------------------------------------------------------------
// hf_http.h  –  HuggingFace Hub HTTP client (WinHTTP-based)
//
// Upload strategy:
//   Small files (< LFS_THRESHOLD):  base64-JSON commit
//   Large files (>= LFS_THRESHOLD): Git LFS 3-step flow
//     1. SHA-256 + size
//     2. POST .git/info/lfs/objects/batch  → presigned S3 URLs
//     3a. Multipart: PUT each part, collect ETags, POST completion
//     3b. Basic:     PUT single presigned URL
//     4. POST /commit/main with lfsFile entry
//---------------------------------------------------------------------------
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <functional>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#include "hf_debug.h"

static const LONGLONG LFS_THRESHOLD = 10LL * 1024 * 1024;  // 10 MB

namespace HfHttp {

// Last HTTP error body — set on any non-2xx response, readable by caller
inline std::string& LastErrorBody() { static std::string s; return s; }

// ── String utilities ──────────────────────────────────────────────────────
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}
inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        r.data(), n, nullptr, nullptr);
    return r;
}

inline std::wstring UrlEncodePathW(const std::wstring& s) {
    std::string u = WideToUtf8(s);
    std::string out;
    for (unsigned char c : u) {
        if (isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~'||c=='/') out+=(char)c;
        else { char b[4]; sprintf_s(b,"%%%02X",c); out+=b; }
    }
    return Utf8ToWide(out);
}

inline std::string Base64Encode(const BYTE* data, size_t len) {
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve(((len+2)/3)*4);
    for (size_t i=0;i<len;i+=3){
        DWORD v=(DWORD)data[i]<<16;
        if(i+1<len)v|=(DWORD)data[i+1]<<8;
        if(i+2<len)v|=(DWORD)data[i+2];
        out+=t[(v>>18)&63]; out+=t[(v>>12)&63];
        out+=(i+1<len)?t[(v>>6)&63]:'=';
        out+=(i+2<len)?t[v&63]:'=';
    }
    return out;
}

inline bool Sha256File(LPCWSTR path, BYTE outHash[32]) {
    BCRYPT_ALG_HANDLE hAlg=nullptr; BCRYPT_HASH_HANDLE hHash=nullptr; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_SHA256_ALGORITHM,nullptr,0)!=0) return false;
    DWORD objSz=0,used=0;
    BCryptGetProperty(hAlg,BCRYPT_OBJECT_LENGTH,(PBYTE)&objSz,sizeof(objSz),&used,0);
    std::vector<BYTE> obj(objSz);
    if(BCryptCreateHash(hAlg,&hHash,obj.data(),objSz,nullptr,0,0)!=0) goto done;
    {
        HANDLE hf=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,
                              OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(hf==INVALID_HANDLE_VALUE) goto done;
        std::vector<BYTE> buf(256*1024); DWORD rd=0;
        while(ReadFile(hf,buf.data(),(DWORD)buf.size(),&rd,nullptr)&&rd>0)
            BCryptHashData(hHash,buf.data(),rd,0);
        CloseHandle(hf);
    }
    if(BCryptFinishHash(hHash,outHash,32,0)==0) ok=true;
done:
    if(hHash) BCryptDestroyHash(hHash);
    if(hAlg)  BCryptCloseAlgorithmProvider(hAlg,0);
    return ok;
}

inline std::string BytesToHex(const BYTE* d,size_t len){
    static const char* h="0123456789abcdef";
    std::string s; s.reserve(len*2);
    for(size_t i=0;i<len;++i){s+=h[d[i]>>4];s+=h[d[i]&0xf];}
    return s;
}

inline std::string JsonEscape(const std::string& s){
    std::string r;
    for(char c:s){
        if(c=='"') r+="\\\"";
        else if(c=='\\') r+="\\\\";
        else if(c=='\n') r+="\\n";
        else if(c=='\r') r+="\\r";
        else r+=c;
    }
    return r;
}

// ── HTTP response ─────────────────────────────────────────────────────────
struct HttpResponse {
    int         statusCode = 0;
    std::string body;
    std::string etag;
    bool ok() const { return statusCode>=200 && statusCode<300; }
};

// ── WinHTTP session singleton ─────────────────────────────────────────────
inline HINTERNET& Session(){
    static HINTERNET h=WinHttpOpen(L"HuggingFace-VFS/1.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    return h;
}

// ── Parse URL ─────────────────────────────────────────────────────────────
struct ParsedUrl { std::wstring host,path; INTERNET_PORT port=443; bool secure=true; };
inline ParsedUrl ParseUrl(const std::wstring& url){
    ParsedUrl r;
    URL_COMPONENTSW uc={};  uc.dwStructSize=sizeof(uc);
    wchar_t sch[16]={},host[512]={},path[8192]={};
    uc.lpszScheme=sch;   uc.dwSchemeLength=15;
    uc.lpszHostName=host; uc.dwHostNameLength=511;
    uc.lpszUrlPath=path;  uc.dwUrlPathLength=8191;
    WinHttpCrackUrl(url.c_str(),0,0,&uc);
    r.host=host; r.path=path; r.port=uc.nPort;
    r.secure=(uc.nScheme==INTERNET_SCHEME_HTTPS);
    return r;
}

// ── Core HTTP: send to arbitrary URL, optionally stream from file ─────────
inline HttpResponse DoUrl(
    const std::wstring& method,
    const std::wstring& url,
    const std::vector<std::wstring>& extraHeaders = {},
    const void* bodyPtr = nullptr,
    DWORD bodyLen = 0,
    HANDLE hStream = INVALID_HANDLE_VALUE,
    LONGLONG streamOffset = 0,
    LONGLONG streamLen = 0)
{
    HttpResponse result;
    auto pu = ParseUrl(url);

    HfDebug::Log("HTTP " + WideToUtf8(method) + " host=" + WideToUtf8(pu.host) +
                 " path=" + WideToUtf8(pu.path).substr(0, 120));

    HINTERNET hConn = WinHttpConnect(Session(), pu.host.c_str(), pu.port, 0);
    if (!hConn) { HfDebug::Log("WinHttpConnect FAILED", (int)GetLastError()); return result; }

    DWORD flags = pu.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, method.c_str(), pu.path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        HfDebug::Log("WinHttpOpenRequest FAILED", (int)GetLastError());
        WinHttpCloseHandle(hConn); return result;
    }

    for (auto& h : extraHeaders)
        WinHttpAddRequestHeaders(hReq, h.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    bool sent = false;
    if (hStream != INVALID_HANDLE_VALUE) {
        // Stream file content in chunks via WinHttpWriteData
        DWORD totalLen = (DWORD)streamLen;
        HfDebug::Log("Streaming from file, offset", streamOffset);
        HfDebug::Log("Streaming bytes", (long long)totalLen);

        if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, totalLen, 0)) {
            HfDebug::Log("WinHttpSendRequest(stream) FAILED", (int)GetLastError());
            WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return result;
        }

        LARGE_INTEGER li; li.QuadPart = streamOffset;
        SetFilePointerEx(hStream, li, nullptr, FILE_BEGIN);

        std::vector<BYTE> buf(256 * 1024);
        LONGLONG remaining = streamLen;
        LONGLONG sent_bytes = 0;
        while (remaining > 0) {
            DWORD toRead = (DWORD)min((LONGLONG)buf.size(), remaining);
            DWORD didRead = 0;
            if (!ReadFile(hStream, buf.data(), toRead, &didRead, nullptr) || didRead == 0) {
                HfDebug::Log("ReadFile failed during stream, remaining", remaining);
                break;
            }
            DWORD written = 0;
            if (!WinHttpWriteData(hReq, buf.data(), didRead, &written)) {
                HfDebug::Log("WinHttpWriteData FAILED", (int)GetLastError());
                break;
            }
            remaining  -= didRead;
            sent_bytes += didRead;
        }
        HfDebug::Log("Stream sent bytes total", sent_bytes);
        sent = true;
    } else {
        sent = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  const_cast<void*>(bodyPtr), bodyLen, bodyLen, 0) != FALSE;
        if (!sent) HfDebug::Log("WinHttpSendRequest FAILED", (int)GetLastError());
    }

    if (!sent || !WinHttpReceiveResponse(hReq, nullptr)) {
        HfDebug::Log("WinHttpReceiveResponse FAILED", (int)GetLastError());
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return result;
    }

    DWORD sc=0, sz=sizeof(sc);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &sc, &sz, nullptr);
    result.statusCode = (int)sc;
    HfDebug::Log("Response status", (int)sc);

    // Grab ETag
    wchar_t etBuf[512]={}; DWORD etSz=sizeof(etBuf);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_ETAG, nullptr, etBuf, &etSz, nullptr))
        result.etag = WideToUtf8(etBuf);

    DWORD avail=0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail>0) {
        std::string chunk(avail,'\0'); DWORD rd=0;
        WinHttpReadData(hReq, chunk.data(), avail, &rd);
        result.body.append(chunk.data(), rd);
    }

    if (!result.ok() && !result.body.empty()) {
        LastErrorBody() = result.body;
        HfDebug::Log("Error body", result.body);
    }

    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn);
    return result;
}

// Convenience wrapper for HF API calls (huggingface.co)
inline HttpResponse DoApi(
    const std::wstring& apiToken,
    const std::string&  method,
    const std::wstring& path,
    const std::string*  jsonBody = nullptr)
{
    std::wstring url = L"https://huggingface.co" + path;
    std::vector<std::wstring> hdrs;
    if (!apiToken.empty())
        hdrs.push_back(L"Authorization: Bearer " + apiToken);
    if (jsonBody) {
        if (path.find(L"lfs/objects/batch") != std::wstring::npos) {
            hdrs.push_back(L"Accept: application/vnd.git-lfs+json");
            hdrs.push_back(L"Content-Type: application/vnd.git-lfs+json");
        } else {
            hdrs.push_back(L"Content-Type: application/json");
        }
    }
    return DoUrl(Utf8ToWide(method), url, hdrs,
                 jsonBody ? jsonBody->data() : nullptr,
                 jsonBody ? (DWORD)jsonBody->size() : 0);
}

// ── JSON helpers ──────────────────────────────────────────────────────────
inline std::string JsonGet(const std::string& json, const std::string& key){
    std::string search="\""+key+"\"";
    auto pos=json.find(search);
    if(pos==std::string::npos) return "";
    pos=json.find(':',pos+search.size());
    if(pos==std::string::npos) return "";
    ++pos;
    while(pos<json.size()&&(json[pos]==' '||json[pos]=='\t'))++pos;
    if(pos>=json.size()) return "";
    if(json[pos]=='"'){
        ++pos;
        auto end=json.find('"',pos);
        return end==std::string::npos?"":json.substr(pos,end-pos);
    }
    auto end=json.find_first_of(",}\r\n",pos);
    if(end==std::string::npos)end=json.size();
    std::string v=json.substr(pos,end-pos);
    while(!v.empty()&&(v.back()==' '||v.back()=='\t'))v.pop_back();
    return v;
}

// Extract a nested object block starting at the first '{' after key
inline std::string JsonGetBlock(const std::string& json, const std::string& key){
    std::string search="\""+key+"\"";
    auto pos=json.find(search);
    if(pos==std::string::npos) return "";
    pos=json.find(':',pos+search.size());
    if(pos==std::string::npos) return "";
    pos=json.find('{',pos);
    if(pos==std::string::npos) return "";
    int depth=0; size_t i=pos;
    for(;i<json.size();++i){
        if(json[i]=='{')++depth;
        else if(json[i]=='}'){--depth;if(depth==0)break;}
    }
    return json.substr(pos,i-pos+1);
}

// ── Download file to local path ───────────────────────────────────────────
inline bool DownloadFile(const std::wstring& apiToken,
                         const std::wstring& urlPath,
                         LPCWSTR localPath,
                         std::function<bool(LONGLONG,LONGLONG)> progress)
{
    std::wstring url = L"https://huggingface.co" + urlPath;
    std::vector<std::wstring> hdrs;
    if (!apiToken.empty())
        hdrs.push_back(L"Authorization: Bearer " + apiToken);

    auto pu = ParseUrl(url);
    HINTERNET hConn = WinHttpConnect(Session(), pu.host.c_str(), pu.port, 0);
    if (!hConn) return false;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", pu.path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConn); return false; }
    for (auto& h : hdrs)
        WinHttpAddRequestHeaders(hReq, h.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(hReq,WINHTTP_NO_ADDITIONAL_HEADERS,0,
                            WINHTTP_NO_REQUEST_DATA,0,0,0)||
        !WinHttpReceiveResponse(hReq,nullptr))
    { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false; }

    DWORD sc=0,sz=sizeof(sc);
    WinHttpQueryHeaders(hReq,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr,&sc,&sz,nullptr);
    if(sc<200||sc>=300){ WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false; }

    wchar_t clBuf[32]={}; DWORD clSz=sizeof(clBuf); LONGLONG total=-1;
    if(WinHttpQueryHeaders(hReq,WINHTTP_QUERY_CONTENT_LENGTH,nullptr,clBuf,&clSz,nullptr))
        total=_wtoll(clBuf);

    HANDLE hFile=CreateFileW(localPath,GENERIC_WRITE,0,nullptr,
                             CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(hFile==INVALID_HANDLE_VALUE){ WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return false; }

    LONGLONG received=0; DWORD avail=0; bool ok=true;
    while(WinHttpQueryDataAvailable(hReq,&avail)&&avail>0){
        std::vector<char> buf(avail); DWORD rd=0;
        if(!WinHttpReadData(hReq,buf.data(),avail,&rd)){ok=false;break;}
        DWORD wr=0; WriteFile(hFile,buf.data(),rd,&wr,nullptr);
        received+=rd;
        if(progress&&!progress(received,total)){ok=false;break;}
    }
    CloseHandle(hFile);
    if(!ok) DeleteFileW(localPath);
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn);
    return ok;
}

// ──────────────────────────────────────────────────────────────────────────
// UPLOAD: small file via base64-JSON commit
// ──────────────────────────────────────────────────────────────────────────
static bool UploadSmall(
    const std::wstring& apiToken,
    const std::string& repoType, const std::string& repoId,
    const std::string& pathInRepo, LPCWSTR localPath,
    std::function<bool(LONGLONG,LONGLONG)> progress)
{
    HfDebug::Log("UploadSmall", pathInRepo);
    HANDLE hf=CreateFileW(localPath,GENERIC_READ,FILE_SHARE_READ,
                          nullptr,OPEN_EXISTING,0,nullptr);
    if(hf==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fs; GetFileSizeEx(hf,&fs);
    std::vector<BYTE> data((size_t)fs.QuadPart);
    DWORD rd=0;
    bool ok=ReadFile(hf,data.data(),(DWORD)data.size(),&rd,nullptr);
    CloseHandle(hf);
    if(!ok||rd!=data.size()) return false;
    if(progress) progress(fs.QuadPart/2,fs.QuadPart);
    std::string b64=Base64Encode(data.data(),data.size());
    std::string body=
        "{\"summary\":\"Upload "+JsonEscape(pathInRepo)+" via HuggingFace VFS\","
        "\"files\":[{\"path\":\""+JsonEscape(pathInRepo)+"\","
        "\"content\":\""+b64+"\",\"encoding\":\"base64\"}]}";
    std::wstring apiPath=L"/api/"+Utf8ToWide(repoType+"s")+
                         L"/"+Utf8ToWide(repoId)+L"/commit/main";
    auto resp=DoApi(apiToken,"POST",apiPath,&body);
    HfDebug::Log("UploadSmall commit status",(int)resp.statusCode);
    if(progress) progress(fs.QuadPart,fs.QuadPart);
    return resp.ok();
}

// ──────────────────────────────────────────────────────────────────────────
// UPLOAD: large file via Git LFS
// ──────────────────────────────────────────────────────────────────────────
static bool UploadLfs(
    const std::wstring& apiToken,
    const std::string& repoType, const std::string& repoId,
    const std::string& pathInRepo, LPCWSTR localPath,
    std::function<bool(LONGLONG,LONGLONG)> progress)
{
    HfDebug::Log("=== UploadLfs START ===", pathInRepo);

    // 1. SHA-256 + size
    BYTE sha[32]={};
    if(!Sha256File(localPath,sha)){ HfDebug::Log("SHA256 FAILED"); return false; }
    std::string oid=BytesToHex(sha,32);
    HfDebug::Log("OID",oid);

    LARGE_INTEGER fs={}; {
        HANDLE hf=CreateFileW(localPath,GENERIC_READ,FILE_SHARE_READ,
                              nullptr,OPEN_EXISTING,0,nullptr);
        if(hf==INVALID_HANDLE_VALUE){ HfDebug::Log("Cannot open file"); return false; }
        GetFileSizeEx(hf,&fs); CloseHandle(hf);
    }
    LONGLONG fileSize=fs.QuadPart;
    HfDebug::Log("File size",fileSize);
    if(progress) progress(0,fileSize);

    // 2. LFS batch request
    std::string urlPrefix=(repoType=="dataset")?"/datasets/":"/";
    std::wstring batchPath=Utf8ToWide(urlPrefix+repoId+".git/info/lfs/objects/batch");
    std::string batchBody=
        "{\"operation\":\"upload\","
        "\"transfers\":[\"basic\",\"multipart\"],"
        "\"objects\":[{\"oid\":\""+oid+"\",\"size\":"+std::to_string(fileSize)+"}],"
        "\"hash_algo\":\"sha256\"}";

    HfDebug::Log("LFS batch path", WideToUtf8(batchPath));
    auto batchResp=DoApi(apiToken,"POST",batchPath,&batchBody);
    HfDebug::Log("LFS batch status",(int)batchResp.statusCode);
    HfDebug::Log("LFS batch body", batchResp.body);
    if(!batchResp.ok()) return false;

    // 3. Parse batch response
    // Find the objects array and the object matching our OID
    std::string actionsBlock = JsonGetBlock(batchResp.body, "actions");
    bool alreadyExists = actionsBlock.empty();
    HfDebug::Log("Already exists on server", alreadyExists ? "YES" : "NO");
    HfDebug::Log("Actions block", actionsBlock);

    if (!alreadyExists) {
        // Extract upload action
        std::string uploadBlock = JsonGetBlock(actionsBlock, "upload");
        if (uploadBlock.empty()) {
            // Fallback: maybe actions IS the upload block directly
            uploadBlock = actionsBlock;
        }
        HfDebug::Log("Upload block", uploadBlock);

        std::string uploadHref = JsonGet(uploadBlock, "href");
        HfDebug::Log("Upload href", uploadHref);
        if (uploadHref.empty()) { HfDebug::Log("No upload href found!"); return false; }

        // Look for chunk_size inside the header sub-block
        std::string headerBlock = JsonGetBlock(uploadBlock, "header");
        HfDebug::Log("Header block", headerBlock);
        std::string chunkSizeStr = JsonGet(headerBlock, "chunk_size");
        HfDebug::Log("chunk_size", chunkSizeStr.empty() ? "(none=basic)" : chunkSizeStr);

        // Open file for streaming
        HANDLE hFile=CreateFileW(localPath,GENERIC_READ,FILE_SHARE_READ,
                                 nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(hFile==INVALID_HANDLE_VALUE){ HfDebug::Log("Cannot open file for upload"); return false; }

        if (!chunkSizeStr.empty()) {
            // ── Multipart upload ──────────────────────────────────────────
            LONGLONG chunkSize=_atoi64(chunkSizeStr.c_str());
            if(chunkSize<=0) chunkSize=5LL*1024*1024*1024;
            int numParts=(int)((fileSize+chunkSize-1)/chunkSize);
            HfDebug::Log("Multipart upload, numParts",(int)numParts);
            HfDebug::Log("chunkSize",chunkSize);

            // Part URLs are 5-digit zero-padded keys "00001","00002",... inside header block
            std::vector<std::string> partUrls(numParts);
            for(int i=1;i<=numParts;++i){
                char key[8]; sprintf_s(key, "%05d", i);
                partUrls[i-1]=JsonGet(headerBlock, key);
                HfDebug::Log("Part "+std::to_string(i)+" URL",partUrls[i-1]);
            }

            std::vector<std::string> etags(numParts);
            for(int i=0;i<numParts;++i){
                LONGLONG offset=(LONGLONG)i*chunkSize;
                LONGLONG partBytes=min(chunkSize,fileSize-offset);
                HfDebug::Log("Uploading part "+std::to_string(i+1)+
                             " offset="+std::to_string(offset)+
                             " bytes="+std::to_string(partBytes));

                if(partUrls[i].empty()){
                    HfDebug::Log("EMPTY part URL for part",i+1);
                    CloseHandle(hFile); return false;
                }

                auto partResp=DoUrl(L"PUT",Utf8ToWide(partUrls[i]),
                                    {},nullptr,0,hFile,offset,(DWORD)partBytes);
                HfDebug::Log("Part "+std::to_string(i+1)+" status",(int)partResp.statusCode);
                HfDebug::Log("Part "+std::to_string(i+1)+" ETag",partResp.etag);
                if(!partResp.ok()){ CloseHandle(hFile); return false; }
                etags[i]=partResp.etag;
                if(progress) progress(offset+partBytes,fileSize);
            }
            CloseHandle(hFile);

            // Completion POST
            std::string compBody="{\"oid\":\""+oid+"\",\"parts\":[";
            for(int i=0;i<numParts;++i){
                if(i>0) compBody+=",";
                compBody+="{\"partNumber\":"+std::to_string(i+1)+
                          ",\"etag\":\""+JsonEscape(etags[i])+"\"}";
            }
            compBody+="]}";
            HfDebug::Log("Completion body",compBody);

            // uploadHref for multipart IS the HF completion endpoint
            auto compResp=DoUrl(L"POST",Utf8ToWide(uploadHref),
                                {L"Content-Type: application/vnd.git-lfs+json"},
                                compBody.data(),(DWORD)compBody.size());
            HfDebug::Log("Completion status",(int)compResp.statusCode);
            HfDebug::Log("Completion body",compResp.body);
            if(!compResp.ok()) return false;

        } else {
            // ── Basic single-part upload (PUT to S3 presigned URL) ────────
            HfDebug::Log("Basic single-part PUT, bytes",fileSize);
            // For large single-part, we must stream — can't load into RAM
            auto putResp=DoUrl(L"PUT",Utf8ToWide(uploadHref),
                               {},nullptr,0,hFile,0,fileSize);
            CloseHandle(hFile);
            HfDebug::Log("Basic PUT status",(int)putResp.statusCode);
            HfDebug::Log("Basic PUT body",putResp.body);
            if(!putResp.ok()) return false;
            if(progress) progress(fileSize,fileSize);
        }

        // Optional verify step
        std::string verifyBlock=JsonGetBlock(actionsBlock,"verify");
        if(!verifyBlock.empty()){
            std::string verifyHref=JsonGet(verifyBlock,"href");
            HfDebug::Log("Verify href",verifyHref);
            if(!verifyHref.empty()){
                std::string verBody="{\"oid\":\""+oid+"\",\"size\":"+std::to_string(fileSize)+"}";
                auto verResp=DoUrl(L"POST",Utf8ToWide(verifyHref),
                    {L"Authorization: Bearer "+apiToken,
                     L"Content-Type: application/vnd.git-lfs+json"},
                    verBody.data(),(DWORD)verBody.size());
                HfDebug::Log("Verify status",(int)verResp.statusCode);
                HfDebug::Log("Verify body",verResp.body);
            }
        }
    }

    // 4. Create commit referencing the LFS object
    HfDebug::Log("Creating commit for LFS file");
    std::string commitBody=
        "{\"summary\":\"Upload "+JsonEscape(pathInRepo)+" via HuggingFace VFS\","
        "\"lfsFiles\":[{\"path\":\""+JsonEscape(pathInRepo)+"\","
        "\"algo\":\"sha256\","
        "\"oid\":\""+oid+"\","
        "\"size\":"+std::to_string(fileSize)+"}]}";
    HfDebug::Log("Commit body",commitBody);

    std::wstring commitPath=L"/api/"+Utf8ToWide(repoType+"s")+
                            L"/"+Utf8ToWide(repoId)+L"/commit/main";
    auto commitResp=DoApi(apiToken,"POST",commitPath,&commitBody);
    HfDebug::Log("Commit status",(int)commitResp.statusCode);
    HfDebug::Log("Commit body",commitResp.body);
    HfDebug::Log("=== UploadLfs END ===", commitResp.ok()?"SUCCESS":"FAILED");
    return commitResp.ok();
}

// ── Public upload entry point ─────────────────────────────────────────────
inline bool UploadFile(
    const std::wstring& apiToken,
    const std::string& repoType, const std::string& repoId,
    const std::string& pathInRepo, LPCWSTR localPath,
    std::function<bool(LONGLONG,LONGLONG)> progress)
{
    LARGE_INTEGER fs={}; {
        HANDLE hf=CreateFileW(localPath,GENERIC_READ,FILE_SHARE_READ,
                              nullptr,OPEN_EXISTING,0,nullptr);
        if(hf==INVALID_HANDLE_VALUE) return false;
        GetFileSizeEx(hf,&fs); CloseHandle(hf);
    }
    HfDebug::Log("UploadFile size",fs.QuadPart);
    HfDebug::Log("UploadFile threshold",(long long)LFS_THRESHOLD);
    HfDebug::Log("UploadFile path",pathInRepo);

    if(fs.QuadPart<LFS_THRESHOLD)
        return UploadSmall(apiToken,repoType,repoId,pathInRepo,localPath,progress);
    else
        return UploadLfs  (apiToken,repoType,repoId,pathInRepo,localPath,progress);
}

} // namespace HfHttp
