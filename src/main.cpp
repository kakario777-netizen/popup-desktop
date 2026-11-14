// 팝업 바탕화면 (Popup Desktop) - 초경량 바탕화면 정리 프로그램
// Win32 + GDI+ 단일 exe
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <exdisp.h>
#include <servprov.h>
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cmath>
#include <cstdint>
#include <climits>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "json.h"

using namespace Gdiplus;

// ───────────────────────── 전역 ─────────────────────────
static HWND g_hwnd;
static std::wstring g_baseDir, g_dataDir, g_layoutPath, g_backupDir, g_exePath;
static float S = 1.0f;
static int CW, CH, ICON1, ICON2, LINEH, MARGIN, PAD, HEAD, RAD;
static RECT g_screen;          // 가상 화면(모든 모니터) – 창 클라이언트 좌표
struct Mon { RECT rc, work; };
static std::vector<Mon> g_mons; // 0번 = 주 모니터
static POINT g_virt;            // 가상 화면 원점(스크린 좌표)
static std::wstring g_backupData, g_desktopDir, g_publicDesktop;
static volatile LONG g_watchGen = 0;
static UINT WM_TASKBARCREATED;
static RECT g_paintRc;

enum { TM_ANIM = 1, TM_SAVE = 2, TM_RESCAN = 3 };
#define WM_APP_RESCAN (WM_APP + 1)
#define WM_APP_FIRSTRUN (WM_APP + 2)

// ───────────────────────── 유틸 ─────────────────────────
static std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring W16(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring Lower(std::wstring s) { CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }
static bool IsDirPath(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring ExtOf(const std::wstring& n) {
    size_t d = n.find_last_of(L'.');
    if (d == std::wstring::npos || d == 0) return L"";
    return Lower(n.substr(d + 1));
}
static std::wstring FileName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}
static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
static std::wstring UniqueName(const std::wstring& dir, const std::wstring& base) {
    std::wstring n = base; int i = 2;
    while (Exists(dir + L"\\" + n)) n = base + L" (" + std::to_wstring(i++) + L")";
    return n;
}
static bool StartsWithI(const std::wstring& s, const std::wstring& pre) {
    return s.size() >= pre.size() && Lower(s.substr(0, pre.size())) == Lower(pre);
}
static std::wstring DblNull(const std::vector<std::wstring>& v) {
    std::wstring r;
    for (auto& s : v) { r += s; r.push_back(L'\0'); }
    r.push_back(L'\0');
    return r;
}
static std::wstring ResolveLnk(const std::wstring& path) {
    std::wstring r;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            if (SUCCEEDED(pf->Load(path.c_str(), STGM_READ))) {
                wchar_t buf[MAX_PATH * 2] = {};
                if (SUCCEEDED(sl->GetPath(buf, MAX_PATH * 2, nullptr, 0))) {
                    wchar_t ex[MAX_PATH * 2] = {};
                    ExpandEnvironmentStringsW(buf, ex, MAX_PATH * 2);
                    r = ex;
                }
            }
            pf->Release();
        }
        sl->Release();
    }
    return r;
}
static bool CreateLnk(const std::wstring& lnk, const std::wstring& target) {
    bool ok = false;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) {
        sl->SetPath(target.c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    return ok;
}
static std::wstring PickFolder(HWND owner, const wchar_t* title) {
    std::wstring r;
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&d))) {
        DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(title);
        if (SUCCEEDED(d->Show(owner))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(d->GetResult(&it))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = p; CoTaskMemFree(p); }
                it->Release();
            }
        }
        d->Release();
    }
    return r;
}
static unsigned long long DirSize(const std::wstring& p, int depth = 0) {
    if (depth > 30) return 0;
    unsigned long long t = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) t += DirSize(p + L"\\" + fd.cFileName, depth + 1);
        else t += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return t;
}
static std::wstring SizeStr(unsigned long long b) {
    wchar_t buf[64];
    if (b >= (1ull << 30)) swprintf(buf, 64, L"%.1fGB", b / 1073741824.0);
    else if (b >= (1ull << 20)) swprintf(buf, 64, L"%.1fMB", b / 1048576.0);
    else swprintf(buf, 64, L"%lluKB", b / 1024 + 1);
    return buf;
}

// ───────────────────────── 레이아웃 저장 ─────────────────────────
struct Entry {
    int gx = -1, gy = -1, gs = 1;
    bool pin = false, pop = false;
    int px = INT_MIN, py = 0, pc = 4, pr = 3;
    int m = 0;
};
static std::map<std::wstring, Entry> g_lay;
struct Settings { bool firstRunDone = false; int wallMode = 0; std::wstring wallPath; int mode = -1; bool posImported = false; } g_set;   // mode 0=연동 1=백업

static std::wstring Key(const std::wstring& path) {
    std::wstring lp = Lower(path), ld = Lower(g_backupData) + L"\\";
    if (lp.compare(0, ld.size(), ld) == 0) return L"@" + lp.substr(ld.size());
    return lp;
}
static void MoveKeys(const std::wstring& oldPath, const std::wstring& newPath) {
    std::wstring ok = Key(oldPath), nk = Key(newPath);
    std::map<std::wstring, Entry> out;
    for (auto& kv : g_lay) {
        const std::wstring& k = kv.first;
        if (k == ok) out[nk] = kv.second;
        else if (k.size() > ok.size() && k.compare(0, ok.size(), ok) == 0 && k[ok.size()] == L'\\')
            out[nk + k.substr(ok.size())] = kv.second;
        else out[k] = kv.second;
    }
    g_lay.swap(out);
}
static bool ParseLayout(const std::string& txt) {
    JParser jp(txt.data(), txt.size());
    JV root = jp.val();
    if (!jp.ok || root.t != JV::Obj) return false;
    g_lay.clear();
    if (auto st = root.get("settings")) {
        g_set.firstRunDone = st->num("firstRunDone", 0) != 0;
        g_set.wallMode = (int)st->num("wallMode", 0);
        g_set.wallPath = W16(st->str("wallPath"));
        g_set.mode = (int)st->num("mode", -1);
        g_set.posImported = st->num("posImported", 0) != 0;
    }
    if (auto arr = root.get("items")) {
        for (auto& o : arr->a) {
            Entry e;
            e.gx = (int)o.num("x", -1); e.gy = (int)o.num("y", -1); e.gs = (int)o.num("s", 1);
            e.pin = o.num("pin", 0) != 0; e.pop = o.num("pop", 0) != 0;
            e.px = (int)o.num("px", INT_MIN); e.py = (int)o.num("py", 0);
            e.pc = (int)o.num("pc", 4); e.pr = (int)o.num("pr", 3);
            e.m = (int)o.num("m", 0);
            if (e.gs < 1 || e.gs > 2) e.gs = 1;
            if (e.pc < 1) e.pc = 1; if (e.pr < 1) e.pr = 1;
            g_lay[W16(o.str("k"))] = e;
        }
    }
    return true;
}
static std::string ReadFileA(const std::wstring& p, bool* ok) {
    std::string s; *ok = false;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s;
    DWORD sz = GetFileSize(h, nullptr), rd = 0;
    if (sz != INVALID_FILE_SIZE && sz < 64 * 1024 * 1024) {
        s.resize(sz);
        if (ReadFile(h, &s[0], sz, &rd, nullptr) && rd == sz) *ok = true;
    }
    CloseHandle(h);
    return s;
}
static std::vector<std::wstring> ListBackups() {
    std::vector<std::wstring> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_backupDir + L"\\layout_*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do v.push_back(g_backupDir + L"\\" + fd.cFileName); while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(v.begin(), v.end());
    return v;
}
static void LoadLayout() {
    bool ok;
    std::string t = ReadFileA(g_layoutPath, &ok);
    if (ok && ParseLayout(t)) return;
    if (!Exists(g_layoutPath)) return;           // 첫 실행
    auto b = ListBackups();                      // 깨졌으면 최근 백업으로 자동 복구
    for (int i = (int)b.size() - 1; i >= 0; i--) {
        t = ReadFileA(b[i], &ok);
        if (ok && ParseLayout(t)) return;
    }
}
static void WriteLayoutFile() {
    std::string o = "{\n\"version\":1,\n\"settings\":{";
    o += "\"firstRunDone\":" + std::string(g_set.firstRunDone ? "true" : "false");
    o += ",\"wallMode\":" + std::to_string(g_set.wallMode);
    o += ",\"wallPath\":" + JEsc(U8(g_set.wallPath));
    o += ",\"mode\":" + std::to_string(g_set.mode) + ",\"posImported\":" + (g_set.posImported ? "true" : "false");
    o += "},\n\"items\":[";
    bool first = true;
    for (auto& kv : g_lay) {
        const Entry& e = kv.second;
        o += first ? "\n" : ",\n"; first = false;
        o += "{\"k\":" + JEsc(U8(kv.first)) + ",\"x\":" + std::to_string(e.gx) + ",\"y\":" + std::to_string(e.gy) +
             ",\"s\":" + std::to_string(e.gs) + ",\"m\":" + std::to_string(e.m) + ",\"pin\":" + (e.pin ? "1" : "0") + ",\"pop\":" + (e.pop ? "1" : "0");
        if (e.pop) {
            if (e.px != INT_MIN) o += ",\"px\":" + std::to_string(e.px) + ",\"py\":" + std::to_string(e.py);
            o += ",\"pc\":" + std::to_string(e.pc) + ",\"pr\":" + std::to_string(e.pr);
        }
        o += "}";
    }
    o += "\n]\n}\n";
    std::wstring tmp = g_layoutPath + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, o.data(), (DWORD)o.size(), &wr, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && wr == o.size()) MoveFileExW(tmp.c_str(), g_layoutPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
static void BackupLayoutOnStart() {
    if (!Exists(g_layoutPath)) return;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t n[64];
    swprintf(n, 64, L"\\layout_%04d%02d%02d_%02d%02d%02d.json", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    CopyFileW(g_layoutPath.c_str(), (g_backupDir + n).c_str(), FALSE);
    auto b = ListBackups();
    for (int i = 0; i + 10 < (int)b.size(); i++) DeleteFileW(b[i].c_str());
}

// ───────────────────────── 데이터 모델 ─────────────────────────
struct Container;
struct BlurCache { Bitmap* bmp = nullptr; RECT r{}; void* src = nullptr; int gen = -1; ~BlurCache() { delete bmp; } };
struct Item {
    std::wstring name, path, target, folder;   // folder = 실제로 들어있는 폴더
    bool isDir = false, lnkFolder = false;
    int mon = 0;
    bool pop = false, pin = false;
    int gx = -1, gy = -1, gs = 1;
    int px = INT_MIN, py = 0, pc = 4, pr = 3, scroll = 0;
    Container* parent = nullptr;
    std::unique_ptr<Container> child;
    Bitmap* icon = nullptr; int iconPx = 0; bool iconFailed = false;
    Bitmap* sceneBlur = nullptr; RECT sceneRect{};
    BlurCache bg;
    ~Item();
    bool FolderLike() const { return isDir || lnkFolder; }
    int Span() const { return pop ? 2 : gs; }
    std::wstring Dir() const { return lnkFolder ? target : path; }
    std::wstring Label() const {
        std::wstring e = ExtOf(name);
        if (!isDir && (e == L"lnk" || e == L"url")) return name.substr(0, name.size() - e.size() - 1);
        return name;
    }
};
struct Container {
    std::wstring dir;
    std::vector<std::wstring> extraDirs;   // 연동 모드: 공용 바탕화면
    bool external = false;
    Item* owner = nullptr;
    std::vector<std::unique_ptr<Item>> items;
};
Item::~Item() { delete icon; delete sceneBlur; }

static Container g_root;
static std::vector<Item*> g_stack;      // 열린 팝업 (아래→위)
static Item* g_hover = nullptr;
static Item* g_sel = nullptr;
static int g_bgGen = 0;

static int RootCols(int m = 0) { const RECT& w = g_mons[m].work; return std::max(2, (int)((w.right - w.left - 2 * MARGIN) / CW)); }
static int RootRows(int m = 0) { const RECT& w = g_mons[m].work; return std::max(2, (int)((w.bottom - w.top - 2 * MARGIN) / CH)); }
static int MonAt(POINT p) {
    for (int i = 0; i < (int)g_mons.size(); i++) if (PtInRect(&g_mons[i].rc, p)) return i;
    int best = 0; long long bd = LLONG_MAX;
    for (int i = 0; i < (int)g_mons.size(); i++) {
        const RECT& r = g_mons[i].rc;
        long long dx = p.x < r.left ? r.left - p.x : (p.x > r.right ? p.x - r.right : 0);
        long long dy = p.y < r.top ? r.top - p.y : (p.y > r.bottom ? p.y - r.bottom : 0);
        if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = i; }
    }
    return best;
}

static void SyncEntry(Item* it) {
    Entry& e = g_lay[Key(it->path)];
    e.gx = it->gx; e.gy = it->gy; e.gs = it->gs; e.pin = it->pin; e.pop = it->pop;
    e.px = it->px; e.py = it->py; e.pc = it->pc; e.pr = it->pr; e.m = it->mon;
}

static void AutoPlace(Container* c);

static void LoadContainer(Container* c) {
    c->items.clear();
    std::vector<std::wstring> dirs = {c->dir};
    for (auto& x : c->extraDirs) dirs.push_back(x);
    for (auto& dir : dirs) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
            auto it = std::make_unique<Item>();
            it->name = fd.cFileName;
            it->folder = dir;
            it->path = dir + L"\\" + it->name;
            it->isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            it->parent = c;
            if (!it->isDir && ExtOf(it->name) == L"lnk") {
                it->target = ResolveLnk(it->path);
                if (!it->target.empty() && IsDirPath(it->target)) it->lnkFolder = true;
            }
            auto f = g_lay.find(Key(it->path));
            if (f != g_lay.end()) {
                const Entry& e = f->second;
                it->gx = e.gx; it->gy = e.gy; it->gs = e.gs; it->pin = e.pin;
                it->pop = e.pop && it->FolderLike();
                it->px = e.px; it->py = e.py; it->pc = e.pc; it->pr = e.pr; it->mon = e.m;
            }
            c->items.push_back(std::move(it));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    }
    AutoPlace(c);
}

