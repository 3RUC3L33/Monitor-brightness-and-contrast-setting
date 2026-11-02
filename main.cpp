#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "MonitorController.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAYICON 1001
#define ID_MENU_EXIT 3001
#define ID_MENU_RUN_AT_STARTUP 3002
#define ID_MENU_ABOUT 3003
#define ID_APP_ICON 1        // white glyph: for a dark taskbar
#define ID_APP_ICON_DARK 2   // black glyph: for a light taskbar
// Posted by the poll thread. wParam = (brightness + 1) | ((contrast + 1) << 16), 0 = could not read.
// lParam = the selection serial the values were read for.
#define WM_MONITOR_POLLED (WM_APP + 2)

enum Section { kBrightness, kContrast, kSectionCount };
enum HitKind { HitNone, HitSlider, HitPreset, HitStep, HitDropdown, HitDropItem };

struct Hit {
    HitKind kind;
    Section section;
    int index;
};

struct Theme {
    COLORREF background;
    COLORREF text;
    COLORREF subText;
    COLORREF tile;
    COLORREF tileHover;
    COLORREF tileBorder;
    COLORREF track;
    COLORREF accent;
    COLORREF thumb;
};

const Theme kDarkTheme = {
    RGB(32, 32, 32), RGB(255, 255, 255), RGB(180, 180, 180),
    RGB(44, 44, 44), RGB(54, 54, 54), RGB(60, 60, 60),
    RGB(78, 78, 78), RGB(96, 170, 240), RGB(255, 255, 255)
};

const Theme kLightTheme = {
    RGB(243, 243, 243), RGB(0, 0, 0), RGB(96, 96, 96),
    RGB(251, 251, 251), RGB(236, 236, 236), RGB(229, 229, 229),
    RGB(160, 160, 160), RGB(0, 103, 192), RGB(255, 255, 255)
};

const int kPanelWidth = 440;
const int kPanelHeight = 284;
const int kPad = 20;
const int kSectionTop[kSectionCount] = { 16, 120 };
const int kTrackLeft = 52;
const int kTrackRight = 420;
const int kTileWidth = 28;
const int kTileStride = 32;
const int kStepWidth = 32;                 // the "-" and "+" buttons after the presets
const int kStepLeft[2] = { 352, 388 };     // index 0 = minus, 1 = plus
const int kTileHeight = 28;
const int kTileTop = 64;
const int kSliderTop = 28;
const int kPresetCount = 10;
const int kDropTop = 232;                  // monitor drop-down box at the bottom of the panel
const int kDropHeight = 32;
const int kItemHeight = 30;
const int kMaxDropItems = 6;
const DWORD kPollIntervalMs = 1000;        // how often to re-read the monitor while the panel is open
const DWORD kIgnorePollAfterWriteMs = 1500;  // let the monitor settle after we change a value
const DWORD kSettleAfterRestoreMs = 1000;  // wait before reading back after restoring saved settings

struct MonitorEntry {
    MonitorController::Target target;
    std::wstring key;     // config.ini section name, stable across restarts
    std::wstring label;   // text shown in the drop-down
};

struct RestoreJob {
    MonitorController::Target target;
    int brightness = -1;
    int contrast = -1;
};

HWND g_hWnd = NULL;
NOTIFYICONDATAW g_nid = { 0 };
HFONT g_titleFont = NULL;
HFONT g_bodyFont = NULL;
HFONT g_tileFont = NULL;
wchar_t g_fontFace[LF_FACESIZE] = L"Segoe UI";
int g_values[kSectionCount] = { 50, 50 };
bool g_known[kSectionCount] = { false, false };   // true once the real monitor value has been read
bool g_isDark = true;
bool g_trayAdded = false;
Theme g_theme = kDarkTheme;
int g_dragSection = -1;
wchar_t g_configPath[MAX_PATH] = L"";
Hit g_hover = { HitNone, kBrightness, 0 };
DWORD g_lastWriteTick[kSectionCount] = { 0, 0 };

std::vector<MonitorEntry> g_monitors;   // UI thread only
int g_selected = -1;                    // index into g_monitors
bool g_dropOpen = false;

// Shared with the poll thread (guarded by g_pollMutex)
std::mutex g_pollMutex;
MonitorController::Target g_pollTarget;
bool g_pollHasTarget = false;
int g_pollSerial = 0;
std::vector<RestoreJob> g_restoreJobs;

