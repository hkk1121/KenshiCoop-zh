#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf/atoi are fine; silence VC10 C4996

// Update.cpp - 自动更新模块实现，设计说明见 Update.h 与
// 提示词/交接_20261111_自动更新与下载代理.md（尤其第三节发布格式）。
//
// 流程：后台线程 -> 取 latest/update.json（GitHub 会跳到最新 Release 附件）->
// 与本机 update.json 比较 buildTime -> 从固定 tag 的下载地址取 4 个文件到
// update_tmp/ -> 逐个核对 SHA256 -> 逐个改名替换（旧的存成 *.old）->
// 写入新的本机 update.json。任何一步失败都整次作废、文件不变。
//
// 只用 VS2010（v100）支持的写法与 Windows 自带库；线程间只共享下面的状态快照。

#include "Update.h"
#include "Config.h"
#include "../CoopLog.h"
#include "../../netproto/Wire.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace coop {
namespace update {
namespace {

// 只访问固定地址，编进代码，不从配置读（交接文档 4.2）。查版本走 latest/download
// （GitHub 会跳到最新 Release 的附件，不用 api.github.com，未登录每天只有 60 次）；
// 文件按 update.json 里的 tag 取，避免下载途中有新发布、两个版本的文件混在一起。
const char* const kCheckUrl =
    "https://github.com/hkk1121/KenshiCoop-zh/releases/latest/download/update.json";
const char* const kDownloadBase =
    "https://github.com/hkk1121/KenshiCoop-zh/releases/download/";

// 启动后多久自动检查一次（毫秒）；整局只自动一次
const DWORD kAutoCheckDelayMs = 10000;
// 分片等待粒度：shutdown 时线程能很快看到停止标志
const DWORD kWaitSliceMs = 250;
// 退出时等后台线程收尾的上限（毫秒）
const DWORD kShutdownWaitMs = 3000;

CRITICAL_SECTION g_cs;
bool             g_csInit  = false;
Status           g_status;          // 受 g_cs 保护
std::string      g_proxy;           // 空 = 系统代理，否则归一化的 "host:port"（受 g_cs 保护）
volatile LONG    g_stop    = 0;     // 1 = shutdown 已请求停止
volatile LONG    g_busy    = 0;     // 1 = runCheck 正在跑
HANDLE           g_thread  = 0;     // 后台工作线程（启动时创建，退出时收尾）
HANDLE           g_event   = 0;     // 手动检查请求（自动复位事件）
bool             g_autoCheck = false; // 启动时是否安排了自动检查

void lockState() { if (g_csInit) EnterCriticalSection(&g_cs); }
void unlockState() { if (g_csInit) LeaveCriticalSection(&g_cs); }

void copyStr(char* dst, size_t cap, const std::string& src) {
    if (!cap) return;
    size_t n = src.size();
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// ---- 小工具 ----------------------------------------------------------------

std::string trim(const std::string& in) {
    size_t begin = 0, end = in.size();
    while (begin < end && (in[begin] == ' ' || in[begin] == '\t' ||
                           in[begin] == '\r' || in[begin] == '\n')) ++begin;
    while (end > begin && (in[end - 1] == ' ' || in[end - 1] == '\t' ||
                           in[end - 1] == '\r' || in[end - 1] == '\n')) --end;
    return in.substr(begin, end - begin);
}

bool startsWith(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

std::string mapValue(const std::map<std::string, std::string>& m, const std::string& key) {
    std::map<std::string, std::string>::const_iterator it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

// 44.2 字段里的文件名列表（"a|b|c"），去掉空白后逐个取出
void splitFiles(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (start <= s.size()) {
        size_t bar = s.find('|', start);
        std::string item = trim(s.substr(
            start, bar == std::string::npos ? std::string::npos : bar - start));
        if (!item.empty()) out.push_back(item);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
}

// 只允许普通文件名（发布内容由我们控制，这里只防把 ../ 或路径拼进 mod 目录）
bool safeFileName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find("..") != std::string::npos) return false;
    if (name.find_first_of("\\/:*?\"<>|") != std::string::npos) return false;
    return true;
}

std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), 0, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool fileExists(const std::string& p) {
    DWORD attr = GetFileAttributesA(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// 删目录（含子目录与文件）；删不掉的项跳过。用于 update_tmp 与残留清理。
void removeDirTree(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeDirTree(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

// ---- 代理串 ----------------------------------------------------------------
// 只接受 "host:port" 或 "http://host:port"（WinHTTP 不支持 SOCKS）。
// 返回 0 成功（out 归一化为 host:port）；1 空；2 SOCKS；3 格式无效。
int normalizeProxy(const std::string& raw, std::string& out) {
    std::string s = trim(raw);
    if (s.empty()) return 1;
    std::string lower = s;
    for (size_t i = 0; i < lower.size(); ++i)
        lower[i] = (char)tolower((unsigned char)lower[i]);
    if (startsWith(lower, "socks://") || startsWith(lower, "socks4://") ||
        startsWith(lower, "socks4a://") || startsWith(lower, "socks5://") ||
        startsWith(lower, "socks5h://"))
        return 2;
    if (startsWith(lower, "http://")) s = s.substr(7);
    else if (lower.find("://") != std::string::npos) return 3; // https:// 等不支持
    while (!s.empty() && s[s.size() - 1] == '/') s.erase(s.size() - 1);
    if (s.empty()) return 3;
    if (s.find_first_of(" \t\r\n/\\?#@") != std::string::npos) return 3;
    size_t colon = s.find_last_of(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return 3;
    std::string host = s.substr(0, colon);
    std::string port = s.substr(colon + 1);
    for (size_t i = 0; i < port.size(); ++i)
        if (port[i] < '0' || port[i] > '9') return 3;
    int portValue = atoi(port.c_str());
    if (portValue < 1 || portValue > 65535) return 3;
    for (size_t i = 0; i < host.size(); ++i) {
        char ch = host[i];
        bool okChar = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-' || ch == '_';
        if (!okChar) return 3;
    }
    out = s;
    return 0;
}

// ---- 状态写入 --------------------------------------------------------------

void setStage(int stage) {
    lockState();
    g_status.stage = stage;
    unlockState();
}

void setLatest(const std::string& build, const std::string& commit) {
    lockState();
    copyStr(g_status.latestBuild, sizeof(g_status.latestBuild), build);
    copyStr(g_status.latestCommit, sizeof(g_status.latestCommit), commit);
    unlockState();
}

void setDownloading(int index, int count) {
    lockState();
    g_status.stage = STAGE_DOWNLOADING;
    g_status.fileIndex = index;
    g_status.fileCount = count;
    unlockState();
}

// 失败：写 [update] failed 日志（格式固定），面板用 stage 名 + 错误码显示原因
void failCheck(const char* stage, DWORD err, DWORD http) {
    int stageId = 0; // 0 检查 / 1 下载 / 2 校验 / 3 替换
    if (strcmp(stage, "download") == 0) stageId = 1;
    else if (strcmp(stage, "verify") == 0) stageId = 2;
    else if (strcmp(stage, "replace") == 0) stageId = 3;
    char reason[80];
    _snprintf(reason, sizeof(reason) - 1, "err=%lu http=%lu",
              (unsigned long)err, (unsigned long)http);
    reason[sizeof(reason) - 1] = '\0';
    char line[160];
    _snprintf(line, sizeof(line) - 1, "[update] failed stage=%s %s", stage, reason);
    line[sizeof(line) - 1] = '\0';
    coop::logLine(line);
    lockState();
    g_status.stage = STAGE_FAILED;
    g_status.failedStage = stageId;
    copyStr(g_status.reason, sizeof(g_status.reason), reason);
    unlockState();
}

// ---- SHA256（BCrypt，系统自带） --------------------------------------------

bool sha256File(const std::string& path, std::string& hexOut) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR obj = NULL;
    bool ok = false;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (st == 0) {
        DWORD cbObj = 0, cbHash = 0, cbTmp = 0;
        st = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbObj,
                               sizeof(cbObj), &cbTmp, 0);
        if (st == 0)
            st = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&cbHash,
                                   sizeof(cbHash), &cbTmp, 0);
        if (st == 0 && cbObj && cbHash) {
            obj = (PUCHAR)malloc(cbObj);
            std::vector<unsigned char> digest((size_t)cbHash);
            st = BCryptCreateHash(alg, &hash, obj, cbObj, NULL, 0, 0);
            if (st == 0) {
                std::vector<char> buf(65536);
                bool readOk = true;
                for (;;) {
                    DWORD read = 0;
                    if (!ReadFile(f, &buf[0], (DWORD)buf.size(), &read, NULL)) {
                        readOk = false;
                        break;
                    }
                    if (read == 0) break;
                    st = BCryptHashData(hash, (PUCHAR)&buf[0], read, 0);
                    if (st != 0) { readOk = false; break; }
                }
                if (readOk) {
                    st = BCryptFinishHash(hash, &digest[0], (DWORD)digest.size(), 0);
                    if (st == 0) {
                        static const char* kHex = "0123456789abcdef";
                        hexOut.clear();
                        for (size_t i = 0; i < digest.size(); ++i) {
                            hexOut += kHex[digest[i] >> 4];
                            hexOut += kHex[digest[i] & 0x0F];
                        }
                        ok = true;
                    }
                }
            }
            free(obj);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(f);
    return ok;
}

// ---- WinHTTP 下载 ----------------------------------------------------------

struct HttpResult {
    DWORD err;    // WinHTTP / Win32 错误码（0 = 无）
    DWORD status; // HTTP 状态码（0 = 没拿到）
    HttpResult() : err(0), status(0) {}
};

bool stopRequested() {
    return InterlockedCompareExchange(&g_stop, 0, 0) != 0;
}

// 打开会话：用户配置的代理优先；否则采用当前用户的系统代理（IE 设置）；
// 都没有就直连。proxyLabel 给 [update] check start 日志用。
HINTERNET openSession(std::string& proxyLabel, HttpResult& h) {
    std::string configured;
    lockState();
    configured = g_proxy;
    unlockState();
    std::string proxy;
    bool named = false;
    if (!configured.empty()) {
        proxy = configured;
        proxyLabel = configured;
        named = true;
    } else {
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg;
        memset(&cfg, 0, sizeof(cfg));
        if (WinHttpGetIEProxyConfigForCurrentUser(&cfg)) {
            if (cfg.lpszProxy && cfg.lpszProxy[0]) {
                char narrow[512];
                int n = WideCharToMultiByte(CP_ACP, 0, cfg.lpszProxy, -1, narrow,
                                            sizeof(narrow), 0, 0);
                if (n > 0) {
                    proxy = narrow;
                    proxyLabel = "system";
                    named = true;
                }
            }
            if (cfg.lpszProxy) GlobalFree(cfg.lpszProxy);
            if (cfg.lpszProxyBypass) GlobalFree(cfg.lpszProxyBypass);
            if (cfg.lpszAutoConfigUrl) GlobalFree(cfg.lpszAutoConfigUrl);
        }
    }
    if (!named) proxyLabel = "none";
    std::wstring wproxy = named ? widenUtf8(proxy) : std::wstring();
    HINTERNET session = WinHttpOpen(
        L"KenshiCoop-update/1.0",
        named ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
        named ? wproxy.c_str() : WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        h.err = GetLastError();
        return 0;
    }
    // 连接、收发各 15～30 秒
    WinHttpSetTimeouts(session, 15000, 15000, 15000, 30000);
    return session;
}

// GET url：memory 非空时追加到内存，savePath 非空时写入该文件（两者可都不需要）。
// 只认 HTTP 200；HTTPS 证书按系统默认校验；GitHub 的跳转由 WinHTTP 默认跟。
bool httpGet(HINTERNET session, const std::string& url, std::string* memory,
             const std::string* savePath, HttpResult& h, unsigned long long* bytesOut) {
    if (bytesOut) *bytesOut = 0;
    if (!session) { h.err = ERROR_INVALID_HANDLE; return false; }
    std::wstring wurl = widenUtf8(url);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        h.err = GetLastError();
        return false;
    }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    HINTERNET conn = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    if (!conn) {
        h.err = GetLastError();
        return false;
    }
    HINTERNET req = WinHttpOpenRequest(
        conn, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    bool ok = false;
    if (!req) {
        h.err = GetLastError();
    } else {
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            h.err = GetLastError();
        } else if (!WinHttpReceiveResponse(req, NULL)) {
            h.err = GetLastError();
        } else {
            DWORD status = 0, len = sizeof(status);
            if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                     WINHTTP_NO_HEADER_INDEX)) {
                h.err = GetLastError();
            } else {
                h.status = status; // 非 200 就是失败；err 留 0，状态码本身就是原因
                if (status == 200) {
                    HANDLE file = INVALID_HANDLE_VALUE;
                    bool fileOk = true;
                    if (savePath) {
                        file = CreateFileA(savePath->c_str(), GENERIC_WRITE, 0, NULL,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                        if (file == INVALID_HANDLE_VALUE) {
                            fileOk = false;
                            h.err = GetLastError();
                            if (!h.err) h.err = ERROR_WRITE_FAULT;
                        }
                    }
                    if (fileOk) {
                        std::vector<char> buf(16384);
                        unsigned long long total = 0;
                        bool readOk = true;
                        for (;;) {
                            if (stopRequested()) {
                                readOk = false;
                                h.err = ERROR_OPERATION_ABORTED;
                                break;
                            }
                            DWORD avail = 0;
                            if (!WinHttpQueryDataAvailable(req, &avail)) {
                                readOk = false;
                                h.err = GetLastError();
                                break;
                            }
                            if (avail == 0) break;
                            DWORD want = avail > (DWORD)buf.size() ? (DWORD)buf.size() : avail;
                            DWORD got = 0;
                            if (!WinHttpReadData(req, &buf[0], want, &got)) {
                                readOk = false;
                                h.err = GetLastError();
                                break;
                            }
                            if (got == 0) break;
                            if (memory) memory->append(&buf[0], got);
                            if (file != INVALID_HANDLE_VALUE) {
                                DWORD written = 0;
                                if (!WriteFile(file, &buf[0], got, &written, NULL) ||
                                    written != got) {
                                    readOk = false;
                                    h.err = GetLastError();
                                    if (!h.err) h.err = ERROR_WRITE_FAULT;
                                    break;
                                }
                            }
                            total += got;
                        }
                        if (bytesOut) *bytesOut = total;
                        ok = readOk;
                    }
                    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                }
            }
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
    return ok;
}

// ---- 版本与替换 ------------------------------------------------------------

struct Manifest {
    std::string buildTime, commit, protocol, tag, files;
    std::map<std::string, std::string> raw;
};

bool parseManifest(const std::string& text, Manifest& m) {
    m.raw = coop::parseFlatJsonText(text);
    m.buildTime = mapValue(m.raw, "buildTime");
    m.commit = mapValue(m.raw, "commit");
    m.protocol = mapValue(m.raw, "protocol");
    m.tag = mapValue(m.raw, "tag");
    m.files = mapValue(m.raw, "files");
    return !m.buildTime.empty() && !m.commit.empty() && !m.tag.empty() && !m.files.empty();
}

// 本机 mods/KenshiCoop/update.json（打包时写进包里的“已安装版本”）。
// 没有它 = 开发版：只检查、只显示，不自动替换。
bool readLocalBuild(std::string& buildTime) {
    buildTime.clear();
    std::ifstream f((coop::moduleDirPath() + "update.json").c_str(), std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    buildTime = mapValue(coop::parseFlatJsonText(text), "buildTime");
    return !buildTime.empty();
}

// 中途失败回滚用：记录每个文件替换前的样子
struct MovedFile {
    std::string target, old;
    bool hadOld;
};

// 下载 + 核对通过后的替换：旧的改名为 *.old，新文件移进来，最后写本机 update.json。
// 任何一步失败都回滚已经换掉的文件并删掉 update_tmp，面板显示失败原因。
void replaceFiles(const std::string& dir, const std::vector<std::string>& names,
                  const std::vector<std::string>& tmpPaths, const std::string& remoteText,
                  const Manifest& man) {
    const std::string tmpDir = dir + "update_tmp";
    std::vector<MovedFile> moved;
    DWORD err = 0;
    bool ok = true;
    for (size_t i = 0; i < names.size() && ok; ++i) {
        if (stopRequested()) {
            err = ERROR_OPERATION_ABORTED;
            ok = false;
            break;
        }
        MovedFile mv;
        mv.target = dir + names[i];
        mv.old = mv.target + ".old";
        mv.hadOld = fileExists(mv.target);
        if (mv.hadOld) {
            // 正在使用的 DLL 不能覆盖，但可以改名；先清掉上次残留的 .old
            if (fileExists(mv.old)) DeleteFileA(mv.old.c_str());
            if (!MoveFileExA(mv.target.c_str(), mv.old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                err = GetLastError();
                ok = false;
                break;
            }
        }
        if (!MoveFileExA(tmpPaths[i].c_str(), mv.target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            err = GetLastError();
            if (mv.hadOld)
                MoveFileExA(mv.old.c_str(), mv.target.c_str(), MOVEFILE_REPLACE_EXISTING);
            ok = false;
            break;
        }
        moved.push_back(mv);
    }
    if (ok) {
        // 新的本机 update.json 用刚下载的远端文本：tag 与文件同源，不会记错版本。
        const std::string tmpJson = dir + "update.json.tmp";
        HANDLE f = CreateFileA(tmpJson.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        bool written = false;
        if (f != INVALID_HANDLE_VALUE) {
            DWORD done = 0;
            written = WriteFile(f, remoteText.data(), (DWORD)remoteText.size(), &done, NULL) != 0 &&
                      done == (DWORD)remoteText.size();
            if (!written) err = GetLastError();
            CloseHandle(f);
        } else {
            err = GetLastError();
        }
        if (written &&
            !MoveFileExA(tmpJson.c_str(), (dir + "update.json").c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            err = GetLastError();
            written = false;
        }
        if (!written) {
            DeleteFileA(tmpJson.c_str());
            ok = false;
        }
    }
    if (!ok) {
        // 中途失败：把已经换掉的文件从 .old 改回去；原来不存在的删掉新文件
        for (size_t j = moved.size(); j > 0; --j) {
            const MovedFile& mv = moved[j - 1];
            if (mv.hadOld)
                MoveFileExA(mv.old.c_str(), mv.target.c_str(), MOVEFILE_REPLACE_EXISTING);
            else
                DeleteFileA(mv.target.c_str());
        }
        removeDirTree(tmpDir);
        failCheck("replace", err, 0);
        return;
    }
    removeDirTree(tmpDir);
    char line[96];
    _snprintf(line, sizeof(line) - 1, "[update] replaced files=%u restart required",
              (unsigned)moved.size());
    line[sizeof(line) - 1] = '\0';
    coop::logLine(line);
    // 远端 protocol 与本机协议号不同：照常更新，面板多提示一句所有玩家都要更新。
    // 缺字段（旧格式）也按不同处理，提示总是安全的。
    int protocolChanged = 1;
    if (!man.protocol.empty() && atoi(man.protocol.c_str()) == (int)coop::PROTOCOL_VERSION)
        protocolChanged = 0;
    lockState();
    g_status.stage = STAGE_UPDATED;
    g_status.protocolChanged = protocolChanged;
    unlockState();
}

// ---- 一次完整检查 ----------------------------------------------------------

void runCheck() {
    // 已更新待重启：本局不再检查（自动检查与手动按钮都一样）
    {
        lockState();
        int stage = g_status.stage;
        unlockState();
        if (stage == STAGE_UPDATED) return;
    }
    setStage(STAGE_CHECKING);
    HttpResult h;
    std::string proxyLabel;
    HINTERNET session = openSession(proxyLabel, h);
    {
        char line[160];
        _snprintf(line, sizeof(line) - 1, "[update] check start proxy=%s", proxyLabel.c_str());
        line[sizeof(line) - 1] = '\0';
        coop::logLine(line);
    }
    if (!session) {
        failCheck("check", h.err, 0);
        return;
    }
    // 1. 远端 update.json（latest/download 会跳到最新 Release 的附件）
    std::string remoteText;
    Manifest man;
    if (!httpGet(session, kCheckUrl, &remoteText, 0, h, 0)) {
        WinHttpCloseHandle(session);
        failCheck("check", h.err, h.status);
        return;
    }
    if (!parseManifest(remoteText, man)) {
        WinHttpCloseHandle(session);
        failCheck("check", 87 /*ERROR_INVALID_PARAMETER*/, h.status);
        return;
    }
    // 2. 与本机版本比较（buildTime 可按字符串比较大小）
    std::string localBuild;
    bool hasLocal = readLocalBuild(localBuild);
    const std::string localLabel = hasLocal ? localBuild : std::string("dev");
    const char* verdict = "up-to-date";
    if (!hasLocal) verdict = "dev-skip";
    else if (man.buildTime > localBuild) verdict = "update";
    setLatest(man.buildTime, man.commit);
    {
        char line[224];
        _snprintf(line, sizeof(line) - 1,
                  "[update] latest build=%s commit=%s local=%s -> %s",
                  man.buildTime.c_str(), man.commit.c_str(), localLabel.c_str(), verdict);
        line[sizeof(line) - 1] = '\0';
        coop::logLine(line);
    }
    if (!hasLocal) {
        // 开发版：只检查、只显示最新版本，不自动替换
        setStage(STAGE_DEV_BUILD);
        WinHttpCloseHandle(session);
        return;
    }
    if (strcmp(verdict, "update") != 0) {
        setStage(STAGE_UP_TO_DATE);
        WinHttpCloseHandle(session);
        return;
    }
    // 3. 下载到 update_tmp/ 并逐个核对 SHA256（任何一个不对，整次作废）
    std::string dir = coop::moduleDirPath();
    std::vector<std::string> list;
    splitFiles(man.files, list);
    if (dir.empty() || list.empty()) {
        WinHttpCloseHandle(session);
        failCheck("download", 87, 0);
        return;
    }
    const std::string tmpDir = dir + "update_tmp";
    removeDirTree(tmpDir); // 上次失败可能的残留
    CreateDirectoryA(tmpDir.c_str(), NULL);
    std::vector<std::string> paths;
    const int total = (int)list.size();
    for (int i = 0; i < total; ++i) {
        const std::string name = list[i];
        if (!safeFileName(name)) {
            WinHttpCloseHandle(session);
            removeDirTree(tmpDir);
            failCheck("download", 87, 0);
            return;
        }
        if (stopRequested()) { // 正在退出：静默收尾
            WinHttpCloseHandle(session);
            removeDirTree(tmpDir);
            return;
        }
        setDownloading(i + 1, total);
        const std::string url = std::string(kDownloadBase) + man.tag + "/" + name;
        const std::string path = tmpDir + "\\" + name;
        unsigned long long bytes = 0;
        if (!httpGet(session, url, 0, &path, h, &bytes)) {
            WinHttpCloseHandle(session);
            removeDirTree(tmpDir);
            failCheck("download", h.err, h.status);
            return;
        }
        const std::string want = mapValue(man.raw, "sha256_" + name);
        std::string got;
        const bool shaOk = !want.empty() && sha256File(path, got) &&
                           equalsIgnoreCase(got, want);
        {
            char line[192];
            _snprintf(line, sizeof(line) - 1,
                      "[update] downloaded name=%s bytes=%I64u sha=%s",
                      name.c_str(), bytes, shaOk ? "ok" : "BAD");
            line[sizeof(line) - 1] = '\0';
            coop::logLine(line);
        }
        if (!shaOk) {
            WinHttpCloseHandle(session);
            removeDirTree(tmpDir);
            failCheck("verify", 0, h.status);
            return;
        }
        paths.push_back(path);
    }
    WinHttpCloseHandle(session);
    // 4. 逐个替换（失败回滚），最后写入新的本机 update.json
    replaceFiles(dir, list, paths, remoteText, man);
}

// ---- 后台线程 --------------------------------------------------------------

DWORD WINAPI workerMain(LPVOID) {
    DWORD nextAuto = GetTickCount() + kAutoCheckDelayMs;
    bool autoPending = g_autoCheck;
    bool want = false;
    for (;;) {
        if (stopRequested()) break;
        if (autoPending && (LONG)(GetTickCount() - nextAuto) >= 0) {
            autoPending = false;
            want = true;
        }
        if (!want) {
            DWORD waitMs = INFINITE;
            if (autoPending) {
                DWORD now = GetTickCount();
                waitMs = (DWORD)(nextAuto - now);
                if (waitMs > kWaitSliceMs) waitMs = kWaitSliceMs;
            }
            DWORD wr = WaitForSingleObject(g_event, waitMs);
            if (stopRequested()) break;
            if (wr == WAIT_OBJECT_0) {
                // 手动检查：按玩家的明确操作立即执行，取消还没到点的自动检查
                autoPending = false;
                want = true;
            }
        }
        if (want) {
            want = false;
            InterlockedExchange(&g_busy, 1);
            runCheck();
            InterlockedExchange(&g_busy, 0);
        }
    }
    return 0;
}

// 启动时清理上次替换残留的 *.old 与 update_tmp/（删不掉就跳过）
void startupCleanup() {
    const std::string dir = coop::moduleDirPath();
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.old").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileA((dir + fd.cFileName).c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    removeDirTree(dir + "update_tmp");
}

} // namespace

// ---- 对面板/插件暴露的接口 ------------------------------------------------

void startup(const std::string& proxy, bool autoCheck) {
    if (g_csInit) return;
    InitializeCriticalSection(&g_cs);
    g_csInit = true;
    memset(&g_status, 0, sizeof(g_status));
    g_status.stage = STAGE_IDLE;
    if (!proxy.empty()) {
        std::string normalized;
        if (normalizeProxy(proxy, normalized) == 0) {
            lockState();
            g_proxy = normalized;
            unlockState();
        } else {
            coop::logErrLine("[update] updateProxy in coop_config.json is invalid - using system proxy");
        }
    }
    startupCleanup();
    g_autoCheck = autoCheck;
    g_event = CreateEventA(NULL, FALSE, FALSE, NULL); // 自动复位
    if (!g_event) {
        coop::logErrLine("[update] CreateEvent FAILED - update checks disabled");
        return;
    }
    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(0, 0, &workerMain, 0, 0, 0);
    if (!g_thread) coop::logErrLine("[update] CreateThread FAILED - update checks disabled");
    if (autoCheck && g_thread) coop::logLine("[update] auto check armed (10 s after startup)");
}

void shutdown() {
    if (!g_csInit) return;
    InterlockedExchange(&g_stop, 1);
    if (g_event) SetEvent(g_event);
    if (g_thread) {
        WaitForSingleObject(g_thread, kShutdownWaitMs);
        CloseHandle(g_thread);
        g_thread = 0;
    }
    if (g_event) {
        CloseHandle(g_event);
        g_event = 0;
    }
}

void checkNow() {
    if (!g_csInit || !g_event || !g_thread) return;
    if (stopRequested()) return;
    {
        lockState();
        int stage = g_status.stage;
        unlockState();
        if (stage == STAGE_UPDATED) return; // 已更新待重启：点了没有效果
    }
    if (InterlockedCompareExchange(&g_busy, 0, 0) != 0) return; // 已有检查在跑
    SetEvent(g_event);
}

void getStatus(Status& out) {
    memset(&out, 0, sizeof(out));
    lockState();
    out = g_status;
    unlockState();
}

std::string proxyText() {
    std::string p;
    lockState();
    p = g_proxy;
    unlockState();
    return p;
}

int applyProxyText(const std::string& raw) {
    std::string normalized;
    int r = normalizeProxy(raw, normalized);
    if (r != 0) return r;
    if (!coop::saveConfigString("updateProxy", normalized)) return 4;
    lockState();
    g_proxy = normalized;
    unlockState();
    return 0;
}

int clearProxy() {
    if (!coop::saveConfigString("updateProxy", std::string())) return 4;
    lockState();
    g_proxy.clear();
    unlockState();
    return 0;
}

} // namespace update
} // namespace coop