static Container* Child(Item* it) {
    if (!it->child) {
        it->child = std::make_unique<Container>();
        it->child->dir = it->Dir();
        it->child->owner = it;
        it->child->external = it->lnkFolder || (it->parent && it->parent->external);
        LoadContainer(it->child.get());
    }
    return it->child.get();
}

// 격자 점유
struct Occ {
    int cols, rows; std::vector<char> v;
    Occ(int c, int r) : cols(c), rows(r), v((size_t)c * r, 0) {}
    bool Fits(int x, int y, int s) const {
        if (x < 0 || y < 0 || x + s > cols || y + s > rows) return false;
        for (int j = 0; j < s; j++) for (int i = 0; i < s; i++) if (v[(size_t)(y + j) * cols + x + i]) return false;
        return true;
    }
    void Mark(int x, int y, int s) {
        for (int j = 0; j < s; j++) for (int i = 0; i < s; i++)
            if (x + i < cols && y + j < rows) v[(size_t)(y + j) * cols + x + i] = 1;
    }
};
static void ContDims(Container* c, int& cols, int& rows) {
    bool root = (c == &g_root);
    int maxSpan = 1, maxBottom = 0;
    for (auto& it : c->items) { maxSpan = std::max(maxSpan, it->Span()); maxBottom = std::max(maxBottom, it->gy + it->Span()); }
    if (root) { cols = RootCols(); rows = RootRows(); }
    else {
        cols = std::max(std::max(1, c->owner->pc), maxSpan);
        rows = maxBottom + (int)c->items.size() * 4 / cols + 6;
    }
}
static void AutoPlaceRoot(Container* c) {
    int M = (int)g_mons.size();
    std::vector<Occ> occ;
    for (int m = 0; m < M; m++) occ.emplace_back(RootCols(m), RootRows(m));
    std::vector<Item*> order;
    for (auto& it : c->items) order.push_back(it.get());
    std::stable_sort(order.begin(), order.end(), [](Item* a, Item* b) { return a->pin > b->pin; });
    std::vector<Item*> pending;
    for (Item* it : order) {
        int s = it->Span();
        if (it->mon >= 0 && it->mon < M && it->gx >= 0 && it->gy >= 0 && occ[it->mon].Fits(it->gx, it->gy, s)) occ[it->mon].Mark(it->gx, it->gy, s);
        else pending.push_back(it);
    }
    int overflow = 0;
    for (Item* it : pending) {
        int s = it->Span(); bool done = false;
        for (int m = 0; m < M && !done; m++)
            for (int x = 0; x < occ[m].cols && !done; x++)
                for (int y = 0; y < occ[m].rows && !done; y++)
                    if (occ[m].Fits(x, y, s)) { it->mon = m; it->gx = x; it->gy = y; occ[m].Mark(x, y, s); done = true; }
        if (!done) { it->mon = 0; it->gx = 0; it->gy = RootRows(0) + 2 * overflow++; }   // 화면이 꽉 참
        SyncEntry(it);
    }
}
static void AutoPlace(Container* c) {
    bool root = (c == &g_root);
    if (root) { AutoPlaceRoot(c); return; }
    int cols, rows; ContDims(c, cols, rows);
    if (root) rows += (int)c->items.size() * 4 / cols + 2;   // 넘치면 화면 밖으로
    Occ occ(cols, rows);
    int visRows = root ? RootRows() : rows;
    std::vector<Item*> order;
    for (auto& it : c->items) order.push_back(it.get());
    std::stable_sort(order.begin(), order.end(), [](Item* a, Item* b) { return a->pin > b->pin; });
    std::vector<Item*> pending;
    for (Item* it : order) {
        int s = it->Span();
        if (it->gx >= 0 && it->gy >= 0 && it->gy + s <= visRows && occ.Fits(it->gx, it->gy, s)) occ.Mark(it->gx, it->gy, s);
        else pending.push_back(it);
    }
    for (Item* it : pending) {
        int s = it->Span(); bool done = false;
        if (root) {
            for (int x = 0; x < cols && !done; x++)
                for (int y = 0; y < RootRows() && !done; y++)
                    if (occ.Fits(x, y, s)) { it->gx = x; it->gy = y; done = true; }
            for (int y = RootRows(); y < rows && !done; y++)
                for (int x = 0; x < cols && !done; x++)
                    if (occ.Fits(x, y, s)) { it->gx = x; it->gy = y; done = true; }
        } else {
            for (int y = 0; y < rows && !done; y++)
                for (int x = 0; x < cols && !done; x++)
                    if (occ.Fits(x, y, s)) { it->gx = x; it->gy = y; done = true; }
        }
        if (done) occ.Mark(it->gx, it->gy, s);
        SyncEntry(it);
    }
}
static bool NearestFree(Container* c, Item* self, int s, int tx, int ty, int& ox, int& oy, int mon = 0) {
    bool root = (c == &g_root);
    int cols, rows; ContDims(c, cols, rows);
    if (root) { if (mon < 0 || mon >= (int)g_mons.size()) mon = 0; cols = RootCols(mon); rows = RootRows(mon); }
    Occ occ(cols, rows);
    for (auto& it : c->items) if (it.get() != self && (!root || it->mon == mon)) occ.Mark(it->gx, it->gy, it->Span());
    long best = LONG_MAX;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            if (occ.Fits(x, y, s)) {
                long d = (long)(x - tx) * (x - tx) + (long)(y - ty) * (y - ty);
                if (d < best) { best = d; ox = x; oy = y; }
            }
    return best != LONG_MAX;
}

// ───────────────────────── 레이아웃 반영 ─────────────────────────
static void SaveTree(Container* c) {
    for (auto& it : c->items) { SyncEntry(it.get()); if (it->child) SaveTree(it->child.get()); }
}
static void ScheduleSave() { SetTimer(g_hwnd, TM_SAVE, 800, nullptr); }

static void RebuildAll() {
    SaveTree(&g_root);
    std::vector<std::wstring> keys;
    for (Item* it : g_stack) keys.push_back(Key(it->path));
    g_stack.clear(); g_hover = g_sel = nullptr;
    g_root.items.clear();
    LoadContainer(&g_root);
    Container* c = &g_root;
    for (auto& k : keys) {
        Item* found = nullptr;
        for (auto& it : c->items) if (Key(it->path) == k && it->pop) { found = it.get(); break; }
        if (!found) break;
        g_stack.push_back(found);
        c = Child(found);
    }
    ScheduleSave();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ───────────────────────── 배경 / 블러 ─────────────────────────
static HDC g_bgDC = nullptr; static HBITMAP g_bgHbm = nullptr, g_bgOld = nullptr;
static Bitmap* g_bgGp = nullptr;       // 배경 DIB 래퍼
static Bitmap* g_blurSmall = nullptr;  // 320px 블러
static uint32_t* g_bgBits = nullptr;

static void Blur1D(const uint32_t* s, uint32_t* d, int w, int h, int r, bool horiz) {
    int n = horiz ? w : h, lines = horiz ? h : w;
    int step = horiz ? 1 : w, lstep = horiz ? w : 1;
    int div = 2 * r + 1;
    for (int l = 0; l < lines; l++) {
        const uint32_t* sl = s + (size_t)l * lstep; uint32_t* dl = d + (size_t)l * lstep;
        int sum[4] = {0, 0, 0, 0};
        for (int i = -r; i <= r; i++) {
            uint32_t p = sl[(size_t)std::min(std::max(i, 0), n - 1) * step];
            for (int c = 0; c < 4; c++) sum[c] += (p >> (c * 8)) & 255;
        }
        for (int i = 0; i < n; i++) {
            dl[(size_t)i * step] = (uint32_t)(sum[0] / div) | ((uint32_t)(sum[1] / div) << 8) |
                                   ((uint32_t)(sum[2] / div) << 16) | ((uint32_t)(sum[3] / div) << 24);
            uint32_t pr = sl[(size_t)std::min(std::max(i - r, 0), n - 1) * step];
            uint32_t pa = sl[(size_t)std::min(std::max(i + r + 1, 0), n - 1) * step];
            for (int c = 0; c < 4; c++) sum[c] += (int)((pa >> (c * 8)) & 255) - (int)((pr >> (c * 8)) & 255);
        }
    }
}
static void BlurBitmap(Bitmap* b, int r, int passes) {
    int w = b->GetWidth(), h = b->GetHeight();
    BitmapData d; Rect rc(0, 0, w, h);
    if (b->LockBits(&rc, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppPARGB, &d) != Ok) return;
    std::vector<uint32_t> a((size_t)w * h), t((size_t)w * h);
    for (int y = 0; y < h; y++) memcpy(&a[(size_t)y * w], (char*)d.Scan0 + (size_t)y * d.Stride, w * 4);
    for (int p = 0; p < passes; p++) { Blur1D(a.data(), t.data(), w, h, r, true); Blur1D(t.data(), a.data(), w, h, r, false); }
    for (int y = 0; y < h; y++) memcpy((char*)d.Scan0 + (size_t)y * d.Stride, &a[(size_t)y * w], w * 4);
    b->UnlockBits(&d);
}
// 큰 영역 → 작게 줄이고 블러
static Bitmap* MakeSmallBlur(Image* src, int sx, int sy, int sw, int sh, int targetW) {
    int tw = std::max(8, targetW), th = std::max(8, (int)((long long)sh * tw / std::max(1, sw)));
    Bitmap* b = new Bitmap(tw, th, PixelFormat32bppPARGB);
    {
        Graphics g(b);
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        ImageAttributes ia; ia.SetWrapMode(WrapModeTileFlipXY);
        g.DrawImage(src, Rect(0, 0, tw, th), sx, sy, sw, sh, UnitPixel, &ia);
    }
    BlurBitmap(b, 4, 3);
    return b;
}
static void FreeBg() {
    delete g_blurSmall; g_blurSmall = nullptr;
    delete g_bgGp; g_bgGp = nullptr;
    if (g_bgDC) { SelectObject(g_bgDC, g_bgOld); DeleteObject(g_bgHbm); DeleteDC(g_bgDC); g_bgDC = nullptr; }
}
static void LoadBackground() {
    FreeBg();
    int w = g_screen.right - g_screen.left, h = g_screen.bottom - g_screen.top;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    g_bgDC = CreateCompatibleDC(sdc);
    g_bgHbm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&g_bgBits, nullptr, 0);
    ReleaseDC(nullptr, sdc);
    g_bgOld = (HBITMAP)SelectObject(g_bgDC, g_bgHbm);

    COLORREF cr = GetSysColor(COLOR_DESKTOP);
    struct Job { std::wstring path; RECT r; int pos; };
    std::vector<Job> jobs;
    int pos = 4; // DWPOS_FILL
    if (g_set.wallMode == 1 && !g_set.wallPath.empty()) {
        for (auto& m : g_mons) jobs.push_back({g_set.wallPath, m.rc, 4});
    } else {
        IDesktopWallpaper* dw = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_IDesktopWallpaper, (void**)&dw))) {
            dw->GetBackgroundColor(&cr);
            DESKTOP_WALLPAPER_POSITION dp; if (SUCCEEDED(dw->GetPosition(&dp))) pos = (int)dp;
            UINT n = 0; dw->GetMonitorDevicePathCount(&n);
            for (UINT i = 0; i < n; i++) {
                LPWSTR id = nullptr;
                if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
                RECT r; LPWSTR wp = nullptr;
                if (SUCCEEDED(dw->GetMonitorRECT(id, &r))) {
                    OffsetRect(&r, -g_virt.x, -g_virt.y);
                    dw->GetWallpaper(id, &wp);
                    jobs.push_back({wp ? wp : L"", r, pos});
                }
                CoTaskMemFree(wp); CoTaskMemFree(id);
            }
            dw->Release();
        }
        if (jobs.empty()) {
            wchar_t buf[MAX_PATH] = {};
            SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, buf, 0);
            for (auto& m : g_mons) jobs.push_back({buf, m.rc, 4});
        }
        if (pos == 5 && !jobs.empty()) {   // DWPOS_SPAN: 모든 모니터에 한 장
            RECT u = jobs[0].r; for (auto& j : jobs) UnionRect(&u, &u, &j.r);
            std::wstring p0 = jobs[0].path; jobs.clear(); jobs.push_back({p0, u, 4});
        }
    }
    {
        HBRUSH br = CreateSolidBrush(cr);
        RECT r = {0, 0, w, h}; FillRect(g_bgDC, &r, br); DeleteObject(br);
    }
    {
        Graphics g(g_bgDC);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        std::wstring lastPath; Bitmap* img = nullptr;
        for (auto& j : jobs) {
            if (j.path.empty() || !Exists(j.path)) continue;
            if (j.path != lastPath) { delete img; img = Bitmap::FromFile(j.path.c_str()); lastPath = j.path; }
            if (!img || img->GetLastStatus() != Ok || img->GetWidth() == 0) continue;
            double iw = img->GetWidth(), ih = img->GetHeight();
            double rw = j.r.right - j.r.left, rh = j.r.bottom - j.r.top;
            Region old; g.GetClip(&old);
            g.SetClip(Rect(j.r.left, j.r.top, (INT)rw, (INT)rh));
            if (j.pos == 1) {             // TILE
                for (double y = j.r.top; y < j.r.bottom; y += ih)
                    for (double x = j.r.left; x < j.r.right; x += iw)
                        g.DrawImage(img, Rect((INT)x, (INT)y, (INT)iw, (INT)ih));
            } else {
                double dw = iw, dh = ih;
                if (j.pos == 2) { dw = rw; dh = rh; }                                       // STRETCH
                else if (j.pos == 3) { double sc = std::min(rw / iw, rh / ih); dw = iw * sc; dh = ih * sc; }  // FIT
                else if (j.pos != 0) { double sc = std::max(rw / iw, rh / ih); dw = iw * sc; dh = ih * sc; }  // FILL
                g.DrawImage(img, RectF((REAL)(j.r.left + (rw - dw) / 2), (REAL)(j.r.top + (rh - dh) / 2), (REAL)dw, (REAL)dh));
            }
            g.SetClip(&old);
        }
        delete img;
    }
    // 원본 디코딩 이미지는 버리고 화면 크기 한 장만 유지
    g_bgGp = new Bitmap(w, h, w * 4, PixelFormat32bppRGB, (BYTE*)g_bgBits);
    g_blurSmall = MakeSmallBlur(g_bgGp, 0, 0, w, h, std::max(320, w / 6));
    g_bgGen++;
}