std::atomic<bool> g_panelVisible(false);
std::atomic<bool> g_stopPolling(false);
HANDLE g_pollWakeEvent = NULL;   // auto-reset: "do a read now"
HANDLE g_stopEvent = NULL;       // manual-reset: "the app is closing"
std::thread g_pollThread;

// ---------------------------------------------------------------- fonts

int CALLBACK FontFoundProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *reinterpret_cast<bool*>(lp) = true;
    return 0;
}

bool FontInstalled(const wchar_t* face) {
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, _countof(lf.lfFaceName), face);
    bool found = false;
    HDC hdc = GetDC(NULL);
    EnumFontFamiliesExW(hdc, &lf, FontFoundProc, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(NULL, hdc);
    return found;
}

// Prefer Segoe UI Variable (Windows 11), fall back to Segoe UI on older systems.
void PickFontFace() {
    const wchar_t* candidates[] = { L"Segoe UI Variable", L"Segoe UI Variable Text", L"Segoe UI" };
    for (const wchar_t* face : candidates) {
        if (FontInstalled(face)) {
            wcscpy_s(g_fontFace, _countof(g_fontFace), face);
            return;
        }
    }
}

HFONT MakeFont(int height, int weight) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_fontFace);
}

// ---------------------------------------------------------------- theme / icon

// Windows' light/dark setting. SystemUsesLightTheme drives the taskbar/flyouts,
// AppsUseLightTheme is the fallback if that value is missing.
bool IsSystemDarkMode() {
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS) {
        return value == 0;
    }
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS) {
        return value == 0;
    }
    return true;
}

void ApplyTheme(HWND hwnd) {
    g_isDark = IsSystemDarkMode();
    g_theme = g_isDark ? kDarkTheme : kLightTheme;
    BOOL useDarkModeAttr = g_isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkModeAttr, sizeof(useDarkModeAttr));
    InvalidateRect(hwnd, NULL, TRUE);
}

HICON LoadTrayIcon() {
    HINSTANCE inst = GetModuleHandleW(NULL);
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    // dark taskbar -> white glyph, light taskbar -> black glyph
    int id = g_isDark ? ID_APP_ICON : ID_APP_ICON_DARK;
    HICON icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(id), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR | LR_SHARED);
    if (!icon) icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(ID_APP_ICON), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR | LR_SHARED);
    return icon;
}

void UpdateTrayIcon() {
    g_nid.hIcon = LoadTrayIcon();
    if (g_trayAdded) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------------------------------------------------------------- geometry / hit testing

bool HasSelection() {
    return g_selected >= 0 && g_selected < static_cast<int>(g_monitors.size());
}

RECT SliderRect(Section s) {
    int top = kSectionTop[s];
    return RECT{ kTrackLeft - 8, top + kSliderTop, kTrackRight + 8, top + kSliderTop + 24 };
}

RECT PresetRect(Section s, int i) {
    int x = kPad + i * kTileStride;
    int y = kSectionTop[s] + kTileTop;
    return RECT{ x, y, x + kTileWidth, y + kTileHeight };
}

RECT StepRect(Section s, int i) {
    int y = kSectionTop[s] + kTileTop;
    return RECT{ kStepLeft[i], y, kStepLeft[i] + kStepWidth, y + kTileHeight };
}

RECT DropRect() {
    return RECT{ kPad, kDropTop, kPanelWidth - kPad, kDropTop + kDropHeight };
}

int DropItemCount() {
    return (std::min)(static_cast<int>(g_monitors.size()), kMaxDropItems);
}

// The list opens upwards from the drop-down box.
RECT DropItemRect(int i) {
    int top = kDropTop - 4 - DropItemCount() * kItemHeight;
    return RECT{ kPad, top + i * kItemHeight, kPanelWidth - kPad, top + (i + 1) * kItemHeight };
}

int ThumbX(int value) {
    return kTrackLeft + (kTrackRight - kTrackLeft) * value / 100;
}

int ValueFromX(int x) {
    int value = (x - kTrackLeft) * 100 / (kTrackRight - kTrackLeft);
    return value < 0 ? 0 : (value > 100 ? 100 : value);
}

bool SameHit(Hit a, Hit b) {
    return a.kind == b.kind && a.section == b.section && a.index == b.index;
}

Hit HitTest(POINT pt) {
    if (g_dropOpen) {
        for (int i = 0; i < DropItemCount(); i++) {
            RECT rc = DropItemRect(i);
            if (PtInRect(&rc, pt)) return Hit{ HitDropItem, kBrightness, i };
        }
        RECT box = DropRect();
        if (PtInRect(&box, pt)) return Hit{ HitDropdown, kBrightness, 0 };
        return Hit{ HitNone, kBrightness, 0 };
    }
    for (int s = 0; s < kSectionCount; s++) {
        RECT rc = SliderRect((Section)s);
        if (PtInRect(&rc, pt)) return Hit{ HitSlider, (Section)s, 0 };
    }
    for (int s = 0; s < kSectionCount; s++) {
        for (int i = 0; i < kPresetCount; i++) {
            RECT rc = PresetRect((Section)s, i);
            if (PtInRect(&rc, pt)) return Hit{ HitPreset, (Section)s, i };
        }
    }
    for (int s = 0; s < kSectionCount; s++) {
        for (int i = 0; i < 2; i++) {
            RECT rc = StepRect((Section)s, i);
            if (PtInRect(&rc, pt)) return Hit{ HitStep, (Section)s, i };
        }
    }
    RECT box = DropRect();
    if (PtInRect(&box, pt)) return Hit{ HitDropdown, kBrightness, 0 };
    return Hit{ HitNone, kBrightness, 0 };
}

// ---------------------------------------------------------------- config.ini

const wchar_t* SettingKey(Section s) {
    return s == kBrightness ? L"Brightness" : L"Contrast";
}

void InitConfigPath() {
    GetModuleFileNameW(NULL, g_configPath, MAX_PATH);
    wchar_t* slash = wcsrchr(g_configPath, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_configPath), L"config.ini");
}

