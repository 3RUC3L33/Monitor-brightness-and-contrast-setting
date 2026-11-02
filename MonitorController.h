#pragma once
#include <windows.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "user32.lib")

// Talks to monitors over DDC/CI. All values exposed here are percentages (0-100),
// regardless of the real min/max range the monitor reports.
class MonitorController {
public:
    // Identifies one physical monitor: the display device it is attached to
    // (e.g. \\.\DISPLAY2) plus its index on that device (almost always 0).
    struct Target {
        std::wstring device;
        int index = 0;
        std::wstring description;   // physical monitor description from Windows
        bool primary = false;
    };

private:
    struct Range {
        DWORD minV = 0;
        DWORD maxV = 100;
        bool valid = false;
    };

    enum Feature { kFeatBrightness, kFeatContrast };

    // MCCS VCP codes
    static const BYTE kVcpBrightness = 0x10;
    static const BYTE kVcpContrast = 0x12;

    // The UI thread (writes) and the poll thread (reads) both go through DDC/CI,
    // which does not like concurrent access, so every public call takes this lock.
    static std::mutex& Lock() {
        static std::mutex m;
        return m;
    }

    static std::map<std::wstring, Range>& Ranges() {
        static std::map<std::wstring, Range> ranges;
        return ranges;
    }

    static std::wstring RangeKey(const Target& t, Feature f) {
        return t.device + L"#" + std::to_wstring(t.index) + (f == kFeatBrightness ? L"#b" : L"#c");
    }

    struct FindCtx {
        const std::wstring* device;
        HMONITOR result;
    };