// 영역 r에 맞는 블러 배경(캐시)
static Bitmap* GetBlurBg(BlurCache& c, RECT r, Bitmap* src, RECT srcArea) {
    if (c.bmp && EqualRect(&c.r, &r) && c.src == src && c.gen == g_bgGen) return c.bmp;
    delete c.bmp;
    int w = std::max(1L, r.right - r.left), h = std::max(1L, r.bottom - r.top);
    c.bmp = new Bitmap(w, h, PixelFormat32bppPARGB);
    float kx = (float)(srcArea.right - srcArea.left) / src->GetWidth();
    float ky = (float)(srcArea.bottom - srcArea.top) / src->GetHeight();
    Graphics g(c.bmp);
    g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    ImageAttributes ia; ia.SetWrapMode(WrapModeTileFlipXY);
    g.DrawImage(src, RectF(0, 0, (REAL)w, (REAL)h), (r.left - srcArea.left) / kx, (r.top - srcArea.top) / ky,
                w / kx, h / ky, UnitPixel, &ia);
    c.r = r; c.src = src; c.gen = g_bgGen;
    return c.bmp;
}

// ───────────────────────── 아이콘 ─────────────────────────
static Bitmap* HbmToBitmap(HBITMAP hb) {
    BITMAP bm; if (!GetObject(hb, sizeof bm, &bm)) return nullptr;
    int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint32_t> px((size_t)w * h);
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    bool anyA = false, notPremul = false;
    for (uint32_t p : px) {
        uint32_t a = p >> 24;
        if (a) anyA = true;
        if (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a) notPremul = true;
    }
    for (uint32_t& p : px) {
        if (!anyA) { p |= 0xFF000000; continue; }
        if (notPremul) {
            uint32_t a = p >> 24;
            uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
            p = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    Bitmap* b = new Bitmap(w, h, PixelFormat32bppPARGB);
    BitmapData d; Rect rc(0, 0, w, h);
    if (b->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppPARGB, &d) == Ok) {
        for (int y = 0; y < h; y++) memcpy((char*)d.Scan0 + (size_t)y * d.Stride, &px[(size_t)y * w], w * 4);
        b->UnlockBits(&d);
    }
    return b;
}
static Bitmap* GetIcon(Item* it, int px) {
    if (it->icon && it->iconPx >= px) return it->icon;
    if (it->iconFailed && it->iconPx >= px) return it->icon;
    IShellItemImageFactory* f = nullptr;
    Bitmap* res = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(it->path.c_str(), nullptr, IID_IShellItemImageFactory, (void**)&f))) {
        HBITMAP hb = nullptr; SIZE sz = {px, px};
        if (SUCCEEDED(f->GetImage(sz, SIIGBF_RESIZETOFIT, &hb)) && hb) { res = HbmToBitmap(hb); DeleteObject(hb); }
        f->Release();
    }
    if (res) { delete it->icon; it->icon = res; it->iconFailed = false; }
    else it->iconFailed = true;
    it->iconPx = px;
    return it->icon;
}

// ───────────────────────── 그리기 ─────────────────────────
static Font* g_font = nullptr; static Font* g_fontBold = nullptr;
static FontFamily* g_ff = nullptr;

static void RoundPath(GraphicsPath& gp, RectF r, float rad) {
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    gp.AddArc(r.X, r.Y, d, d, 180, 90);
    gp.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    gp.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    gp.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    gp.CloseFigure();
}
static RectF RF(const RECT& r) { return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)); }
static bool Hits(const RECT& a, const RECT& b) { RECT t; return IntersectRect(&t, &a, &b) != 0; }

struct Geo { int ox, oy, cols; RECT clip; };
static RECT PanelRect(Item* it) {
    int w = it->pc * CW + 2 * PAD, h = HEAD + it->pr * CH + PAD;
    int x = it->px, y = it->py;
    if (x == INT_MIN) { const RECT& w0 = g_mons[0].work; x = (w0.left + w0.right - w) / 2; y = (w0.top + w0.bottom - h) / 2; }
    const RECT& wk = g_mons[MonAt({x + w / 2, y + std::min(h / 2, (int)(40 * S))})].work;
    x = std::max((int)wk.left, std::min(x, (int)wk.right - w));
    y = std::max((int)wk.top, std::min(y, (int)wk.bottom - h));
    return {x, y, x + w, y + h};
}
static Geo GeoRoot(int m) {
    const RECT& w = g_mons[m].work;
    Geo g; g.ox = w.left + MARGIN; g.oy = w.top + MARGIN; g.cols = RootCols(m); g.clip = w; return g;
}
static Geo GeoPanel(Item* it) {
    RECT P = PanelRect(it); Geo g;
    g.ox = P.left + PAD; g.oy = P.top + HEAD - it->scroll * CH; g.cols = it->pc;
    g.clip = {P.left + 4, P.top + HEAD - 2, P.right - 4, P.bottom - 6};
    return g;
}
static RECT CellRect(const Geo& g, Item* it) {
    int s = it->Span();
    return {g.ox + it->gx * CW, g.oy + it->gy * CH, g.ox + (it->gx + s) * CW, g.oy + (it->gy + s) * CH};
}
static int LayerOf(Container* c) {
    if (c == &g_root) return -1;
    for (int i = 0; i < (int)g_stack.size(); i++) if (g_stack[i]->child.get() == c) return i;
    return -2;
}
static bool ItemRect(Item* it, RECT& r) {
    int L = LayerOf(it->parent);
    if (L == -2) return false;
    if (L < 0 && (it->mon < 0 || it->mon >= (int)g_mons.size())) return false;
    r = CellRect(L < 0 ? GeoRoot(it->mon) : GeoPanel(g_stack[L]), it);
    return true;
}
// 아이콘(또는 폴더 타일) 사각형
static RECT IconRect(const RECT& R, Item* it) {
    int isz = it->Span() == 1 ? ICON1 : ICON2;
    int x = R.left + (R.right - R.left - isz) / 2, y = R.top + (int)(6 * S);
    return {x, y, x + isz, y + isz};
}

static void DrawLabel(Graphics& g, const std::wstring& t, RectF r) {
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsLineLimit);
    SolidBrush sh(Color(190, 0, 0, 0)), wb(Color(255, 255, 255, 255));
    RectF r2 = r; r2.X += 1; r2.Y += 1;
    g.DrawString(t.c_str(), -1, g_font, r2, &sf, &sh);
    g.DrawString(t.c_str(), -1, g_font, r, &sf, &wb);
}
static void DrawLock(Graphics& g, float x, float y, float s) {
    SolidBrush bg(Color(220, 255, 255, 255)), dark(Color(255, 60, 60, 70));
    g.FillEllipse(&bg, x, y, s, s);
    Pen p(Color(255, 60, 60, 70), s * 0.09f);
    g.DrawArc(&p, x + s * 0.33f, y + s * 0.18f, s * 0.34f, s * 0.38f, 180, 180);
    g.FillRectangle(&dark, x + s * 0.27f, y + s * 0.4f, s * 0.46f, s * 0.36f);
}
static void DrawLinkBadge(Graphics& g, float x, float y, float s) {
    SolidBrush bg(Color(235, 255, 255, 255));
    GraphicsPath gp; RoundPath(gp, RectF(x, y, s, s), s * 0.25f);
    g.FillPath(&bg, &gp);
    Pen p(Color(255, 40, 110, 220), s * 0.13f);
    p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
    g.DrawLine(&p, x + s * 0.3f, y + s * 0.7f, x + s * 0.7f, y + s * 0.3f);
    g.DrawLine(&p, x + s * 0.42f, y + s * 0.3f, x + s * 0.7f, y + s * 0.3f);
    g.DrawLine(&p, x + s * 0.7f, y + s * 0.3f, x + s * 0.7f, y + s * 0.58f);
}
static void DrawIconFit(Graphics& g, Bitmap* b, RectF r) {
    if (!b) return;
    float iw = (float)b->GetWidth(), ih = (float)b->GetHeight();
    float sc = std::min(r.Width / iw, r.Height / ih);
    if (sc > 0.98f && sc < 1.02f) sc = 1.0f;
    int w = (int)std::lround(iw * sc), h = (int)std::lround(ih * sc);
    int x = (int)std::lround(r.X + (r.Width - w) / 2), y = (int)std::lround(r.Y + (r.Height - h) / 2);
    g.DrawImage(b, Rect(x, y, w, h), 0, 0, (INT)iw, (INT)ih, UnitPixel);   // 정수 좌표 → 선명하게
}

static Item* g_dragItem = nullptr; static bool g_dragging = false;
static Item* g_dropTarget = nullptr;