// Returns the saved value (0-100) for one monitor, or -1 if nothing is saved.
int LoadMonitorSetting(const std::wstring& section, Section s) {
    int value = static_cast<int>(GetPrivateProfileIntW(section.c_str(), SettingKey(s), -1, g_configPath));
    return (value < 0 || value > 100) ? -1 : value;
}

void SaveMonitorSetting(const MonitorEntry& m, Section s, int value) {
    wchar_t text[8];
    swprintf_s(text, _countof(text), L"%d", value);
    WritePrivateProfileStringW(m.key.c_str(), SettingKey(s), text, g_configPath);
    WritePrivateProfileStringW(m.key.c_str(), L"Name", m.label.c_str(), g_configPath);  // only to make the file readable
}

// ---------------------------------------------------------------- monitor list

std::wstring SanitizeKey(const std::wstring& in) {
    std::wstring out;
    for (wchar_t c : in) {
        bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
        out += ok ? c : L'_';
    }
    return out;
}

// A name for the config.ini section that stays the same between runs. It is built from the
// monitor's device interface path (model + connection), not from the \\.\DISPLAYn number,
// which Windows can reassign.
std::wstring MakeMonitorKey(const MonitorController::Target& t) {
    std::wstring id;
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(t.device.c_str(), 0, &dd, 0x00000001 /* EDD_GET_DEVICE_INTERFACE_NAME */)) {
        id = dd.DeviceID;
    }
    size_t guard = id.find(L"#{");
    if (guard != std::wstring::npos) id.resize(guard);
    if (id.compare(0, 4, L"\\\\?\\") == 0) id.erase(0, 4);
    if (id.empty()) id = t.device;
    if (t.index > 0) id += L"_" + std::to_wstring(t.index);
    return L"Monitor_" + SanitizeKey(id);
}

std::wstring MakeLabel(const MonitorController::Target& t) {
    size_t pos = t.device.find(L"DISPLAY");
    std::wstring number = pos != std::wstring::npos ? t.device.substr(pos + 7) : L"?";
    std::wstring label = L"Display " + number;
    if (t.index > 0) label += L"." + std::to_wstring(t.index + 1);
    if (!t.description.empty()) label += L": " + t.description;
    if (t.primary) label += L" (Primary)";
    return label;
}

std::vector<MonitorEntry> BuildMonitorList() {
    std::vector<MonitorEntry> list;
    for (const auto& t : MonitorController::Enumerate()) {
        MonitorEntry e;
        e.target = t;
        e.key = MakeMonitorKey(t);
        e.label = MakeLabel(t);
        list.push_back(e);
    }
    return list;
}

void RequestMonitorRefresh() {
    if (g_pollWakeEvent) SetEvent(g_pollWakeEvent);
}

// Tell the poll thread which monitor is selected now.
void PublishPollTarget() {
    std::lock_guard<std::mutex> guard(g_pollMutex);
    g_pollHasTarget = HasSelection();
    if (g_pollHasTarget) g_pollTarget = g_monitors[g_selected].target;
    g_pollSerial++;
}