    static BOOL CALLBACK FindProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
        FindCtx* ctx = reinterpret_cast<FindCtx*>(lp);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi) && *ctx->device == mi.szDevice) {
            ctx->result = hMon;
            return FALSE;
        }
        return TRUE;
    }

    static BOOL CALLBACK ListProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
        std::vector<Target>* out = reinterpret_cast<std::vector<Target>*>(lp);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) || count == 0) return TRUE;
        std::vector<PHYSICAL_MONITOR> phys(count);
        if (!GetPhysicalMonitorsFromHMONITOR(hMon, count, phys.data())) return TRUE;

        for (DWORD i = 0; i < count; i++) {
            Target t;
            t.device = mi.szDevice;
            t.index = static_cast<int>(i);
            t.description = phys[i].szPhysicalMonitorDescription;
            t.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            out->push_back(t);
        }
        DestroyPhysicalMonitors(count, phys.data());
        return TRUE;
    }

    // Opens the physical monitors of the display the target lives on.
    // Returns false if that display is gone or the index is out of range.
    static bool Open(const Target& t, std::vector<PHYSICAL_MONITOR>& monitors) {
        FindCtx ctx{ &t.device, NULL };
        EnumDisplayMonitors(NULL, NULL, FindProc, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.result) return false;

        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(ctx.result, &count) || count == 0) return false;
        monitors.resize(count);
        if (!GetPhysicalMonitorsFromHMONITOR(ctx.result, count, monitors.data())) {
            monitors.clear();
            return false;
        }
        if (t.index < 0 || t.index >= static_cast<int>(count)) {
            Release(monitors);
            return false;
        }
        return true;
    }

    static void Release(std::vector<PHYSICAL_MONITOR>& monitors) {
        if (!monitors.empty()) {
            DestroyPhysicalMonitors(static_cast<DWORD>(monitors.size()), monitors.data());
            monitors.clear();
        }
    }

    // Raw VCP access is tried first: it reports the true current/max value and works on
    // monitors whose capability string does not advertise brightness/contrast, which makes
    // the high-level GetMonitorBrightness/GetMonitorContrast calls fail. The high-level
    // calls are kept as a fallback.
    static bool RawGet(HANDLE h, Feature f, DWORD& minV, DWORD& curV, DWORD& maxV) {
        DWORD vcpCur = 0, vcpMax = 0;
        BYTE code = f == kFeatBrightness ? kVcpBrightness : kVcpContrast;
        if (GetVCPFeatureAndVCPFeatureReply(h, code, NULL, &vcpCur, &vcpMax) && vcpMax > 0) {
            minV = 0;
            curV = vcpCur;
            maxV = vcpMax;
            return true;
        }
        return f == kFeatBrightness
            ? GetMonitorBrightness(h, &minV, &curV, &maxV) != FALSE
            : GetMonitorContrast(h, &minV, &curV, &maxV) != FALSE;
    }

    static bool RawSet(HANDLE h, Feature f, DWORD value) {
        BYTE code = f == kFeatBrightness ? kVcpBrightness : kVcpContrast;
        if (SetVCPFeature(h, code, value)) return true;
        return f == kFeatBrightness
            ? SetMonitorBrightness(h, value) != FALSE
            : SetMonitorContrast(h, value) != FALSE;
    }

    static bool ReadPercentOnce(const Target& t, Feature f, int& percent) {
        std::lock_guard<std::mutex> guard(Lock());
        std::vector<PHYSICAL_MONITOR> monitors;
        if (!Open(t, monitors)) return false;

        DWORD minV = 0, curV = 0, maxV = 0;
        bool ok = RawGet(monitors[t.index].hPhysicalMonitor, f, minV, curV, maxV);
        Release(monitors);
        if (!ok || maxV <= minV) return false;

        Ranges()[RangeKey(t, f)] = Range{ minV, maxV, true };
        if (curV < minV) curV = minV;
        if (curV > maxV) curV = maxV;
        DWORD span = maxV - minV;
        percent = static_cast<int>(((curV - minV) * 100 + span / 2) / span);
        return true;
    }

    // The first DDC/CI call after launch (or after wake) often fails, so try a few times.
    // The lock is taken per attempt so the UI thread is never blocked during the pauses.
    static bool ReadPercent(const Target& t, Feature f, int& percent) {
        for (int attempt = 0; attempt < 4; attempt++) {
            if (ReadPercentOnce(t, f, percent)) return true;
            Sleep(150);
        }
        return false;
    }

    static bool WritePercent(const Target& t, Feature f, int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        std::lock_guard<std::mutex> guard(Lock());
        std::vector<PHYSICAL_MONITOR> monitors;
        if (!Open(t, monitors)) return false;
        HANDLE h = monitors[t.index].hPhysicalMonitor;

        Range& range = Ranges()[RangeKey(t, f)];
        if (!range.valid) {
            DWORD minV = 0, curV = 0, maxV = 0;
            if (RawGet(h, f, minV, curV, maxV) && maxV > minV) {
                range = Range{ minV, maxV, true };
            }
        }
        DWORD minV = range.valid ? range.minV : 0;
        DWORD maxV = range.valid ? range.maxV : 100;
        DWORD raw = minV + static_cast<DWORD>((static_cast<long long>(maxV - minV) * percent + 50) / 100);

        bool ok = RawSet(h, f, raw);
        Release(monitors);
        return ok;
    }

public:
    // All physical monitors currently attached, sorted by display device name.
    static std::vector<Target> Enumerate() {
        std::lock_guard<std::mutex> guard(Lock());
        std::vector<Target> result;
        EnumDisplayMonitors(NULL, NULL, ListProc, reinterpret_cast<LPARAM>(&result));
        std::sort(result.begin(), result.end(), [](const Target& a, const Target& b) {
            int c = a.device.compare(b.device);
            return c != 0 ? c < 0 : a.index < b.index;
        });
        return result;
    }

    // Return true and fill 'percent' with the monitor's real current value,
    // or return false if the monitor could not be read (no DDC/CI, busy, unplugged ...).
    static bool GetBrightness(const Target& t, int& percent) { return ReadPercent(t, kFeatBrightness, percent); }
    static bool GetContrast(const Target& t, int& percent)   { return ReadPercent(t, kFeatContrast, percent); }

    static bool SetBrightness(const Target& t, int percent)  { return WritePercent(t, kFeatBrightness, percent); }
    static bool SetContrast(const Target& t, int percent)    { return WritePercent(t, kFeatContrast, percent); }
};