static void DrawFolderTile(Graphics& g, Item* it, const RECT& T) {
    RectF tr = RF(T);
    float rad = tr.Width * 0.22f;
    GraphicsPath gp; RoundPath(gp, tr, rad);
    Bitmap* bb = GetBlurBg(it->bg, T, g_blurSmall, g_screen);
    TextureBrush tb(bb, WrapModeClamp);
    tb.TranslateTransform((REAL)T.left, (REAL)T.top);
    g.FillPath(&tb, &gp);
    SolidBrush tint(Color(70, 255, 255, 255));
    g.FillPath(&tint, &gp);
    Pen border(Color(80, 255, 255, 255), 1.0f);
    g.DrawPath(&border, &gp);
    // 3x3 미리보기
    Container* c = Child(it);
    std::vector<Item*> v;
    for (auto& x : c->items) v.push_back(x.get());
    size_t n = std::min<size_t>(9, v.size());
    std::partial_sort(v.begin(), v.begin() + n, v.end(), [](Item* a, Item* b) {
        return a->gy != b->gy ? a->gy < b->gy : a->gx < b->gx;
    });
    float pad = tr.Width * 0.11f, gap = tr.Width * 0.055f;
    float m = (tr.Width - 2 * pad - 2 * gap) / 3;
    for (size_t i = 0; i < n; i++) {
        float x = tr.X + pad + (i % 3) * (m + gap), y = tr.Y + pad + (i / 3) * (m + gap);
        Item* ci = v[i];
        if (ci->pop) {
            GraphicsPath mp; RoundPath(mp, RectF(x, y, m, m), m * 0.22f);
            SolidBrush mb(Color(120, 255, 255, 255));
            g.FillPath(&mb, &mp);
        } else DrawIconFit(g, GetIcon(ci, ICON1), RectF(x, y, m, m));
    }
    if (it->lnkFolder) DrawLinkBadge(g, tr.X + 2 * S, tr.GetBottom() - 20 * S, 18 * S);
}

static void DrawItem(Graphics& g, Item* it, const RECT& R, bool hl) {
    RECT ir = IconRect(R, it);
    if (hl || it == g_sel || it == g_dropTarget) {
        int a = it == g_dropTarget ? 110 : (it == g_sel ? 70 : 40);
        RECT h = R; InflateRect(&h, -(int)(3 * S), -(int)(2 * S));
        GraphicsPath gp; RoundPath(gp, RF(h), 8 * S);
        SolidBrush b(Color(a, 255, 255, 255));
        g.FillPath(&b, &gp);
    }
    if (it->pop) DrawFolderTile(g, it, ir);
    else DrawIconFit(g, GetIcon(it, ir.right - ir.left), RF(ir));
    if (it->pin) DrawLock(g, ir.right - 10 * S, ir.top - 4 * S, 16 * S);
    RectF lr((REAL)R.left + 2 * S, (REAL)ir.bottom + 4 * S, (REAL)(R.right - R.left) - 4 * S, (REAL)LINEH * 2 + 3);
    DrawLabel(g, it->Label(), lr);
}

static void DrawItems(Graphics& g, Container* c, const Geo& geo, int mon = -1) {
    for (auto& up : c->items) {
        Item* it = up.get();
        if (mon >= 0 && it->mon != mon) continue;
        if (g_dragging && it == g_dragItem) continue;
        RECT R = CellRect(geo, it);
        if (!Hits(R, geo.clip) || !Hits(R, g_paintRc)) continue;
        DrawItem(g, it, R, it == g_hover);
    }
}

static void DrawPanel(Graphics& g, Item* it) {
    RECT P = PanelRect(it);
    RECT big = P; InflateRect(&big, (int)(30 * S), (int)(30 * S));
    if (!Hits(big, g_paintRc)) return;
    RectF pr = RF(P);
    float rad = (float)RAD;
    for (int i = 4; i >= 1; i--) {   // 그림자
        GraphicsPath sp; RectF s2 = pr; s2.Inflate(i * 3 * S, i * 3 * S); s2.Y += 3 * S;
        RoundPath(sp, s2, rad + i * 3 * S);
        SolidBrush sb(Color(14, 0, 0, 0));
        g.FillPath(&sb, &sp);
    }
    GraphicsPath gp; RoundPath(gp, pr, rad);
    Bitmap* src = g_blurSmall; RECT area = g_screen;
    if (it->sceneBlur && EqualRect(&it->sceneRect, &P)) { src = it->sceneBlur; area = it->sceneRect; }
    Bitmap* bb = GetBlurBg(it->bg, P, src, area);
    TextureBrush tb(bb, WrapModeClamp);
    tb.TranslateTransform((REAL)P.left, (REAL)P.top);
    g.FillPath(&tb, &gp);
    SolidBrush tint(Color(80, 18, 18, 24));
    g.FillPath(&tint, &gp);
    Pen border(Color(70, 255, 255, 255), 1.0f);
    g.DrawPath(&border, &gp);
    // 제목
    StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter); sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush wb(Color(240, 255, 255, 255));
    g.DrawString(it->Label().c_str(), -1, g_fontBold, RectF(pr.X + 16 * S, pr.Y, pr.Width - 32 * S, (REAL)HEAD), &sf, &wb);
    // 내용
    Geo geo = GeoPanel(it);
    Region old; g.GetClip(&old);
    g.SetClip(RF(geo.clip), CombineModeIntersect);
    DrawItems(g, Child(it), geo);
    g.SetClip(&old);
    // 크기 조절 손잡이
    Pen gpn(Color(150, 255, 255, 255), 1.5f * S);
    float bx = pr.GetRight() - 8 * S, by = pr.GetBottom() - 8 * S;
    for (int i = 1; i <= 3; i++) g.DrawLine(&gpn, bx - i * 4 * S, by, bx, by - i * 4 * S);
}

static void SetupG(Graphics& g, bool opaque = false) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(opaque ? TextRenderingHintClearTypeGridFit : TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
}
static void DrawSceneBelow(Graphics& g, int depth) {
    for (int m = 0; m < (int)g_mons.size(); m++)
        if (Hits(g_mons[m].rc, g_paintRc)) DrawItems(g, &g_root, GeoRoot(m), m);
    for (int i = 0; i < depth && i < (int)g_stack.size(); i++) DrawPanel(g, g_stack[i]);
}

// 팝업 뒤 장면(아이콘 포함) 블러 – 열 때/놓을 때만
static void ComputeSceneBlur(Item* it, int depth) {
    RECT P = PanelRect(it);
    int w = P.right - P.left, h = P.bottom - P.top;
    Bitmap full(w, h, PixelFormat32bppPARGB);
    {
        Graphics g(&full); SetupG(g);
        g.TranslateTransform((REAL)-P.left, (REAL)-P.top);
        g.DrawImage(g_bgGp, Rect(P.left, P.top, w, h), P.left, P.top, w, h, UnitPixel);
        RECT saved = g_paintRc; g_paintRc = P;
        DrawSceneBelow(g, depth);
        g_paintRc = saved;
    }
    delete it->sceneBlur;
    it->sceneBlur = MakeSmallBlur(&full, 0, 0, w, h, std::max(16, w / 6));
    it->sceneRect = P;
}

// ───────────────────────── 애니메이션 ─────────────────────────
struct Anim { bool on = false, opening = true; Item* it = nullptr; RECT from{}, to{}; DWORD t0 = 0; Bitmap* snap = nullptr; } g_anim;
static const DWORD ANIM_MS = 190;

static RECT AnimUnion() {
    RECT u; UnionRect(&u, &g_anim.from, &g_anim.to);
    InflateRect(&u, (int)(40 * S), (int)(40 * S));
    return u;
}
static Bitmap* SnapshotPanel(Item* it) {
    RECT P = PanelRect(it);
    RECT b = P; InflateRect(&b, (int)(16 * S), (int)(16 * S));
    Bitmap* s = new Bitmap(b.right - b.left, b.bottom - b.top, PixelFormat32bppPARGB);
    Graphics g(s); SetupG(g);
    g.TranslateTransform((REAL)-b.left, (REAL)-b.top);
    RECT saved = g_paintRc; g_paintRc = b;
    DrawPanel(g, it);
    g_paintRc = saved;
    return s;
}
static RECT TileRect(Item* it) {
    RECT R;
    if (ItemRect(it, R)) return IconRect(R, it);
    RECT P = PanelRect(it); int cx = (P.left + P.right) / 2, cy = (P.top + P.bottom) / 2;
    return {cx - 10, cy - 10, cx + 10, cy + 10};
}
static void StartAnim(Item* it, bool opening) {
    delete g_anim.snap;
    g_anim.on = true; g_anim.opening = opening; g_anim.it = it;
    RECT P = PanelRect(it); RECT T = TileRect(it);
    g_anim.from = opening ? T : P; g_anim.to = opening ? P : T;
    g_anim.snap = SnapshotPanel(it);
    g_anim.t0 = GetTickCount();
    SetTimer(g_hwnd, TM_ANIM, 15, nullptr);
    InvalidateRect(g_hwnd, &g_anim.from, FALSE);
}
static void DrawAnim(Graphics& g) {
    if (!g_anim.on || !g_anim.snap) return;
    float t = std::min(1.0f, (GetTickCount() - g_anim.t0) / (float)ANIM_MS);
    float e = 1 - (1 - t) * (1 - t) * (1 - t);      // ease-out
    float a = g_anim.opening ? e : 1 - e;
    RECT f = g_anim.from, to = g_anim.to;
    float x = f.left + (to.left - f.left) * e, y = f.top + (to.top - f.top) * e;
    float w = (f.right - f.left) + ((to.right - to.left) - (f.right - f.left)) * e;
    float h = (f.bottom - f.top) + ((to.bottom - to.top) - (f.bottom - f.top)) * e;
    RECT P = g_anim.opening ? g_anim.to : g_anim.from;
    float pw = (float)(P.right - P.left), ph = (float)(P.bottom - P.top);
    float sx = w / pw, sy = h / ph;
    float m = 16 * S;
    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, 0.25f + 0.75f * a, 0}, {0, 0, 0, 0, 1}}};
    ImageAttributes ia; ia.SetColorMatrix(&cm);
    RectF dst(x - m * sx, y - m * sy, (pw + 2 * m) * sx, (ph + 2 * m) * sy);
    g.DrawImage(g_anim.snap, dst, 0, 0, (REAL)g_anim.snap->GetWidth(), (REAL)g_anim.snap->GetHeight(), UnitPixel, &ia);
}

// ───────────────────────── 팝업 열기/닫기 ─────────────────────────
static void OpenPopup(Item* it) {
    if (g_anim.on) return;
    if (it->child && it->child->external) { it->child.reset(); }
    Child(it);
    if (it->px == INT_MIN) {
        RECT T = TileRect(it);
        const RECT& wk = g_mons[MonAt({(T.left + T.right) / 2, (T.top + T.bottom) / 2})].work;
        int w = it->pc * CW + 2 * PAD, h = HEAD + it->pr * CH + PAD;
        it->px = (wk.left + wk.right - w) / 2; it->py = (wk.top + wk.bottom - h) / 2;
        RECT P = PanelRect(it); it->px = P.left; it->py = P.top;
    }
    it->scroll = 0;
    ComputeSceneBlur(it, (int)g_stack.size());
    StartAnim(it, true);
    ScheduleSave();
}
static void ClosePopups(int keepCount, bool animate) {
    if (g_anim.on) return;
    while ((int)g_stack.size() > keepCount) {
        Item* top = g_stack.back();
        bool last = (int)g_stack.size() == keepCount + 1;
        RECT P = PanelRect(top);
        g_stack.pop_back();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        (void)P;
        if (last && animate) StartAnim(top, false);
    }
    g_sel = nullptr;
}

// ───────────────────────── 히트 테스트 ─────────────────────────
enum Area { A_NONE, A_ITEM, A_BODY, A_HEADER, A_GRIP, A_ROOT };
struct Hit { int layer = -1; Item* item = nullptr; Area area = A_ROOT; int mon = 0; };
static Item* ItemAt(Container* c, const Geo& geo, POINT pt, Item* ignore, int mon = -1) {
    if (!PtInRect(&geo.clip, pt)) return nullptr;
    for (auto& up : c->items) {
        Item* it = up.get();
        if (it == ignore || (mon >= 0 && it->mon != mon)) continue;
        RECT R = CellRect(geo, it); InflateRect(&R, -(int)(3 * S), -(int)(2 * S));
        if (PtInRect(&R, pt)) return it;
    }
    return nullptr;
}
static Hit HitTest(POINT pt, Item* ignore = nullptr) {
    Hit h;
    for (int i = (int)g_stack.size() - 1; i >= 0; i--) {
        Item* p = g_stack[i];
        RECT P = PanelRect(p);
        if (!PtInRect(&P, pt)) continue;
        h.layer = i;
        if (pt.x > P.right - 22 * S && pt.y > P.bottom - 22 * S) { h.area = A_GRIP; return h; }
        if (pt.y < P.top + HEAD - 4) { h.area = A_HEADER; return h; }
        h.item = ItemAt(Child(p), GeoPanel(p), pt, ignore);
        h.area = h.item ? A_ITEM : A_BODY;
        return h;
    }
    h.layer = -1;
    h.mon = MonAt(pt);
    h.item = ItemAt(&g_root, GeoRoot(h.mon), pt, ignore, h.mon);
    h.area = h.item ? A_ITEM : A_ROOT;
    return h;
}
static Container* ContOfLayer(int L) { return L < 0 ? &g_root : Child(g_stack[L]); }
static Geo GeoOfLayer(int L, int mon) { return L < 0 ? GeoRoot(mon) : GeoPanel(g_stack[L]); }
static void CellAt(int L, int mon, POINT pt, int& cx, int& cy) {
    Geo g = GeoOfLayer(L, mon);
    cx = (int)std::floor((pt.x - g.ox) / (double)CW);
    cy = (int)std::floor((pt.y - g.oy) / (double)CH);
    if (cx < 0) cx = 0; if (cy < 0) cy = 0;
}