// Queue "write the saved brightness/contrast back to this monitor" for the poll thread.
void QueueRestore(const MonitorEntry& m) {
    RestoreJob job;
    job.target = m.target;
    job.brightness = LoadMonitorSetting(m.key, kBrightness);
    job.contrast = LoadMonitorSetting(m.key, kContrast);
    if (job.brightness < 0 && job.contrast < 0) return;
    {
        std::lock_guard<std::mutex> guard(g_pollMutex);
        g_restoreJobs.push_back(job);
    }
    RequestMonitorRefresh();
}

// (Re)builds the drop-down list. Called at startup and whenever the display setup changes.
// Monitors that are new to this run get their saved settings restored.
void RefreshMonitorList() {
    std::wstring shownKey = HasSelection() ? g_monitors[g_selected].key : L"";
    std::wstring wantedKey = shownKey;
    if (wantedKey.empty()) {
        wchar_t buf[256] = L"";
        GetPrivateProfileStringW(L"Settings", L"SelectedMonitor", L"", buf, _countof(buf), g_configPath);
        wantedKey = buf;
    }

    std::vector<MonitorEntry> fresh = BuildMonitorList();
    for (const auto& m : fresh) {
        bool existed = false;
        for (const auto& old : g_monitors) {
            if (old.key == m.key) { existed = true; break; }
        }
        if (!existed) QueueRestore(m);
    }
    g_monitors = fresh;

    int sel = -1;
    for (int i = 0; i < static_cast<int>(g_monitors.size()); i++) {
        if (g_monitors[i].key == wantedKey) { sel = i; break; }
    }
    if (sel < 0) {
        for (int i = 0; i < static_cast<int>(g_monitors.size()); i++) {
            if (g_monitors[i].target.primary) { sel = i; break; }
        }
    }
    if (sel < 0 && !g_monitors.empty()) sel = 0;
    g_selected = sel;

    std::wstring newKey = HasSelection() ? g_monitors[g_selected].key : L"";
    if (newKey != shownKey) {
        for (int s = 0; s < kSectionCount; s++) {
            g_known[s] = false;
            g_lastWriteTick[s] = 0;
        }
        g_dropOpen = false;
    }
    PublishPollTarget();
    RequestMonitorRefresh();
    if (g_hWnd) InvalidateRect(g_hWnd, NULL, FALSE);
}

void SelectMonitor(int index) {
    if (index < 0 || index >= static_cast<int>(g_monitors.size()) || index == g_selected) return;
    g_selected = index;
    WritePrivateProfileStringW(L"Settings", L"SelectedMonitor", g_monitors[index].key.c_str(), g_configPath);
    for (int s = 0; s < kSectionCount; s++) {
        g_known[s] = false;      // shown as "--%" until the real value of this monitor is read
        g_lastWriteTick[s] = 0;
    }
    PublishPollTarget();
    RequestMonitorRefresh();
}

// ---------------------------------------------------------------- values

void WriteToMonitor(Section s, int value) {
    if (!HasSelection()) return;
    const MonitorController::Target& t = g_monitors[g_selected].target;
    if (s == kBrightness) MonitorController::SetBrightness(t, value);
    else MonitorController::SetContrast(t, value);
}

void SetValue(Section s, int value) {
    g_values[s] = value;
    g_known[s] = true;
    g_lastWriteTick[s] = GetTickCount();
    WriteToMonitor(s, value);
    if (HasSelection()) SaveMonitorSetting(g_monitors[g_selected], s, value);
    InvalidateRect(g_hWnd, NULL, FALSE);
}

// Applies a value read from the monitor to the UI, unless the user is busy changing it.
// A changed value (for example from the monitor's own buttons) is saved to config.ini too.
void ApplyPolledValue(Section s, int value) {
    if (value < 0) return;
    if (g_known[s] && value == g_values[s]) return;
    if (g_dragSection == (int)s) return;
    if (GetTickCount() - g_lastWriteTick[s] < kIgnorePollAfterWriteMs) return;
    g_values[s] = value;
    g_known[s] = true;
    if (HasSelection()) SaveMonitorSetting(g_monitors[g_selected], s, value);
    InvalidateRect(g_hWnd, NULL, FALSE);
}

// ---------------------------------------------------------------- poll thread

// Reads one monitor's real brightness/contrast; -1 for a value that could not be read.
void ReadMonitorValues(const MonitorController::Target& target, int& brightness, int& contrast) {
    brightness = -1;
    contrast = -1;
    int v;
    if (MonitorController::GetBrightness(target, v)) brightness = v;
    if (MonitorController::GetContrast(target, v)) contrast = v;
}

