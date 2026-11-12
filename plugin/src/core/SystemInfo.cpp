// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
// Built WITHOUT the precompiled header (CMake: SKIP_PRECOMPILE_HEADERS): the real DXGI headers and CommonLib's own D3D types must not meet in one file.
#include "SystemInfo.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dxgi.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace
{
    std::string CpuName()
    {
        char name[256] = {};
        DWORD size = sizeof(name);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) return "unknown CPU";
        std::string s = name;
        s.erase(0, s.find_first_not_of(' '));
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    std::pair<int, int> CpuCores()  // physical cores, logical processors
    {
        DWORD bytes = 0;
        GetLogicalProcessorInformation(nullptr, &bytes);
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
        int physical = 0;
        if (bytes && GetLogicalProcessorInformation(info.data(), &bytes)) {
            for (DWORD i = 0; i < bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ++i) {
                if (info[i].Relationship == RelationProcessorCore) ++physical;
            }
        }
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        return { physical, static_cast<int>(si.dwNumberOfProcessors) };
    }

    std::string Gpu()
    {
        // the adapter with the most video memory (the one the game uses on nearly every machine; a laptop's integrated GPU has little dedicated memory)
        using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (!dxgi) return "unknown GPU";
        std::string best = "unknown GPU";
        if (auto create = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) {
            IDXGIFactory1* factory = nullptr;
            if (SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
                SIZE_T bestMemory = 0;
                IDXGIAdapter1* adapter = nullptr;
                for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                    DXGI_ADAPTER_DESC1 desc{};
                    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (best == "unknown GPU" || desc.DedicatedVideoMemory > bestMemory)) {
                        char name[128] = {};
                        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                        best = std::format("{} ({} MB video memory)", name, desc.DedicatedVideoMemory / (1024 * 1024));
                        bestMemory = desc.DedicatedVideoMemory;
                    }
                    adapter->Release();
                }
                factory->Release();
            }
        }
        FreeLibrary(dxgi);
        return best;
    }

    std::string WindowsVersion()
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")); fn && fn(&v) == 0) {
            const char* name = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "Windows 11" : v.dwMajorVersion == 10 ? "Windows 10" : "Windows";
            return std::format("{} (build {})", name, v.dwBuildNumber);
        }
        return "Windows (unknown version)";
    }

    std::string GraphicsMods()  // ENB / ReShade: they change how the water looks and what it costs
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const auto dir = std::filesystem::path(exe).parent_path();
        std::error_code ec;
        const bool d3d11 = std::filesystem::exists(dir / "d3d11.dll", ec);
        const bool dxgi = std::filesystem::exists(dir / "dxgi.dll", ec);
        const bool enb = std::filesystem::exists(dir / "enblocal.ini", ec) || std::filesystem::exists(dir / "enbseries.ini", ec);
        const bool reshade = std::filesystem::exists(dir / "ReShade.ini", ec) || std::filesystem::exists(dir / "reshade-shaders", ec);
        std::string out;
        if (enb) out += "ENB";
        if (reshade) out += out.empty() ? "ReShade" : " + ReShade";
        if (out.empty() && (d3d11 || dxgi)) out = std::format("a {} proxy", d3d11 ? "d3d11.dll" : "dxgi.dll");
        return out.empty() ? "no ENB / ReShade" : out;
    }
}

namespace DS
{
    std::string SystemSummary()
    {
        const auto [physical, logical] = CpuCores();
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);
        return std::format("System: {} ({} cores, {} threads), {} GB RAM, GPU {}, {}, desktop {}x{}, {}", CpuName(), physical, logical, (mem.ullTotalPhys + (512ull << 20)) >> 30, Gpu(),
            WindowsVersion(), GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), GraphicsMods());
    }
}