// ───────────────────────── 파일 작업 ─────────────────────────
static bool ShellOp(UINT func, const std::vector<std::wstring>& from, const std::wstring& to, FILEOP_FLAGS flags) {
    std::wstring f = DblNull(from), t;
    if (!to.empty()) { t = to; t.push_back(L'\0'); t.push_back(L'\0'); }
    SHFILEOPSTRUCTW op = {};
    op.hwnd = g_hwnd; op.wFunc = func; op.pFrom = f.c_str(); op.pTo = to.empty() ? nullptr : t.c_str();
    op.fFlags = flags;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
static void MoveItemTo(Item* it, const std::wstring& dstDir, int gx, int gy, int mon = 0) {
    if (it->pin) return;
    std::wstring srcDir = it->folder;
    if (!_wcsicmp(srcDir.c_str(), dstDir.c_str())) return;
    if (it->isDir && (StartsWithI(dstDir + L"\\", it->path + L"\\"))) return;  // 자기 안으로 이동 금지
    std::wstring newPath = dstDir + L"\\" + it->name;
    if (Exists(newPath)) { MessageBoxW(g_hwnd, L"같은 이름의 항목이 이미 있어요.", L"팝업 바탕화면", MB_ICONINFORMATION); return; }
    SyncEntry(it);
    if (ShellOp(FO_MOVE, {it->path}, dstDir, FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR)) {
        MoveKeys(it->path, newPath);
        Entry& e = g_lay[Key(newPath)];
        e.gx = gx; e.gy = gy; e.m = mon;
    }
    RebuildAll();
}
static void DeleteItem(Item* it) {
    if (it->pin) return;
    ShellOp(FO_DELETE, {it->path}, L"", FOF_ALLOWUNDO);
    if (!Exists(it->path)) g_lay.erase(Key(it->path));
    RebuildAll();
}
static void Launch(Item* it) {
    ShellExecuteW(g_hwnd, nullptr, it->path.c_str(), nullptr, it->folder.c_str(), SW_SHOWNORMAL);
}

// ───────────────────────── 이름 바꾸기 ─────────────────────────
static HWND g_edit = nullptr; static Item* g_editItem = nullptr; static WNDPROC g_editOld = nullptr;
static HFONT g_editFont = nullptr;
static void EndRename(bool commit) {
    if (!g_edit) return;
    HWND e = g_edit; g_edit = nullptr;
    if (commit && g_editItem) {
        wchar_t buf[MAX_PATH] = {}; GetWindowTextW(e, buf, MAX_PATH);
        std::wstring nn = buf;
        Item* it = g_editItem;
        std::wstring ext = ExtOf(it->name);
        if (!it->isDir && (ext == L"lnk" || ext == L"url")) nn += L"." + ext;
        if (!nn.empty() && nn != it->name && nn.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos) {
            std::wstring np = it->folder + L"\\" + nn;
            SyncEntry(it);
            if (!Exists(np) && MoveFileW(it->path.c_str(), np.c_str())) MoveKeys(it->path, np);
            else MessageBoxW(g_hwnd, L"이름을 바꿀 수 없어요.", L"팝업 바탕화면", MB_ICONINFORMATION);
        }
    }
    g_editItem = nullptr;
    DestroyWindow(e);
    RebuildAll();
}
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) { EndRename(true); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { EndRename(false); return 0; }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
    if (m == WM_KILLFOCUS) { LRESULT r = CallWindowProcW(g_editOld, h, m, w, l); PostMessageW(g_hwnd, WM_APP + 3, 0, 0); return r; }
    return CallWindowProcW(g_editOld, h, m, w, l);
}
static void StartRename(Item* it) {
    if (it->pin) return;
    RECT R; if (!ItemRect(it, R)) return;
    RECT ir = IconRect(R, it);
    int x = R.left - (int)(10 * S), w = R.right - R.left + (int)(20 * S);
    g_editItem = it;
    g_edit = CreateWindowExW(0, L"EDIT", it->Label().c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_AUTOHSCROLL,
                             x, ir.bottom + (int)(3 * S), w, LINEH + (int)(8 * S), g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    g_editOld = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_edit);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
}

// 새 폴더 만들기
static void NewFolder(int L, int mon, int cx, int cy, bool popup) {
    Container* c = ContOfLayer(L);
    std::wstring name = UniqueName(c->dir, popup ? L"새 팝업 폴더" : L"새 폴더");
    std::wstring p = c->dir + L"\\" + name;
    if (!CreateDirectoryW(p.c_str(), nullptr)) return;
    Entry& e = g_lay[Key(p)];
    e.pop = popup;
    int s = popup ? 2 : 1, ox = cx, oy = cy;
    // 같은 컨테이너 자리 확인용 임시 아이템 없이 근처 빈칸
    Item tmp; tmp.gs = s; tmp.pop = popup;
    e.m = mon;
    if (NearestFree(c, nullptr, s, cx, cy, ox, oy, mon)) { e.gx = ox; e.gy = oy; }
    RebuildAll();
    for (auto& it : ContOfLayer(L < (int)g_stack.size() ? L : -1)->items)
        if (Key(it->path) == Key(p)) { StartRename(it.get()); break; }
}
static void LinkFolder(int L, int mon, int cx, int cy) {
    std::wstring t = PickFolder(g_hwnd, L"팝업으로 연결할 폴더를 고르세요");
    if (t.empty()) return;
    Container* c = ContOfLayer(L);
    std::wstring nm = FileName(t); if (nm.empty() || nm.back() == L':') nm = t.substr(0, 1) + L" 드라이브";
    std::wstring lnk = c->dir + L"\\" + UniqueName(c->dir, nm + L".lnk").c_str();
    if (!CreateLnk(lnk, t)) return;
    Entry& e = g_lay[Key(lnk)];
    e.pop = true;
    e.m = mon;
    int ox, oy; if (NearestFree(c, nullptr, 2, cx, cy, ox, oy, mon)) { e.gx = ox; e.gy = oy; }
    RebuildAll();
}