// Writes saved settings back to a monitor. DDC/CI is often not ready right at logon, so retry a few times.
void RestoreMonitor(const RestoreJob& job) {
    bool brightnessDone = job.brightness < 0;
    bool contrastDone = job.contrast < 0;
    for (int attempt = 0; attempt < 5 && !g_stopPolling; attempt++) {
        if (!brightnessDone) brightnessDone = MonitorController::SetBrightness(job.target, job.brightness);
        if (!contrastDone) contrastDone = MonitorController::SetContrast(job.target, job.contrast);
        if (brightnessDone && contrastDone) break;
        WaitForSingleObject(g_stopEvent, 500);
    }
}

// Background thread: DDC/CI calls can take 100ms+, so they never run on the UI thread.
// Each pass it (1) restores saved settings to monitors that were queued for it,
// (2) reads the selected monitor and posts the result to the UI, then waits.
// It keeps re-reading every kPollIntervalMs while the panel is visible, and also retries
// while the selected monitor has not been read successfully yet (it may not be ready right
// after launch), up to a limit.
void PollThreadProc() {
    bool haveBrightness = false, haveContrast = false;
    int attempts = 0;
    int lastSerial = -1;
    const int kMaxStartupAttempts = 30;

    while (!g_stopPolling) {
        std::vector<RestoreJob> jobs;
        {
            std::lock_guard<std::mutex> guard(g_pollMutex);
            jobs.swap(g_restoreJobs);
        }
        for (const auto& job : jobs) RestoreMonitor(job);
        if (!jobs.empty()) {
            // monitors can take a moment to apply a new value; don't read it back too early
            WaitForSingleObject(g_stopEvent, kSettleAfterRestoreMs);
            if (g_stopPolling) break;
        }

        MonitorController::Target target;
        bool hasTarget;
        int serial;
        {
            std::lock_guard<std::mutex> guard(g_pollMutex);
            target = g_pollTarget;
            hasTarget = g_pollHasTarget;
            serial = g_pollSerial;
        }
        if (serial != lastSerial) {
            lastSerial = serial;
            haveBrightness = haveContrast = false;
            attempts = 0;
        }

        if (hasTarget) {
            int brightness, contrast;
            ReadMonitorValues(target, brightness, contrast);
            if (brightness >= 0) haveBrightness = true;
            if (contrast >= 0) haveContrast = true;
            if (!haveBrightness || !haveContrast) attempts++;
            if (brightness >= 0 || contrast >= 0) {
                WPARAM packed = (WPARAM)((brightness + 1) & 0xFFFF) | ((WPARAM)((contrast + 1) & 0xFFFF) << 16);
                PostMessageW(g_hWnd, WM_MONITOR_POLLED, packed, (LPARAM)serial);
            }
        }

        bool retrying = hasTarget && (!haveBrightness || !haveContrast) && attempts < kMaxStartupAttempts;
        DWORD wait = (g_panelVisible || retrying) ? kPollIntervalMs : INFINITE;
        HANDLE handles[2] = { g_pollWakeEvent, g_stopEvent };
        WaitForMultipleObjects(2, handles, FALSE, wait);
    }
}

// ---------------------------------------------------------------- drawing

void FillShape(HDC hdc, RECT rc, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    if (radius < 0) Ellipse(hdc, rc.left, rc.top, rc.right, rc.bottom);
    else RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawLabel(HDC hdc, const wchar_t* text, RECT rc, HFONT font, COLORREF color, UINT alignment) {
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text, -1, &rc, alignment | DT_SINGLELINE | DT_VCENTER);
    SelectObject(hdc, oldFont);
}