// ───────────────────────── 첫 실행: 바탕화면 가져오기 ─────────────────────────
static std::wstring Category(const WIN32_FIND_DATAW& fd) {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return L"폴더";
    std::wstring e = ExtOf(fd.cFileName);
    auto in = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (e == x) return true; return false; };
    if (in({L"lnk", L"url", L"exe", L"msi", L"bat", L"cmd", L"appref-ms"})) return L"바로가기";
    if (in({L"txt", L"doc", L"docx", L"hwp", L"hwpx", L"pdf", L"xls", L"xlsx", L"xlsm", L"csv", L"ppt", L"pptx", L"rtf", L"md", L"odt"})) return L"문서";
    if (in({L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"tif", L"tiff", L"svg", L"ico", L"psd"})) return L"이미지";
    if (in({L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"flv", L"webm", L"ts", L"m4v"})) return L"영상";
    if (in({L"mp3", L"wav", L"flac", L"m4a", L"aac", L"ogg", L"wma"})) return L"음악";
    if (in({L"zip", L"rar", L"7z", L"tar", L"gz", L"egg", L"alz", L"iso"})) return L"압축";
    return L"기타";
}
// ── 백업 모드: 목록에서 골라 복사 ──
struct ImpRow { std::wstring path, name, cat; unsigned long long size = 0; };
static std::vector<ImpRow> g_imp;
static HWND g_impList = nullptr, g_impInfo = nullptr, g_impOkBtn = nullptr;
static int g_impResult = 0;
static unsigned long long g_impFree = 0;
static HFONT g_uiFont = nullptr;
static void ImpRefresh() {
    unsigned long long sum = 0; int cnt = 0;
    for (int i = 0; i < (int)g_imp.size(); i++) if (ListView_GetCheckState(g_impList, i)) { sum += g_imp[i].size; cnt++; }
    std::wstring t = L"선택 " + std::to_wstring(cnt) + L"개 · " + SizeStr(sum) + L"   |   저장할 곳 남은 공간 " + SizeStr(g_impFree);
    if (sum > g_impFree) t += L"   (공간 부족 — 큰 항목을 빼 주세요)";
    SetWindowTextW(g_impInfo, t.c_str());
    EnableWindow(g_impOkBtn, cnt > 0 && sum <= g_impFree);
}
static LRESULT CALLBACK ImpProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->hwndFrom == g_impList && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* nl = (NMLISTVIEW*)l;
            if ((nl->uChanged & LVIF_STATE) && ((nl->uNewState ^ nl->uOldState) & LVIS_STATEIMAGEMASK)) ImpRefresh();
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDOK: g_impResult = 1; break;
        case IDCANCEL: g_impResult = 2; break;
        case 3001: case 3002: case 3003:
            for (int i = 0; i < (int)g_imp.size(); i++)
                ListView_SetCheckState(g_impList, i, LOWORD(w) == 3001 || (LOWORD(w) == 3003 && g_imp[i].size < 100ull * 1048576));
            ImpRefresh();
            break;
        }
        return 0;
    case WM_CLOSE: g_impResult = 2; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static bool ImportDialog(std::vector<bool>& checks) {
    static bool reg = false;
    g_impInfo = g_impOkBtn = nullptr;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = ImpProc; wc.hInstance = hi; wc.lpszClassName = L"PopupDesktopImport";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc); reg = true;
    }
    ULARGE_INTEGER fr; g_impFree = GetDiskFreeSpaceExW(g_backupData.c_str(), &fr, nullptr, nullptr) ? fr.QuadPart : 0;
    int W = (int)(660 * S), H = (int)(560 * S);
    const RECT& wk = g_mons[0].work;
    int x = g_virt.x + (wk.left + wk.right - W) / 2, y = g_virt.y + (wk.top + wk.bottom - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PopupDesktopImport", L"바탕화면 자료 가져오기 (백업 모드)",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, g_hwnd, nullptr, hi, nullptr);
    RECT cr; GetClientRect(dlg, &cr);
    int cw = cr.right, ch = cr.bottom, m = (int)(12 * S), bh = (int)(30 * S), bw = (int)(110 * S);
    auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int X, int Y, int Wd, int Ht, int id) {
        HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | st, X, Y, Wd, Ht, dlg, (HMENU)(INT_PTR)id, hi, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        return c;
    };
    mk(L"STATIC", L"가져올 항목을 체크하세요. 체크한 항목만 '내 문서\\팝업 바탕화면'에 복사돼요. (원본은 그대로)", 0, m, m, cw - 2 * m, (int)(20 * S), 0);
    g_impList = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, m, m + (int)(26 * S), cw - 2 * m, ch - (int)(26 * S) - 3 * m - 2 * bh, 0);
    ListView_SetExtendedListViewStyle(g_impList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col = {}; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.pszText = (LPWSTR)L"이름"; col.cx = cw - 2 * m - (int)(230 * S); ListView_InsertColumn(g_impList, 0, &col);
    col.pszText = (LPWSTR)L"분류"; col.cx = (int)(90 * S); ListView_InsertColumn(g_impList, 1, &col);
    col.fmt = LVCFMT_RIGHT; col.pszText = (LPWSTR)L"크기"; col.cx = (int)(110 * S); ListView_InsertColumn(g_impList, 2, &col);
    for (int i = 0; i < (int)g_imp.size(); i++) {
        LVITEMW it = {}; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (LPWSTR)g_imp[i].name.c_str();
        ListView_InsertItem(g_impList, &it);
        ListView_SetItemText(g_impList, i, 1, (LPWSTR)g_imp[i].cat.c_str());
        std::wstring sz = SizeStr(g_imp[i].size);
        ListView_SetItemText(g_impList, i, 2, (LPWSTR)sz.c_str());
        ListView_SetCheckState(g_impList, i, g_imp[i].size < 100ull * 1048576);
    }
    int by = ch - m - bh;
    g_impInfo = mk(L"STATIC", L"", 0, m, by - bh, cw - 2 * m, (int)(22 * S), 0);
    mk(L"BUTTON", L"전체 선택", BS_PUSHBUTTON, m, by, bw, bh, 3001);
    mk(L"BUTTON", L"전체 해제", BS_PUSHBUTTON, m + bw + m / 2, by, bw, bh, 3002);
    mk(L"BUTTON", L"100MB 미만만", BS_PUSHBUTTON, m + 2 * (bw + m / 2), by, bw, bh, 3003);
    mk(L"BUTTON", L"취소", BS_PUSHBUTTON, cw - m - bw, by, bw, bh, IDCANCEL);
    g_impOkBtn = mk(L"BUTTON", L"가져오기", BS_DEFPUSHBUTTON, cw - 2 * m - 2 * bw + m / 2, by, bw, bh, IDOK);
    ImpRefresh();
    ShowWindow(dlg, SW_SHOW); SetForegroundWindow(dlg);
    EnableWindow(g_hwnd, FALSE);
    g_impResult = 0;
    MSG msg;
    while (g_impResult == 0 && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    checks.assign(g_imp.size(), false);
    for (int i = 0; i < (int)g_imp.size(); i++) checks[i] = ListView_GetCheckState(g_impList, i) != 0;
    EnableWindow(g_hwnd, TRUE);
    DestroyWindow(dlg);
    return g_impResult == 1;
}
static void ImportDesktop() {
    g_imp.clear();
    HCURSOR oc = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    for (auto& d : {g_desktopDir, g_publicDesktop}) {
        if (d.empty()) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
            ImpRow r;
            r.path = d + L"\\" + fd.cFileName;
            if (!_wcsicmp(r.path.c_str(), g_exePath.c_str())) continue;
            r.name = fd.cFileName; r.cat = Category(fd);
            r.size = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DirSize(r.path)
                     : (((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
            g_imp.push_back(r);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetCursor(oc);
    if (g_imp.empty()) { MessageBoxW(g_hwnd, L"가져올 바탕화면 자료가 없어요.", L"팝업 바탕화면", MB_ICONINFORMATION); return; }
    const wchar_t* order[] = {L"바로가기", L"폴더", L"문서", L"이미지", L"영상", L"음악", L"압축", L"기타"};
    auto rank = [&](const std::wstring& c) { for (int i = 0; i < 8; i++) if (c == order[i]) return i; return 8; };
    std::stable_sort(g_imp.begin(), g_imp.end(), [&](const ImpRow& a, const ImpRow& b) {
        int ra = rank(a.cat), rb = rank(b.cat); return ra != rb ? ra < rb : _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    std::vector<bool> chk;
    if (!ImportDialog(chk)) return;
    std::map<std::wstring, std::vector<std::wstring>> cats;
    for (size_t i = 0; i < g_imp.size(); i++) if (chk[i]) cats[g_imp[i].cat].push_back(g_imp[i].path);
    for (auto cn : order) {
        auto f = cats.find(cn);
        if (f == cats.end()) continue;
        std::wstring dir = g_backupData + L"\\" + cn;
        CreateDirectoryW(dir.c_str(), nullptr);
        ShellOp(FO_COPY, f->second, dir, FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION);
        Entry& e = g_lay[Key(dir)];
        if (!e.pop) {
            e.pop = true; e.gx = -1; e.gy = -1;
            int n = (int)f->second.size();
            e.pc = n <= 4 ? std::max(2, n) : (n <= 12 ? 4 : 5);
            e.pr = std::max(1, std::min(4, (n + e.pc - 1) / e.pc));
        }
    }
    RebuildAll();
    MessageBoxW(g_hwnd, L"복사가 끝났어요.\n\n이제 기존 바탕화면은 정리하시는 걸 추천드립니다.\n(원본 파일은 그대로 남아 있어요)",
                L"팝업 바탕화면", MB_ICONINFORMATION);
}

// ── 연동 모드: 윈도우 바탕화면 아이콘 위치 읽기 (원본은 건드리지 않음) ──
static const GUID kSID_STopLevelBrowser = {0x4C96BE40, 0x915C, 0x11CF, {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};
static void ImportWinPositions() {
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_IShellWindows, (void**)&sw))) return;
    VARIANT ve; VariantInit(&ve);
    long hwnd = 0; IDispatch* disp = nullptr;
    std::map<std::wstring, POINT> pos; POINT spacing = {0, 0};
    if (SUCCEEDED(sw->FindWindowSW(&ve, &ve, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp)) && disp) {
        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_IServiceProvider, (void**)&sp))) {
            IShellBrowser* sb = nullptr;
            if (SUCCEEDED(sp->QueryService(kSID_STopLevelBrowser, IID_IShellBrowser, (void**)&sb))) {
                IShellView* sv = nullptr;
                if (SUCCEEDED(sb->QueryActiveShellView(&sv))) {
                    IFolderView* fv = nullptr;
                    if (SUCCEEDED(sv->QueryInterface(IID_IFolderView, (void**)&fv))) {
                        fv->GetSpacing(&spacing);
                        IShellFolder* sf = nullptr;
                        if (SUCCEEDED(fv->GetFolder(IID_IShellFolder, (void**)&sf))) {
                            int n = 0; fv->ItemCount(SVGIO_ALLVIEW, &n);
                            for (int i = 0; i < n; i++) {
                                PITEMID_CHILD pidl = nullptr;
                                if (FAILED(fv->Item(i, &pidl)) || !pidl) continue;
                                POINT pt;
                                STRRET sr;
                                if (SUCCEEDED(fv->GetItemPosition(pidl, &pt)) && SUCCEEDED(sf->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &sr))) {
                                    LPWSTR nm = nullptr;
                                    if (SUCCEEDED(StrRetToStrW(&sr, pidl, &nm)) && nm) { pos[Lower(nm)] = pt; CoTaskMemFree(nm); }
                                }
                                CoTaskMemFree(pidl);
                            }
                            sf->Release();
                        }
                        fv->Release();
                    }
                    sv->Release();
                }
                sb->Release();
            }
            sp->Release();
        }
        disp->Release();
    }
    sw->Release();
    if (pos.empty()) return;
    if (spacing.x < 20) spacing.x = CW;
    if (spacing.y < 20) spacing.y = CH;
    for (auto& up : g_root.items) {
        Item* it = up.get();
        auto f = pos.find(Lower(it->path));
        if (f == pos.end()) continue;
        POINT p = f->second;
        POINT c = {p.x + spacing.x / 2, p.y + spacing.y / 2};
        int m = MonAt(c);
        const RECT& wk = g_mons[m].work;
        it->mon = m;
        it->gx = std::max(0, (int)std::lround((p.x - wk.left) / (double)spacing.x));
        it->gy = std::max(0, (int)std::lround((p.y - wk.top) / (double)spacing.y));
        if (it->gx >= RootCols(m) || it->gy >= RootRows(m)) it->gx = it->gy = -1;
        SyncEntry(it);
    }
    g_set.posImported = true;
    RebuildAll();
}

// ── 모드 ──
static DWORD WINAPI WatchThread(LPVOID);
static void ApplyMode(int mode) {
    SaveTree(&g_root);
    g_set.mode = mode;
    g_root.extraDirs.clear();
    if (mode == 0) {
        g_dataDir = g_desktopDir;
        if (!g_publicDesktop.empty() && IsDirPath(g_publicDesktop)) g_root.extraDirs.push_back(g_publicDesktop);
    } else g_dataDir = g_backupData;
    g_root.dir = g_dataDir;
    InterlockedIncrement(&g_watchGen);
    CreateThread(nullptr, 0, WatchThread, (LPVOID)(LONG_PTR)g_watchGen, 0, nullptr);
    g_stack.clear();
    RebuildAll();
}
static int AskMode() {
    TASKDIALOG_BUTTON btns[] = {
        {1001, L"연동 모드\n실제 바탕화면 폴더를 그대로 써요. 복사하지 않아 용량이 들지 않고, 삭제·새 폴더는 실제 바탕화면에도 반영돼요. "
               L"아이콘 위치는 지금 바탕화면 배치를 읽어 오고, 이후 옮기는 건 이 프로그램에만 저장돼요."},
        {1002, L"백업 모드\n가져올 자료를 목록에서 골라 '내 문서\\팝업 바탕화면'에 복사해요. 원본 바탕화면과는 따로 관리돼요."}};
    TASKDIALOGCONFIG c = {};
    c.cbSize = sizeof c; c.hwndParent = g_hwnd;
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.pszWindowTitle = L"팝업 바탕화면";
    c.pszMainInstruction = L"바탕화면 자료를 어떻게 사용할까요?";
    c.pszContent = L"나중에 바탕화면 우클릭 → 모드에서 바꿀 수 있어요.";
    c.cButtons = 2; c.pButtons = btns; c.nDefaultButton = 1001;
    int b = 0;
    if (FAILED(TaskDialogIndirect(&c, &b, nullptr, nullptr))) return -1;
    return b == 1001 ? 0 : (b == 1002 ? 1 : -1);
}
static void ChooseMode(int mode) {
    if (mode < 0) return;
    ApplyMode(mode);
    g_set.firstRunDone = true;
    ScheduleSave();
    if (mode == 0) {
        if (!g_set.posImported) ImportWinPositions();
        MessageBoxW(g_hwnd, L"바탕화면을 그대로 연결했어요.\n\n이제 기존 바탕화면 정리는 이 프로그램에서 하시는 걸 추천드립니다.\n"
                            L"(원래 윈도우 바탕화면의 아이콘 위치는 바뀌지 않아요)", L"팝업 바탕화면", MB_ICONINFORMATION);
    } else {
        bool empty = true;
        for (auto& it : g_root.items) { (void)it; empty = false; break; }
        if (empty) ImportDesktop();
    }
}

// ───────────────────────── 창 / 바탕화면 붙이기 ─────────────────────────
static HWND FindDesktopHost() {
    HWND found = nullptr;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) { *(HWND*)lp = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&found);
    if (!found) found = FindWindowW(L"Progman", nullptr);
    return found;
}
static void AttachToDesktop() {
    HWND host = FindDesktopHost();
    if (host) SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, (LONG_PTR)host);
    SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static void UpdateMetrics() {
    g_virt = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
    g_screen = {0, 0, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    struct T { RECT rc, work; bool primary; };
    std::vector<T> v;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO mi = {sizeof mi};
        if (GetMonitorInfoW(hm, &mi)) ((std::vector<T>*)lp)->push_back({mi.rcMonitor, mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE;
    }, (LPARAM)&v);
    std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) {
        if (a.primary != b.primary) return a.primary;
        return a.rc.left != b.rc.left ? a.rc.left < b.rc.left : a.rc.top < b.rc.top;
    });
    g_mons.clear();
    for (auto& t : v) {
        Mon m{t.rc, t.work};
        OffsetRect(&m.rc, -g_virt.x, -g_virt.y); OffsetRect(&m.work, -g_virt.x, -g_virt.y);
        g_mons.push_back(m);
    }
    if (g_mons.empty()) g_mons.push_back({g_screen, g_screen});
}
static bool AutoStartOn() {
    HKEY k; bool on = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, L"PopupDesktop", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}
static void SetAutoStart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + g_exePath + L"\"";
        RegSetValueExW(k, L"PopupDesktop", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2));
    } else RegDeleteValueW(k, L"PopupDesktop");
    RegCloseKey(k);
}

// ───────────────────────── 메뉴 ─────────────────────────
static const int kSizes[][2] = {{1, 2}, {2, 2}, {2, 4}, {3, 3}, {4, 3}, {4, 4}, {5, 4}, {6, 4}};
static HMENU SizeMenu(Item* it) {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < 8; i++) {
        wchar_t b[32]; swprintf(b, 32, L"가로 %d × 세로 %d", kSizes[i][0], kSizes[i][1]);
        AppendMenuW(m, MF_STRING | ((it->pc == kSizes[i][0] && it->pr == kSizes[i][1]) ? MF_CHECKED : 0), 200 + i, b);
    }
    return m;
}
static void ApplyPopupSize(Item* it, int c, int r) {
    it->pc = c; it->pr = r;
    if (it->child) { for (auto& x : it->child->items) if (x->gx + x->Span() > c) x->gx = -1; AutoPlace(it->child.get()); }
    int d = -1; for (int i = 0; i < (int)g_stack.size(); i++) if (g_stack[i] == it) d = i;
    if (d >= 0) ComputeSceneBlur(it, d);
    SyncEntry(it); ScheduleSave();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void ShowMenu(POINT pt) {
    Hit h = HitTest(pt);
    if (h.layer < (int)g_stack.size() - 1) ClosePopups(h.layer + 1, false);
    POINT sp = pt; ClientToScreen(g_hwnd, &sp);
    HMENU m = CreatePopupMenu();
    Item* it = h.item;
    Item* panel = h.layer >= 0 ? g_stack[h.layer] : nullptr;
    if (it) {
        g_sel = it; InvalidateRect(g_hwnd, nullptr, FALSE);
        AppendMenuW(m, MF_STRING, 301, it->pop ? L"팝업 열기" : L"열기");
        if (it->FolderLike()) AppendMenuW(m, MF_STRING, 302, L"탐색기로 열기");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        UINT lk = it->pin ? MF_GRAYED : 0;
        if (it->FolderLike()) AppendMenuW(m, MF_STRING | lk, it->pop ? 304 : 303, it->pop ? L"일반 폴더로 되돌리기" : L"팝업화");
        if (it->pop) AppendMenuW(m, MF_POPUP | lk, (UINT_PTR)SizeMenu(it), L"팝업 크기");
        else {
            HMENU sm = CreatePopupMenu();
            AppendMenuW(sm, MF_STRING | (it->gs == 1 ? MF_CHECKED : 0), 310, L"1 × 1");
            AppendMenuW(sm, MF_STRING | (it->gs == 2 ? MF_CHECKED : 0), 311, L"2 × 2");
            AppendMenuW(m, MF_POPUP | lk, (UINT_PTR)sm, L"아이콘 크기");
        }
        AppendMenuW(m, MF_STRING | (it->pin ? MF_CHECKED : 0), 305, it->pin ? L"고정 해제" : L"고정");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | lk, 306, L"이름 바꾸기");
        AppendMenuW(m, MF_STRING | lk, 307, L"삭제");
    } else {
        AppendMenuW(m, MF_STRING, 101, L"새 팝업 폴더");
        AppendMenuW(m, MF_STRING, 102, L"새 폴더");
        AppendMenuW(m, MF_STRING, 103, L"바로가기 폴더 연결...");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        if (panel) {
            AppendMenuW(m, MF_POPUP, (UINT_PTR)SizeMenu(panel), L"팝업 크기");
            AppendMenuW(m, MF_STRING, 104, L"탐색기로 열기");
        } else {
            HMENU wm = CreatePopupMenu();
            AppendMenuW(wm, MF_STRING | (g_set.wallMode == 0 ? MF_CHECKED : 0), 110, L"윈도우 배경화면 사용");
            AppendMenuW(wm, MF_STRING | (g_set.wallMode == 1 ? MF_CHECKED : 0), 111, L"전용 배경화면 선택...");
            AppendMenuW(m, MF_POPUP, (UINT_PTR)wm, L"배경화면");
            HMENU mm = CreatePopupMenu();
            AppendMenuW(mm, MF_STRING | (g_set.mode == 0 ? MF_CHECKED : 0), 120, L"연동 모드 (실제 바탕화면 사용)");
            AppendMenuW(mm, MF_STRING | (g_set.mode == 1 ? MF_CHECKED : 0), 121, L"백업 모드 (골라서 복사)");
            AppendMenuW(m, MF_POPUP, (UINT_PTR)mm, L"모드");
            if (g_set.mode == 1) AppendMenuW(m, MF_STRING, 112, L"바탕화면 자료 골라서 가져오기...");
            else AppendMenuW(m, MF_STRING, 116, L"윈도우 바탕화면 배치 다시 읽기");
            AppendMenuW(m, MF_STRING, 113, L"자료 폴더 열기");
            AppendMenuW(m, MF_STRING | (AutoStartOn() ? MF_CHECKED : 0), 114, L"윈도우 시작 시 자동 실행");
            AppendMenuW(m, MF_STRING, 115, L"새로고침");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 199, L"프로그램 종료");
        }
    }
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    int cx, cy; CellAt(h.layer, h.mon, pt, cx, cy);
    Item* sizeTarget = it ? it : panel;
    switch (cmd) {
    case 301: if (it->pop) OpenPopup(it); else Launch(it); break;
    case 302: ShellExecuteW(g_hwnd, nullptr, it->Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case 303: {
        it->pop = true;
        if (!ContOfLayer(h.layer)) break;
        int ox, oy;
        if (NearestFree(it->parent, it, 2, it->gx, it->gy, ox, oy, it->mon)) { it->gx = ox; it->gy = oy; }
        SyncEntry(it); RebuildAll(); break;
    }
    case 304: it->pop = false; it->gs = 1; SyncEntry(it); RebuildAll(); break;
    case 305: it->pin = !it->pin; SyncEntry(it); ScheduleSave(); InvalidateRect(g_hwnd, nullptr, FALSE); break;
    case 306: StartRename(it); break;
    case 307: DeleteItem(it); break;
    case 310: case 311: {
        int s = cmd - 309, ox, oy;
        if (NearestFree(it->parent, it, s, it->gx, it->gy, ox, oy, it->mon)) { it->gs = s; it->gx = ox; it->gy = oy; delete it->icon; it->icon = nullptr; it->iconPx = 0; }
        SyncEntry(it); ScheduleSave(); InvalidateRect(g_hwnd, nullptr, FALSE); break;
    }
    case 101: NewFolder(h.layer, h.mon, cx, cy, true); break;
    case 102: NewFolder(h.layer, h.mon, cx, cy, false); break;
    case 103: LinkFolder(h.layer, h.mon, cx, cy); break;
    case 104: ShellExecuteW(g_hwnd, nullptr, panel->Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case 110: g_set.wallMode = 0; LoadBackground(); ScheduleSave(); InvalidateRect(g_hwnd, nullptr, FALSE); break;
    case 111: {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW of = {}; of.lStructSize = sizeof of; of.hwndOwner = g_hwnd;
        of.lpstrFilter = L"그림 파일\0*.jpg;*.jpeg;*.png;*.bmp;*.gif\0모든 파일\0*.*\0";
        of.lpstrFile = file; of.nMaxFile = MAX_PATH; of.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameW(&of)) {
            std::wstring ext = ExtOf(file);
            std::wstring dst = g_baseDir + L"\\배경화면." + (ext.empty() ? L"jpg" : ext);
            CopyFileW(file, dst.c_str(), FALSE);   // 재설치해도 남도록 내 폴더에 보관
            g_set.wallMode = 1; g_set.wallPath = dst;
            LoadBackground(); ScheduleSave(); InvalidateRect(g_hwnd, nullptr, FALSE);
        }
        break;
    }
    case 112: ImportDesktop(); break;
    case 116:
        if (MessageBoxW(g_hwnd, L"윈도우 바탕화면의 현재 아이콘 위치대로 다시 배치할까요?\n(지금 이 프로그램에서 옮겨 둔 위치는 바뀌어요)",
                        L"팝업 바탕화면", MB_YESNO | MB_ICONQUESTION) == IDYES) ImportWinPositions();
        break;
    case 120: case 121:
        if (g_set.mode != cmd - 120) ChooseMode(cmd - 120);
        break;
    case 113: ShellExecuteW(g_hwnd, nullptr, g_dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case 114: SetAutoStart(!AutoStartOn()); break;
    case 115: LoadBackground(); RebuildAll(); break;
    case 199: DestroyWindow(g_hwnd); break;
    default:
        if (cmd >= 200 && cmd < 208 && sizeTarget) ApplyPopupSize(sizeTarget, kSizes[cmd - 200][0], kSizes[cmd - 200][1]);
    }
}

// ───────────────────────── 마우스 ─────────────────────────
enum Mode { M_NONE, M_PRESS, M_DRAG, M_MOVEPANEL, M_RESIZE };
static Mode g_mode = M_NONE;
static POINT g_down, g_grab, g_mouse;
static Item* g_modeItem = nullptr;
static Bitmap* g_ghost = nullptr;
static RECT g_ghostRect{};

static void InvalidateItem(Item* it) {
    RECT r; if (it && ItemRect(it, r)) { InflateRect(&r, 4, 4); InvalidateRect(g_hwnd, &r, FALSE); }
}
static void BeginDrag(Item* it) {
    RECT R; if (!ItemRect(it, R)) return;
    g_dragging = true; g_dragItem = it;
    g_grab = {g_down.x - R.left, g_down.y - R.top};
    delete g_ghost;
    g_ghost = new Bitmap(R.right - R.left, R.bottom - R.top, PixelFormat32bppPARGB);
    {
        Graphics g(g_ghost); SetupG(g);
        g.TranslateTransform((REAL)-R.left, (REAL)-R.top);
        RECT saved = g_paintRc; g_paintRc = R;
        DrawItem(g, it, R, false);
        g_paintRc = saved;
    }
    g_ghostRect = R;
    InvalidateRect(g_hwnd, &R, FALSE);
}
static void EndDrag(POINT pt) {
    Item* it = g_dragItem;
    g_dragging = false; g_dragItem = nullptr;
    Item* tgt = g_dropTarget; g_dropTarget = nullptr;
    delete g_ghost; g_ghost = nullptr;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    if (!it) return;
    if (tgt && tgt != it && tgt->FolderLike()) { MoveItemTo(it, tgt->Dir(), -1, -1); return; }
    Hit h = HitTest(pt, it);
    if (h.layer >= 0 && g_stack[h.layer] == it) return;
    Container* dst = ContOfLayer(h.layer);
    POINT tl = {pt.x - g_grab.x + CW / 2, pt.y - g_grab.y + CH / 2};
    int tm = h.layer < 0 ? MonAt(tl) : 0;
    int cx, cy; CellAt(h.layer, tm, tl, cx, cy);
    if (dst == it->parent) {
        int ox, oy;
        if (NearestFree(dst, it, it->Span(), cx, cy, ox, oy, tm)) { it->gx = ox; it->gy = oy; it->mon = h.layer < 0 ? tm : it->mon; SyncEntry(it); ScheduleSave(); }
        for (int i = 0; i < (int)g_stack.size(); i++) ComputeSceneBlur(g_stack[i], i);
    } else {
        int ox = cx, oy = cy;
        NearestFree(dst, nullptr, it->Span(), cx, cy, ox, oy, tm);
        MoveItemTo(it, dst->dir, ox, oy, tm);
    }
}

// ───────────────────────── 창 프로시저 ─────────────────────────
static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED && WM_TASKBARCREATED) { AttachToDesktop(); return 0; }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        RECT rc = ps.rcPaint;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            HDC mdc = CreateCompatibleDC(dc);
            BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
            void* bits; HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HBITMAP ob = (HBITMAP)SelectObject(mdc, hb);
            BitBlt(mdc, 0, 0, w, h, g_bgDC, rc.left, rc.top, SRCCOPY);
            {
                Graphics g(mdc); SetupG(g, true);
                g.TranslateTransform((REAL)-rc.left, (REAL)-rc.top);
                g_paintRc = rc;
                DrawSceneBelow(g, (int)g_stack.size());
                if (g_dragging && g_ghost) {
                    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, 0.8f, 0}, {0, 0, 0, 0, 1}}};
                    ImageAttributes ia; ia.SetColorMatrix(&cm);
                    g.DrawImage(g_ghost, RF(g_ghostRect), 0, 0, (REAL)g_ghost->GetWidth(), (REAL)g_ghost->GetHeight(), UnitPixel, &ia);
                }
                DrawAnim(g);
            }
            BitBlt(dc, rc.left, rc.top, w, h, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, ob); DeleteObject(hb); DeleteDC(mdc);
        }
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS* p = (WINDOWPOS*)lp;
        if (!(p->flags & SWP_NOZORDER)) p->hwndInsertAfter = HWND_BOTTOM;
        return 0;
    }
    case WM_TIMER:
        if (wp == TM_ANIM) {
            InvalidateRect(hw, nullptr, FALSE);
            if (GetTickCount() - g_anim.t0 >= ANIM_MS) {
                KillTimer(hw, TM_ANIM);
                bool opening = g_anim.opening; Item* it = g_anim.it;
                g_anim.on = false; delete g_anim.snap; g_anim.snap = nullptr;
                if (opening) g_stack.push_back(it);
                InvalidateRect(hw, nullptr, FALSE);
            }
        } else if (wp == TM_SAVE) {
            KillTimer(hw, TM_SAVE);
            SaveTree(&g_root); WriteLayoutFile();
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        } else if (wp == TM_RESCAN) {
            if (g_mode != M_NONE || g_anim.on || g_edit) return 0;   // 작업 중이면 나중에
            KillTimer(hw, TM_RESCAN);
            RebuildAll();
        }
        return 0;
    case WM_APP_RESCAN: SetTimer(hw, TM_RESCAN, 400, nullptr); return 0;
    case WM_APP + 3: if (g_edit) EndRename(true); return 0;
    case WM_APP_FIRSTRUN:
        ChooseMode(AskMode());
        return 0;
    case WM_LBUTTONDOWN: {
        if (g_anim.on) return 0;
        if (g_edit) { EndRename(true); return 0; }
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_down = g_mouse = pt;
        Hit h = HitTest(pt);
        if (!g_stack.empty() && h.layer < (int)g_stack.size() - 1) { ClosePopups(h.layer + 1, true); return 0; }  // 바깥 클릭 → 닫기
        SetCapture(hw);
        if (h.area == A_GRIP) { g_mode = M_RESIZE; g_modeItem = g_stack[h.layer]; }
        else if (h.area == A_HEADER || h.area == A_BODY) {
            g_mode = M_MOVEPANEL; g_modeItem = g_stack[h.layer];
            RECT P = PanelRect(g_modeItem); g_grab = {pt.x - P.left, pt.y - P.top};
            g_sel = nullptr;
        } else if (h.item) { g_mode = M_PRESS; g_modeItem = h.item; Item* o = g_sel; g_sel = h.item; InvalidateItem(o); InvalidateItem(h.item); }
        else { g_mode = M_NONE; Item* o = g_sel; g_sel = nullptr; InvalidateItem(o); ReleaseCapture(); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_mouse = pt;
        if (g_mode == M_PRESS) {
            if (std::abs(pt.x - g_down.x) + std::abs(pt.y - g_down.y) > 6 && !g_modeItem->pin) { g_mode = M_DRAG; BeginDrag(g_modeItem); }
        }
        if (g_mode == M_DRAG && g_dragging) {
            RECT old = g_ghostRect;
            int w = old.right - old.left, h = old.bottom - old.top;
            g_ghostRect = {pt.x - g_grab.x, pt.y - g_grab.y, pt.x - g_grab.x + w, pt.y - g_grab.y + h};
            Hit hh = HitTest(pt, g_dragItem);
            Item* nt = (hh.item && hh.item->FolderLike()) ? hh.item : nullptr;
            if (nt != g_dropTarget) { InvalidateItem(g_dropTarget); g_dropTarget = nt; InvalidateItem(nt); }
            RECT u; UnionRect(&u, &old, &g_ghostRect); InflateRect(&u, 4, 4);
            InvalidateRect(hw, &u, FALSE);
        } else if (g_mode == M_MOVEPANEL) {
            RECT old = PanelRect(g_modeItem);
            g_modeItem->px = pt.x - g_grab.x; g_modeItem->py = pt.y - g_grab.y;
            RECT nw = PanelRect(g_modeItem);
            RECT u; UnionRect(&u, &old, &nw); InflateRect(&u, (int)(30 * S), (int)(30 * S));
            InvalidateRect(hw, &u, FALSE);
        } else if (g_mode == M_RESIZE) {
            RECT P = PanelRect(g_modeItem);
            int c = std::max(1, (int)std::lround((pt.x - P.left - 2 * PAD) / (double)CW));
            int r = std::max(1, (int)std::lround((pt.y - P.top - HEAD - PAD) / (double)CH));
            const RECT& wk = g_mons[MonAt({(P.left + P.right) / 2, P.top + HEAD})].work;
            c = std::min(c, (int)((wk.right - P.left - 2 * PAD) / CW));
            r = std::min(r, (int)((wk.bottom - P.top - HEAD - PAD) / CH));
            c = std::max(c, 1); r = std::max(r, 1);
            if (c != g_modeItem->pc || r != g_modeItem->pr) {
                RECT old = P;
                g_modeItem->pc = c; g_modeItem->pr = r;
                RECT nw = PanelRect(g_modeItem);
                RECT u; UnionRect(&u, &old, &nw); InflateRect(&u, (int)(30 * S), (int)(30 * S));
                InvalidateRect(hw, &u, FALSE);
            }
        } else if (g_mode == M_NONE && !g_anim.on) {
            Hit h = HitTest(pt);
            if (h.item != g_hover) { InvalidateItem(g_hover); g_hover = h.item; InvalidateItem(g_hover); }
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hw, 0}; TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: InvalidateItem(g_hover); g_hover = nullptr; return 0;
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        Mode m = g_mode; g_mode = M_NONE;
        ReleaseCapture();
        if (m == M_PRESS && g_modeItem && g_modeItem->pop) OpenPopup(g_modeItem);
        else if (m == M_DRAG) EndDrag(pt);
        else if (m == M_MOVEPANEL || m == M_RESIZE) {
            Item* it = g_modeItem;
            RECT P = PanelRect(it); it->px = P.left; it->py = P.top;
            if (m == M_RESIZE) ApplyPopupSize(it, it->pc, it->pr);
            else { int d = 0; for (int i = 0; i < (int)g_stack.size(); i++) if (g_stack[i] == it) d = i; ComputeSceneBlur(it, d); SyncEntry(it); ScheduleSave(); }
            InvalidateRect(hw, nullptr, FALSE);
        }
        g_modeItem = nullptr;
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        Hit h = HitTest(pt);
        if (h.item && !h.item->pop) Launch(h.item);
        return 0;
    }
    case WM_RBUTTONUP: {
        if (g_anim.on || g_mode != M_NONE) return 0;
        if (g_edit) EndRename(true);
        ShowMenu({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hw, &pt);
        Hit h = HitTest(pt);
        if (h.layer >= 0) {
            Item* p = g_stack[h.layer];
            int maxB = 0; for (auto& x : Child(p)->items) maxB = std::max(maxB, x->gy + x->Span());
            int mx = std::max(0, maxB - p->pr);
            int ns = std::max(0, std::min(mx, p->scroll - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA));
            if (ns != p->scroll) { p->scroll = ns; RECT P = PanelRect(p); InvalidateRect(hw, &P, FALSE); }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !g_stack.empty()) ClosePopups((int)g_stack.size() - 1, true);
        else if (wp == VK_DELETE && g_sel && !g_sel->pin) DeleteItem(g_sel);
        else if (wp == VK_F2 && g_sel) StartRename(g_sel);
        else if (wp == VK_F5) RebuildAll();
        else if (wp == VK_RETURN && g_sel) { if (g_sel->pop) OpenPopup(g_sel); else Launch(g_sel); }
        return 0;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        POINT pt; DragQueryPoint(hd, &pt);
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        for (UINT i = 0; i < n; i++) {
            wchar_t b[MAX_PATH * 2]; DragQueryFileW(hd, i, b, MAX_PATH * 2); files.push_back(b);
        }
        DragFinish(hd);
        Hit h = HitTest(pt);
        std::wstring dir; int cx = -1, cy = -1;
        if (h.item && h.item->FolderLike()) dir = h.item->Dir();
        else { dir = ContOfLayer(h.layer)->dir; CellAt(h.layer, h.mon, pt, cx, cy); }
        if (!files.empty()) {
            std::wstring first = dir + L"\\" + FileName(files[0]);
            int ox = cx, oy = cy;
            if (cx >= 0 && !Exists(first) && NearestFree(ContOfLayer(h.layer), nullptr, 1, cx, cy, ox, oy, h.mon)) {
                Entry& e = g_lay[Key(first)]; e.gx = ox; e.gy = oy; e.m = h.mon;
            }
        }
        ShellOp(FO_COPY, files, dir, FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION | FOF_ALLOWUNDO);
        RebuildAll();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETDESKWALLPAPER && g_set.wallMode == 0) { LoadBackground(); InvalidateRect(hw, nullptr, FALSE); }
        if (wp == SPI_SETWORKAREA) { UpdateMetrics(); RebuildAll(); }
        return 0;
    case WM_DPICHANGED: return 0;   // 창 크기는 직접 관리
    case WM_DISPLAYCHANGE:
        UpdateMetrics();
        SetWindowPos(hw, HWND_BOTTOM, g_virt.x, g_virt.y, g_screen.right, g_screen.bottom, SWP_NOACTIVATE);
        LoadBackground(); RebuildAll();
        return 0;
    case WM_ENDSESSION: if (wp) { SaveTree(&g_root); WriteLayoutFile(); } return 0;
    case WM_CLOSE: return 0;   // Alt+F4로 실수로 닫히지 않게 (메뉴에서 종료)
    case WM_DESTROY:
        SaveTree(&g_root); WriteLayoutFile();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

// ───────────────────────── 폴더 감시 ─────────────────────────
static DWORD WINAPI WatchThread(LPVOID p) {
    LONG gen = (LONG)(LONG_PTR)p;
    std::vector<std::wstring> dirs = {g_root.dir};
    for (auto& d : g_root.extraDirs) dirs.push_back(d);
    std::vector<HANDLE> hs;
    for (auto& d : dirs) {
        HANDLE h = FindFirstChangeNotificationW(d.c_str(), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
        if (h != INVALID_HANDLE_VALUE) hs.push_back(h);
    }
    while (!hs.empty() && g_watchGen == gen) {
        DWORD r = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE, 1000);
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + hs.size()) {
            if (g_watchGen == gen) PostMessageW(g_hwnd, WM_APP_RESCAN, 0, 0);
            FindNextChangeNotification(hs[r - WAIT_OBJECT_0]);
        }
    }
    for (HANDLE h : hs) FindCloseChangeNotification(h);
    return 0;
}

// ───────────────────────── 시작 ─────────────────────────
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"PopupDesktop_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi; ULONG_PTR gtok;
    GdiplusStartup(&gtok, &gsi, nullptr);

    wchar_t ep[MAX_PATH]; GetModuleFileNameW(nullptr, ep, MAX_PATH); g_exePath = ep;
    g_baseDir = KnownFolder(FOLDERID_Documents) + L"\\팝업 바탕화면";
    g_backupData = g_baseDir + L"\\자료";
    g_desktopDir = KnownFolder(FOLDERID_Desktop);
    g_publicDesktop = KnownFolder(FOLDERID_PublicDesktop);
    g_backupDir = g_baseDir + L"\\layout_백업";
    g_layoutPath = g_baseDir + L"\\layout.json";
    CreateDirectoryW(g_baseDir.c_str(), nullptr);
    CreateDirectoryW(g_backupData.c_str(), nullptr);
    CreateDirectoryW(g_backupDir.c_str(), nullptr);
    LoadLayout();
    BackupLayoutOnStart();

    HDC sdc = GetDC(nullptr); S = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f; ReleaseDC(nullptr, sdc);
    CW = (int)(86 * S); CH = (int)(98 * S); ICON1 = (int)(48 * S); LINEH = (int)(16 * S);
    MARGIN = (int)(10 * S); PAD = (int)(14 * S); HEAD = (int)(42 * S); RAD = (int)(20 * S);
    ICON2 = std::min(2 * CW - (int)(18 * S), 2 * CH - 2 * LINEH - (int)(20 * S));
    g_ff = new FontFamily(L"Malgun Gothic");
    if (g_ff->GetLastStatus() != Ok) { delete g_ff; g_ff = FontFamily::GenericSansSerif()->Clone(); }
    g_font = new Font(g_ff, 12.5f * S, FontStyleRegular, UnitPixel);
    g_fontBold = new Font(g_ff, 15.0f * S, FontStyleBold, UnitPixel);
    g_uiFont = CreateFontW(-(int)(13 * S), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    { INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic); }
    g_editFont = CreateFontW(-(int)(12.5f * S), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");

    UpdateMetrics();
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc = {};
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = L"PopupDesktopWnd";
    wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(1));
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES, wc.lpszClassName, L"팝업 바탕화면", WS_POPUP,
                             g_virt.x, g_virt.y, g_screen.right - g_screen.left, g_screen.bottom - g_screen.top,
                             nullptr, nullptr, hi, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, 0x0049, MSGFLT_ALLOW, nullptr);
    LoadBackground();
    g_root.extraDirs.clear();
    if (g_set.mode == 0) {
        g_dataDir = g_desktopDir;
        if (!g_publicDesktop.empty() && IsDirPath(g_publicDesktop)) g_root.extraDirs.push_back(g_publicDesktop);
    } else g_dataDir = g_backupData;
    g_root.dir = g_dataDir;
    LoadContainer(&g_root);
    AttachToDesktop();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    AttachToDesktop();
    CreateThread(nullptr, 0, WatchThread, (LPVOID)(LONG_PTR)g_watchGen, 0, nullptr);
    if (g_set.mode < 0) PostMessageW(g_hwnd, WM_APP_FIRSTRUN, 0, 0);
    ScheduleSave();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    FreeBg();
    delete g_font; delete g_fontBold; delete g_ff;
    GdiplusShutdown(gtok);
    CoUninitialize();
    ReleaseMutex(mtx);
    return 0;
}