void DrawSunIcon(HDC hdc, int cx, int cy, COLORREF color) {
    const double kPi = 3.14159265358979;
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    for (int k = 0; k < 8; k++) {
        double angle = k * kPi / 4;
        MoveToEx(hdc, cx + (int)std::lround(7 * std::cos(angle)), cy + (int)std::lround(7 * std::sin(angle)), NULL);
        LineTo(hdc, cx + (int)std::lround(10 * std::cos(angle)), cy + (int)std::lround(10 * std::sin(angle)));
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    FillShape(hdc, RECT{ cx - 4, cy - 4, cx + 4, cy + 4 }, color, color, -1);
}

void DrawContrastIcon(HDC hdc, int cx, int cy, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, cx - 8, cy - 8, cx + 8, cy + 8);
    SelectObject(hdc, brush);
    Pie(hdc, cx - 8, cy - 8, cx + 8, cy + 8, cx, cy - 8, cx, cy + 8);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawSlider(HDC hdc, Section s) {
    int cy = kSectionTop[s] + kSliderTop + 12;
    FillShape(hdc, RECT{ kTrackLeft, cy - 2, kTrackRight, cy + 2 }, g_theme.track, g_theme.track, 2);
    if (!g_known[s]) return;   // value not read from the monitor yet: show the bare track

    int thumbX = ThumbX(g_values[s]);
    if (thumbX > kTrackLeft) {
        FillShape(hdc, RECT{ kTrackLeft, cy - 2, thumbX, cy + 2 }, g_theme.accent, g_theme.accent, 2);
    }
    FillShape(hdc, RECT{ thumbX - 8, cy - 8, thumbX + 8, cy + 8 }, g_theme.thumb, g_theme.accent, -1);
}

void DrawSection(HDC hdc, Section s) {
    int top = kSectionTop[s];
    wchar_t valueText[8];
    wchar_t tileText[8];

    DrawLabel(hdc, s == kBrightness ? L"Brightness" : L"Contrast",
        RECT{ kPad, top, 300, top + 20 }, g_titleFont, g_theme.text, DT_LEFT);

    if (g_known[s]) swprintf_s(valueText, _countof(valueText), L"%d%%", g_values[s]);
    else wcscpy_s(valueText, _countof(valueText), L"--%");
    DrawLabel(hdc, valueText, RECT{ 300, top, kPanelWidth - kPad, top + 20 }, g_bodyFont, g_theme.subText, DT_RIGHT);

    int iconX = kPad + 10;
    int iconY = top + 40;
    if (s == kBrightness) DrawSunIcon(hdc, iconX, iconY, g_theme.text);
    else DrawContrastIcon(hdc, iconX, iconY, g_theme.text);

    DrawSlider(hdc, s);

    for (int i = 0; i < kPresetCount; i++) {
        RECT rc = PresetRect(s, i);
        bool hot = g_hover.kind == HitPreset && g_hover.section == s && g_hover.index == i;
        FillShape(hdc, rc, hot ? g_theme.tileHover : g_theme.tile, g_theme.tileBorder, 6);
        swprintf_s(tileText, _countof(tileText), L"%d", (i + 1) * 10);
        DrawLabel(hdc, tileText, rc, g_tileFont, g_theme.text, DT_CENTER);
    }

    for (int i = 0; i < 2; i++) {
        RECT rc = StepRect(s, i);
        bool hot = g_hover.kind == HitStep && g_hover.section == s && g_hover.index == i;
        FillShape(hdc, rc, hot ? g_theme.tileHover : g_theme.tile, g_theme.tileBorder, 6);
        DrawLabel(hdc, i == 0 ? L"\u2212" : L"+", rc, g_bodyFont, g_theme.text, DT_CENTER);
    }
}

void DrawDropdown(HDC hdc) {
    RECT box = DropRect();
    bool hotBox = g_hover.kind == HitDropdown || g_dropOpen;
    FillShape(hdc, box, hotBox ? g_theme.tileHover : g_theme.tile, g_theme.tileBorder, 6);

    const std::wstring text = HasSelection() ? g_monitors[g_selected].label : std::wstring(L"No monitor found");
    DrawLabel(hdc, text.c_str(), RECT{ box.left + 12, box.top, box.right - 32, box.bottom },
        g_bodyFont, g_theme.text, DT_LEFT | DT_END_ELLIPSIS);

    // chevron
    int cx = box.right - 18;
    int cy = (box.top + box.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 2, g_theme.subText);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    if (g_dropOpen) {
        MoveToEx(hdc, cx - 5, cy + 2, NULL);
        LineTo(hdc, cx, cy - 3);
        LineTo(hdc, cx + 5, cy + 2);
    } else {
        MoveToEx(hdc, cx - 5, cy - 2, NULL);
        LineTo(hdc, cx, cy + 3);
        LineTo(hdc, cx + 5, cy - 2);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    if (!g_dropOpen || DropItemCount() == 0) return;

    RECT first = DropItemRect(0);
    RECT last = DropItemRect(DropItemCount() - 1);
    FillShape(hdc, RECT{ first.left, first.top - 2, first.right, last.bottom + 2 }, g_theme.tile, g_theme.tileBorder, 8);
    for (int i = 0; i < DropItemCount(); i++) {
        RECT rc = DropItemRect(i);
        bool hot = g_hover.kind == HitDropItem && g_hover.index == i;
        if (hot) {
            FillShape(hdc, RECT{ rc.left + 3, rc.top + 1, rc.right - 3, rc.bottom - 1 }, g_theme.tileHover, g_theme.tileHover, 6);
        }
        if (i == g_selected) {
            FillShape(hdc, RECT{ rc.left + 6, rc.top + 8, rc.left + 9, rc.bottom - 8 }, g_theme.accent, g_theme.accent, 2);
        }
        DrawLabel(hdc, g_monitors[i].label.c_str(), RECT{ rc.left + 16, rc.top, rc.right - 10, rc.bottom },
            g_bodyFont, g_theme.text, DT_LEFT | DT_END_ELLIPSIS);
    }
}

void PaintPanel(HDC hdc, RECT rc) {
    HBRUSH background = CreateSolidBrush(g_theme.background);
    FillRect(hdc, &rc, background);
    DeleteObject(background);

    SetBkMode(hdc, TRANSPARENT);
    for (int s = 0; s < kSectionCount; s++) {
        DrawSection(hdc, (Section)s);
    }
    DrawDropdown(hdc);   // last, so the open list draws over the sections
}

// ---------------------------------------------------------------- window / tray

void PositionWindowAboveTray(HWND hwnd) {
    APPBARDATA abd = { sizeof(abd) };
    SHAppBarMessage(ABM_GETTASKBARPOS, &abd);

    int posX = abd.rc.right - kPanelWidth - 12;
    int posY = abd.rc.top - kPanelHeight - 12;

    if (abd.uEdge == ABE_TOP) posY = abd.rc.bottom + 12;
    if (abd.uEdge == ABE_LEFT) posX = abd.rc.right + 12;

    SetWindowPos(hwnd, HWND_TOPMOST, posX, posY, kPanelWidth, kPanelHeight, SWP_SHOWWINDOW);
}

void ShowPanel(HWND hwnd) {
    g_panelVisible = true;
    g_dropOpen = false;
    RequestMonitorRefresh();   // pick up whatever the monitor's own buttons changed
    ApplyTheme(hwnd);
    PositionWindowAboveTray(hwnd);
    SetActiveWindow(hwnd);
    SetForegroundWindow(hwnd);
}

void HidePanel(HWND hwnd) {
    g_panelVisible = false;
    g_dropOpen = false;
    ShowWindow(hwnd, SW_HIDE);
}

const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kStartupValueName = L"MonitorControls";

bool LoadRunAtStartup() {
    return GetPrivateProfileIntW(L"Settings", L"RunAtStartup", 0, g_configPath) != 0;
}

void ApplyRunAtStartup(bool enabled) {
    WritePrivateProfileStringW(L"Settings", L"RunAtStartup", enabled ? L"1" : L"0", g_configPath);
    if (enabled) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        wchar_t command[MAX_PATH + 2];
        swprintf_s(command, _countof(command), L"\"%s\"", exePath);
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kStartupValueName, REG_SZ, command, (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kStartupValueName);
    }
}

void ShowAboutBox(HWND hwnd) {
    MessageBoxW(hwnd,
        L"Bright Controller v1.0\n"
        L"Built with \U0001F90D by 3RUC3L33 & Claude\n"
        L"\u00A9 2026 | All Rights Reserved.",
        L"About", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void ShowTrayPopupMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    if (hMenu) {
        InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_MENU_EXIT, L"Exit App");
        InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_MENU_ABOUT, L"About");
        InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING | (LoadRunAtStartup() ? MF_CHECKED : MF_UNCHECKED), ID_MENU_RUN_AT_STARTUP, L"Run at startup");

        POINT pt;
        GetCursorPos(&pt);

        SetForegroundWindow(hwnd);
        TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hMenu);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        PickFontFace();
        g_titleFont = MakeFont(-15, FW_SEMIBOLD);
        g_bodyFont = MakeFont(-14, FW_NORMAL);
        g_tileFont = MakeFont(-13, FW_NORMAL);

        g_pollWakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);   // auto-reset
        g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);        // manual-reset

        // Find the monitors, pick the one chosen last time, and queue the saved settings of
        // every monitor that has any in config.ini. Nothing is written for monitors with no
        // saved settings; their real values are read instead.
        RefreshMonitorList();
        g_pollThread = std::thread(PollThreadProc);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP bitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBitmap = SelectObject(memDC, bitmap);

        PaintPanel(memDC, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MONITOR_POLLED: {
        int currentSerial;
        {
            std::lock_guard<std::mutex> guard(g_pollMutex);
            currentSerial = g_pollSerial;
        }
        if ((int)lParam != currentSerial) break;   // read from a monitor that is no longer selected
        ApplyPolledValue(kBrightness, (int)(wParam & 0xFFFF) - 1);
        ApplyPolledValue(kContrast, (int)((wParam >> 16) & 0xFFFF) - 1);
        break;
    }
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
            ApplyTheme(hWnd);
            UpdateTrayIcon();
        }
        break;
    case WM_DISPLAYCHANGE:
        RefreshMonitorList();
        break;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) RequestMonitorRefresh();
        return TRUE;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        Hit hit = HitTest(pt);

        if (g_dropOpen) {
            // any click closes the list; clicking an item also selects that monitor
            g_dropOpen = false;
            if (hit.kind == HitDropItem) SelectMonitor(hit.index);
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        }
        if (hit.kind == HitDropdown) {
            if (!g_monitors.empty()) g_dropOpen = true;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        else if (hit.kind == HitSlider) {
            g_dragSection = hit.section;
            SetCapture(hWnd);
            SetValue(hit.section, ValueFromX(pt.x));
        }
        else if (hit.kind == HitPreset) {
            SetValue(hit.section, (hit.index + 1) * 10);
        }
        else if (hit.kind == HitStep) {
            // +/-1%, only once the real monitor value is known
            if (g_known[hit.section]) {
                int value = g_values[hit.section] + (hit.index == 0 ? -1 : 1);
                value = value < 0 ? 0 : (value > 100 ? 100 : value);
                if (value != g_values[hit.section]) SetValue(hit.section, value);
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (g_dragSection != -1) {
            int value = ValueFromX(pt.x);
            if (value != g_values[g_dragSection]) SetValue((Section)g_dragSection, value);
        }

        Hit hit = HitTest(pt);
        if (!SameHit(hit, g_hover)) {
            g_hover = hit;
            InvalidateRect(hWnd, NULL, FALSE);
        }

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE: {
        g_hover = Hit{ HitNone, kBrightness, 0 };
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }
    case WM_LBUTTONUP: {
        if (g_dragSection != -1) {
            g_dragSection = -1;
            ReleaseCapture();
        }
        break;
    }
    case WM_CAPTURECHANGED: {
        g_dragSection = -1;
        break;
    }
    case WM_TRAYICON: {
        if (LOWORD(lParam) == WM_LBUTTONDOWN) {
            if (IsWindowVisible(hWnd)) {
                HidePanel(hWnd);
            } else {
                ShowPanel(hWnd);
            }
        }
        else if (LOWORD(lParam) == WM_RBUTTONDOWN) {
            ShowTrayPopupMenu(hWnd);
        }
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == ID_MENU_RUN_AT_STARTUP) {
            ApplyRunAtStartup(!LoadRunAtStartup());
        }
        else if (LOWORD(wParam) == ID_MENU_ABOUT) {
            ShowAboutBox(hWnd);
        }
        else if (LOWORD(wParam) == ID_MENU_EXIT) {
            DestroyWindow(hWnd);
        }
        break;
    }
    case WM_KILLFOCUS:
    case WM_ACTIVATE: {
        if (LOWORD(wParam) == WA_INACTIVE) {
            HidePanel(hWnd);
        }
        break;
    }
    case WM_DESTROY: {
        g_stopPolling = true;
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (g_pollWakeEvent) SetEvent(g_pollWakeEvent);
        if (g_pollThread.joinable()) g_pollThread.join();
        if (g_pollWakeEvent) CloseHandle(g_pollWakeEvent);
        if (g_stopEvent) CloseHandle(g_stopEvent);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DeleteObject(g_titleFont);
        DeleteObject(g_bodyFont);
        DeleteObject(g_tileFont);
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(ID_APP_ICON));
    wcex.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(ID_APP_ICON));
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.lpszClassName = L"MonitorTrayAppClass";

    InitConfigPath();
    if (LoadRunAtStartup()) ApplyRunAtStartup(true);
    RegisterClassExW(&wcex);

    g_hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wcex.lpszClassName, L"Monitor Settings", WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, kPanelWidth, kPanelHeight, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) return 0;

    DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));

    ApplyTheme(g_hWnd);

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hWnd;
    g_nid.uID = ID_TRAYICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadTrayIcon();
    wcscpy_s(g_nid.szTip, L"Monitor Controls");
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
