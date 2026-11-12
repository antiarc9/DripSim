// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Core.h"
#include <ctime>
#include <tlhelp32.h>
#include <deque>
#include "DripSimMode.h"
#include "LogLimit.h"

#include "Body.h"
#include "Field.h"
#include "Film.h"
#include "Params.h"
#include "Spike.h"

#include <condition_variable>
#include <future>
#include <thread>

namespace
{
    using DripSim::StopReason;

    // the core's calls into the texture module, always as its owner
    namespace Tex
    {
        int Start(int s) { return DS::Spike::TextureStart(s, DS::Spike::kOwnerCore); }
        int Poll(int s) { return DS::Spike::TexturePoll(s, DS::Spike::kOwnerCore); }
        void Stop(int s) { DS::Spike::TextureStop(s, DS::Spike::kOwnerCore); }
        bool Running(int s) { return DS::Spike::TextureRunning(s, DS::Spike::kOwnerCore); }
        bool Busy(int s) { return DS::Spike::TextureBusy(s, DS::Spike::kOwnerCore); }
        void SetTiles(int s, const std::uint32_t* p, const std::uint8_t* t) { DS::Spike::TextureSetTiles(s, DS::Spike::kOwnerCore, p, t); }
        void SetNormalTiles(int s, const std::uint32_t* p, const std::uint8_t* t) { DS::Spike::TextureSetNormalTiles(s, DS::Spike::kOwnerCore, p, t); }
        void SetSpecTiles(int s, const std::uint32_t* p, const std::uint8_t* t) { DS::Spike::TextureSetSpecTiles(s, DS::Spike::kOwnerCore, p, t); }
        void SetSpec(int s, bool has, float power, float scale) { DS::Spike::TextureSetSpec(s, DS::Spike::kOwnerCore, has, power, scale); }
    }

    constexpr auto kRenderInterval = std::chrono::milliseconds(33);
    constexpr double kOptionalSurfaceWait = 2.5;  // seconds the optional surfaces (hands, feet) get to get ready before the modes start without them

    // ------------------------------------------------------------------ a surface, as modes see it
    class FieldView final : public DripSim::IField
    {
    public:
        explicit FieldView(int surface = 0) : m_surface(surface) {}
        std::int32_t Size() const override { return DS::Field::kSize; }
        void At(float x, float y, DripSim::Sample* out) const override
        {
            if (!out) return;
            const auto s = F().At(x, y);
            *out = DripSim::Sample{ s.dx, s.dy, s.steep, s.mmpx, s.up, s.inner ? 1 : 0 };
        }
        bool Inner(std::int32_t x, std::int32_t y) const override { return F().Inner(x, y); }
        std::int32_t TriAt(std::int32_t x, std::int32_t y) const override { return F().TriAt(x, y); }
        float CenterZ(std::int32_t tri) const override { return F().CenterZ(tri); }
        float NormalY(std::int32_t tri) const override { return F().NormalY(tri); }
        float RestSteep(std::int32_t tri) const override { return F().RestSteep(tri); }
        float ZMin() const override { return F().ZMin(); }
        float ZMax() const override { return F().ZMax(); }
        std::int32_t SeamTwin(std::int32_t x, std::int32_t y) const override { return F().SeamTwin(x, y); }

    private:
        const DS::Field::Instance& F() const { return DS::Field::Get(m_surface); }
        int m_surface;
    };
    std::array<FieldView, DS::Surface::Count> g_fieldViews{ FieldView(0), FieldView(1), FieldView(2), FieldView(3) };

    void Notify(const char* text)
    {
        if (DS::LogAllow("notify")) logger::info("Notify: {}", text ? text : "");
        if (auto* console = RE::ConsoleLog::GetSingleton()) console->Print("%s", text ? text : "");
    }

    // ------------------------------------------------------------------ one registered mode
    struct Entry;

    class Host final : public DripSim::IHost
    {
    public:
        explicit Host(Entry* entry) : m_entry(entry) {}
        DripSim::ILayer* Layer() override;
        const DripSim::IField* Field() const override { return &g_fieldViews[DS::Surface::Body]; }
        const DripSim::IWorld* World() const override { return DS::World::Get(); }
        void Notify(const char* text) override { ::Notify(text); }
        void Finish() override;
        std::int32_t SurfaceCount() const override;
        const char* SurfaceName(std::int32_t index) const override;
        DripSim::ILayer* SurfaceLayer(std::int32_t index) override;
        const DripSim::IField* SurfaceField(std::int32_t index) const override;
        bool Raycast(const float from[3], const float dir[3], float maxDist, DripSim::Hit* out) const override;
        bool BodyBounds(float lo[3], float hi[3]) const override { return DS::Field::Get(DS::Surface::Body).Bounds(lo, hi); }

    private:
        Entry* m_entry;
    };

    struct Entry
    {
        explicit Entry(DripSim::IMode* m) : mode(m), name(m->Name()), host(this) {}
        DripSim::IMode* mode;
        std::string name;
        bool active = false;
        bool finishing = false;
        std::array<DS::Film::Layer*, DS::Surface::Count> layers{};  // by surface id
        std::vector<int> surfaces;                                   // the surface ids this run got, body first
        Host host;
    };

    DripSim::ILayer* Host::Layer() { return m_entry->layers[DS::Surface::Body]; }
    void Host::Finish() { m_entry->finishing = true; }
    std::int32_t Host::SurfaceCount() const { return static_cast<std::int32_t>(m_entry->surfaces.size()); }
    const char* Host::SurfaceName(std::int32_t index) const
    {
        return index >= 0 && index < SurfaceCount() ? DS::Surface::Name(m_entry->surfaces[static_cast<std::size_t>(index)]) : "";
    }
    DripSim::ILayer* Host::SurfaceLayer(std::int32_t index)
    {
        return index >= 0 && index < SurfaceCount() ? m_entry->layers[static_cast<std::size_t>(m_entry->surfaces[static_cast<std::size_t>(index)])] : nullptr;
    }
    const DripSim::IField* Host::SurfaceField(std::int32_t index) const
    {
        return index >= 0 && index < SurfaceCount() ? &g_fieldViews[static_cast<std::size_t>(m_entry->surfaces[static_cast<std::size_t>(index)])] : nullptr;
    }

    bool Host::Raycast(const float from[3], const float dir[3], float maxDist, DripSim::Hit* out) const
    {
        DS::Field::RayHit hit;
        if (!out || !DS::Field::Get(DS::Surface::Body).Raycast(from, dir, maxDist, &hit)) return false;
        out->x = hit.x;
        out->y = hit.y;
        out->tx = hit.tx;
        out->ty = hit.ty;
        out->dist = hit.dist;
        out->slant = hit.slant;
        std::memcpy(out->point, hit.point, sizeof out->point);
        std::memcpy(out->normal, hit.normal, sizeof out->normal);
        return true;
    }

    struct Hotkey
    {
        std::uint32_t key;
        bool shift;
        bool ctrl;
        void (*callback)(void*);
        void* context;
    };

    struct Request
    {
        std::string name;
        int variant;
    };

    // Preparation of one surface: capture the mesh -> build the field -> own a texture on a free overlay slot.
    enum class Prep { None, Capturing, Building, Texturing, Ready, Failed };
    struct SurfaceState
    {
        Prep prep = Prep::None;
        std::shared_ptr<const DS::Body::Static> before;
        std::future<bool> build;  // the field is built on a worker thread (rasterising a surface takes 20-100 ms: on the main thread that was a visible hitch)
    };

    std::vector<std::unique_ptr<Entry>> g_modes;
    std::vector<Request> g_pending;
    std::vector<std::string> g_stops;
    std::vector<Hotkey> g_hotkeys;
    std::mutex g_hotkeyMutex;
    std::array<SurfaceState, DS::Surface::Count> g_surf;

    // An optional surface (face, hands, feet) that went away while a mode runs (first person and back, gloves, a rebuilt 3D) is prepared again: wait a moment for the new 3D,
    // capture the mesh, rebuild the field (on the main thread: the modes read it, so it must not change under them), give it a texture, then the layers of the running modes
    // (which kept their film) get the new draw mask and everything is uploaded again. After a few failed tries it stays off.
    struct Reattach
    {
        bool active = false;
        int step = 0;  // 0 waiting, 1 capturing, 2 texturing
        int tries = 0;
        std::chrono::steady_clock::time_point next;
        std::chrono::steady_clock::time_point started;
        std::shared_ptr<const DS::Body::Static> before;
    };
    std::array<Reattach, DS::Surface::Count> g_reattach;
    std::chrono::steady_clock::time_point g_prepStarted;
    std::chrono::steady_clock::time_point g_last;
    std::chrono::steady_clock::time_point g_lastField;
    std::chrono::steady_clock::time_point g_lastRender;
    float g_idleTimer = 0.0f;
    // Back-off: when the body is lost again and again (another mod rebuilds the player's 3D every second or two, seen in a user's log 2026-11-08: 30 restarts in a minute,
    // then a crash), automatic starts pause for a while, so DripSim does not work hardest while the game changes the most. A key press still starts a mode.
    std::deque<std::chrono::steady_clock::time_point> g_lostTimes;
    std::chrono::steady_clock::time_point g_holdOffUntil{};
    // The body could not be prepared (no free overlay slot, most often): automatic starts wait, longer each time (30 s, 60 s, ... 10 min), and the notice shows
    // at most every 5 minutes. A user without a free slot got the notice and ~10 log lines twice a second, because rain tried to start again at every Idle.
    int g_bodyFailStreak = 0;
    std::chrono::steady_clock::time_point g_lastSlotNotice{};
    double g_renderMs = 0;
    double g_updateMs = 0;
    double g_lastJobMs = 0;  // how long the last background render took
    std::uint64_t g_frames = 0;

    // What the framework costs on the game's main thread, logged every ~5 s while a mode runs (`PERF` lines): the game's own frame time (so a slowdown shows up in the log next to
    // its cause), and the per-frame cost of each step of our OnFrame. The background render and the texture upload (render thread) are reported separately.
    struct Perf
    {
        std::chrono::steady_clock::time_point lastFrame{};
        std::chrono::steady_clock::time_point logged{};
        double frameSum = 0, frameMax = 0;  // the game's frame interval (ms)
        double collect = 0, field = 0, modes = 0, total = 0;  // main-thread ms of our steps, summed over the frames
        std::uint64_t frames = 0, skipped = 0;
    };
    Perf g_perf;
    std::uint64_t g_renders = 0;

    // One summary line for every effect run (from the first mode start to the last stop), written whatever PerfLog says: a player's log then always says
    // what DripSim cost on that machine. Frames longer than 250 ms (menus, loading screens, the console) are left out of the averages.
    struct Session
    {
        bool on = false;
        std::chrono::steady_clock::time_point start{};
        std::uint64_t frames = 0, skipped = 0, longFrames = 0, jobs = 0;
        double frameSum = 0, frameMax = 0, ourMs = 0, jobSum = 0, jobMax = 0;
        std::string modes;
    };
    Session g_session;
    double g_baselineMs = 0;  // the game's frame time while no effect runs (a slow moving average)
    std::string g_lastSummary = "(no effect has run yet)";
    std::string g_systemInfo = "System: (not set)";
    std::chrono::steady_clock::time_point g_lastAutoReport{};

    Entry* Find(std::string_view name)
    {
        for (auto& e : g_modes) {
            if (e->name == name) return e.get();
        }
        return nullptr;
    }

    bool AnyEntryActive()
    {
        return std::ranges::any_of(g_modes, [](const auto& e) { return e->active; });
    }

    bool AnySurfaceInUse()
    {
        return std::ranges::any_of(g_surf, [](const SurfaceState& s) { return s.prep != Prep::None; });
    }

    bool Ready(int surface)
    {
        return g_surf[static_cast<std::size_t>(surface)].prep == Prep::Ready;
    }

    // The surfaces some mode wants: the body always, plus what the pending and the running modes ask for.
    std::int32_t NeededMask()
    {
        std::int32_t mask = DripSim::kSurfaceBody;
        for (const auto& request : g_pending) {
            if (auto* e = Find(request.name)) mask |= e->mode->Surfaces();
        }
        for (const auto& e : g_modes) {
            if (e->active) mask |= e->mode->Surfaces();
        }
        return mask;
    }

    // ---------------------------------------------------------------- the film renders in the background
    // Rendering and compositing the film is the heavy part (10-30 ms per frame with water on four surfaces). It runs on a worker thread, pipelined with the game: at the start of a
    // frame we wait for the job of the last frame (usually long done), upload its tiles, run the modes (which draw into the layers), then start the next job and carry on with
    // the rest of the game's frame. The modes and the renderer never touch the layers at the same time. The picture is one frame late.
    struct AsyncRender
    {
        std::mutex m;
        std::condition_variable cv;
        bool work = false;  // a job is waiting for the worker
        bool done = true;   // the worker is idle (and its results, if any, are in `did`)
        std::array<bool, DS::Surface::Count> todo{};
        std::array<bool, DS::Surface::Count> did{};
        double ms = 0;
        bool started = false;
    };
    AsyncRender* g_async = new AsyncRender();  // never freed: the worker thread may outlive static destruction

    void WorkerMain()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // the game's own threads come first
        for (;;) {
            std::array<bool, DS::Surface::Count> todo{};
            {
                std::unique_lock lock(g_async->m);
                g_async->cv.wait(lock, [] { return g_async->work; });
                g_async->work = false;
                todo = g_async->todo;
            }
            const auto start = std::chrono::steady_clock::now();
            std::array<bool, DS::Surface::Count> did{};
            try {
                for (int s = 0; s < DS::Surface::Count; ++s) {
                    if (todo[static_cast<std::size_t>(s)]) did[static_cast<std::size_t>(s)] = DS::Film::Render(s);
                }
            } catch (...) {
                did = {};  // never let an exception kill the thread
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            {
                std::scoped_lock lock(g_async->m);
                g_async->did = did;
                g_async->ms = ms;
                g_async->done = true;
            }
            g_async->cv.notify_all();
        }
    }

    // Waits until the worker is idle: call before anything that changes layers or surfaces.
    void WaitRender()
    {
        std::unique_lock lock(g_async->m);
        g_async->cv.wait(lock, [] { return g_async->done; });
    }

    void ResetSurface(int surface)
    {
        WaitRender();
        if (Tex::Busy(surface)) Tex::Stop(surface);
        DS::Film::Reset(surface);
        g_surf[static_cast<std::size_t>(surface)] = SurfaceState{};
        g_reattach[static_cast<std::size_t>(surface)] = Reattach{};
    }

    // Everything that needs the surfaces goes away when the last mode stops.
    void TeardownIfIdle()
    {
        if (AnyEntryActive() || !g_pending.empty() || !AnySurfaceInUse()) return;
        for (int s = 0; s < DS::Surface::Count; ++s) ResetSurface(s);
        logger::info("Core: no mode running; overlay textures released");
    }

    void StopEntry(Entry& e, StopReason reason)
    {
        if (!e.active) return;
        WaitRender();
        try {
            e.mode->Stop(&e.host, reason);
        } catch (const std::exception& ex) {
            logger::error("Core: mode '{}' threw in Stop: {}", e.name, ex.what());
        } catch (...) {
            logger::error("Core: mode '{}' threw in Stop", e.name);
        }
        for (int s = 0; s < DS::Surface::Count; ++s) {
            if (e.layers[static_cast<std::size_t>(s)]) DS::Film::Release(s, e.layers[static_cast<std::size_t>(s)]);
            e.layers[static_cast<std::size_t>(s)] = nullptr;
        }
        e.surfaces.clear();
        e.active = false;
        e.finishing = false;
        if (DS::LogAllow("core-stopped")) logger::info("Core: mode '{}' stopped", e.name);
    }

    void ProcessStops()
    {
        for (const auto& name : g_stops) {
            if (auto* e = Find(name)) StopEntry(*e, StopReason::Requested);
            std::erase_if(g_pending, [&](const Request& r) { return r.name == name; });
        }
        g_stops.clear();
        for (auto& e : g_modes) {
            if (e->active && e->finishing) StopEntry(*e, StopReason::Requested);
        }
        TeardownIfIdle();
    }

    // ------------------------------------------------------------------ the API object
    class Api final : public DripSim::IDripSim
    {
    public:
        std::uint32_t Version() const override { return DripSim::kApiVersion; }

        bool RegisterMode(DripSim::IMode* mode) override
        {
            if (!mode || !mode->Name() || Find(mode->Name())) return false;
            g_modes.push_back(std::make_unique<Entry>(mode));
            logger::info("Core: registered mode '{}'", mode->Name());
            return true;
        }

        bool Start(const char* mode, std::int32_t variant) override
        {
            if (!mode || !Find(mode)) return false;
            if (g_pending.empty()) g_prepStarted = std::chrono::steady_clock::now();
            g_pending.push_back({ mode, variant });
            return true;
        }

        void Stop(const char* mode) override
        {
            if (mode) g_stops.emplace_back(mode);
        }

        bool IsActive(const char* mode) const override
        {
            const auto* e = mode ? Find(mode) : nullptr;
            return e && e->active;
        }

        bool RegisterHotkey(std::uint32_t scanCode, bool shift, bool ctrl, void (*callback)(void*), void* context) override
        {
            if (!scanCode || !callback) return false;
            std::scoped_lock lock(g_hotkeyMutex);
            g_hotkeys.push_back({ scanCode, shift, ctrl, callback, context });
            return true;
        }

        bool ParseKey(const char* text, std::uint32_t* scanCode, bool* shift, bool* ctrl) const override;

        void Notify(const char* text) override { ::Notify(text); }
        const DripSim::IWorld* World() const override { return DS::World::Get(); }
    };
    Api g_api;

    // DirectInput scan codes of the keys a mode may want
    const std::map<std::string, std::uint32_t>& KeyNames()
    {
        static const std::map<std::string, std::uint32_t> names = [] {
            std::map<std::string, std::uint32_t> m;
            const char* qwerty = "QWERTYUIOP";  // 0x10..0x19
            for (int i = 0; i < 10; ++i) m[std::string(1, qwerty[i])] = 0x10u + static_cast<std::uint32_t>(i);
            const char* home = "ASDFGHJKL";  // 0x1E..0x26
            for (int i = 0; i < 9; ++i) m[std::string(1, home[i])] = 0x1Eu + static_cast<std::uint32_t>(i);
            const char* bottom = "ZXCVBNM";  // 0x2C..0x32
            for (int i = 0; i < 7; ++i) m[std::string(1, bottom[i])] = 0x2Cu + static_cast<std::uint32_t>(i);
            for (int i = 1; i <= 9; ++i) m[std::to_string(i)] = 0x01u + static_cast<std::uint32_t>(i);
            m["0"] = 0x0B;
            for (int i = 1; i <= 10; ++i) m["F" + std::to_string(i)] = 0x3Au + static_cast<std::uint32_t>(i);
            m["F11"] = 0x57;
            m["F12"] = 0x58;
            return m;
        }();
        return names;
    }

    bool Api::ParseKey(const char* text, std::uint32_t* scanCode, bool* shift, bool* ctrl) const
    {
        if (!text || !scanCode || !shift || !ctrl) return false;
        std::string rest = text;
        std::ranges::transform(rest, rest.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        bool s = false;
        bool c = false;
        for (;;) {
            if (rest.starts_with("SHIFT+")) {
                s = true;
                rest.erase(0, 6);
            } else if (rest.starts_with("CTRL+")) {
                c = true;
                rest.erase(0, 5);
            } else {
                break;
            }
        }
        if (rest == "NONE" || rest.empty()) {
            *scanCode = 0;
            *shift = s;
            *ctrl = c;
            return true;
        }
        std::uint32_t code = 0;
        if (rest.starts_with("0X")) {
            try {
                code = static_cast<std::uint32_t>(std::stoul(rest, nullptr, 16));
            } catch (const std::exception&) {
                return false;
            }
        } else {
            const auto it = KeyNames().find(rest);
            if (it == KeyNames().end()) return false;
            code = it->second;
        }
        *scanCode = code;
        *shift = s;
        *ctrl = c;
        return true;
    }

    bool CanUseHotkeys()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded() || !ui || ui->GameIsPaused()) return false;
        for (const auto menu : { RE::MainMenu::MENU_NAME, RE::LoadingMenu::MENU_NAME, RE::Console::MENU_NAME, RE::DialogueMenu::MENU_NAME }) {
            if (ui->IsMenuOpen(menu)) return false;
        }
        return true;
    }

    constexpr std::uint32_t kKeyG = 0x22;
    constexpr std::uint32_t kKeyH = 0x23;
    constexpr std::uint32_t kKeyF10 = 0x44;

    // ------------------------------------------------------------------ preparing the surfaces
    void FailSurface(int surface, const char* why)
    {
        if (DS::LogAllow(std::format("surface-fail-{}-{}", surface, why), 2, 600)) logger::warn("Core: surface '{}' is unavailable ({})", DS::Surface::Name(surface), why);
        if (Tex::Busy(surface)) Tex::Stop(surface);
        g_surf[static_cast<std::size_t>(surface)].prep = Prep::Failed;
    }

    // Moves the needed surfaces towards Ready (one step per frame). Returns true when the body is ready and every other needed surface is ready or gave up.
    bool PrepareSurfaces(std::int32_t needed, std::chrono::steady_clock::time_point now)
    {
        if (DS::Spike::GravityViewActive()) {
            logger::info("Core: the gravity debug view is running; stop it first (Ctrl+Shift+G)");
            Notify("DripSim: stop the gravity view first (Ctrl+Shift+G)");
            g_pending.clear();
            return false;
        }
        const double waited = std::chrono::duration<double>(now - g_prepStarted).count();
        bool allDone = true;
        for (int id = 0; id < DS::Surface::Count; ++id) {
            if (!(needed & DS::Surface::Mask(id))) continue;
            auto& st = g_surf[static_cast<std::size_t>(id)];
            if (st.prep == Prep::None) {
                st.before = DS::Body::Get(id);
                st.prep = Prep::Capturing;
                DS::Body::Request(id, false);
            }
            if (st.prep == Prep::Capturing) {
                auto data = DS::Body::Get(id);
                if (data && data != st.before) {
                    st.build = std::async(std::launch::async, [id, data] { return DS::Field::Get(id).Build(data); });
                    st.prep = Prep::Building;
                } else if (waited > 1.5 && !data) {
                    FailSurface(id, "no capture arrived");
                } else if (waited > 4.0) {
                    FailSurface(id, "the capture took too long");
                }
            } else if (st.prep == Prep::Building) {
                if (st.build.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    if (!st.build.get()) {
                        FailSurface(id, "the field could not be built");
                    } else {
                        const int state = Tex::Start(id);
                        if (state == 1) st.prep = Prep::Ready;
                        else if (state == 2) st.prep = Prep::Texturing;
                        else FailSurface(id, "no texture could be started");
                    }
                }
            } else if (st.prep == Prep::Texturing) {
                const int state = Tex::Poll(id);
                if (state == 1) st.prep = Prep::Ready;
                else if (state == 0) FailSurface(id, "the placeholder texture could not be set up");
            }
            if (st.prep == Prep::Capturing || st.prep == Prep::Building || st.prep == Prep::Texturing) {
                if (id != DS::Surface::Body && waited > kOptionalSurfaceWait) FailSurface(id, "not ready in time");
                else allDone = false;
            }
        }
        if (g_surf[DS::Surface::Body].prep == Prep::Failed) {
            ++g_bodyFailStreak;
            const int waitSeconds = std::min(30 << std::min(g_bodyFailStreak - 1, 5), 600);
            g_holdOffUntil = std::max(g_holdOffUntil, now + std::chrono::seconds(waitSeconds));
            if (DS::LogAllow("body-fail-backoff", 3, 600)) logger::warn("Core: the body could not be prepared ({} time(s) in a row); automatic starts wait {} s (a key still starts at once)", g_bodyFailStreak, waitSeconds);
            if (g_lastSlotNotice.time_since_epoch().count() == 0 || now - g_lastSlotNotice >= std::chrono::minutes(5)) {
                g_lastSlotNotice = now;
                Notify("DripSim: no body overlay slot to draw on (needs a free 'Body [Ovl#]' slot; with no other overlay, the placeholder texture file is needed)");
            }
            g_pending.clear();
            for (int s = 0; s < DS::Surface::Count; ++s) ResetSurface(s);
            return false;
        }
        const bool ready = allDone && Ready(DS::Surface::Body);
        if (ready) g_bodyFailStreak = 0;
        return ready;
    }

    // Starts the pending modes once their surfaces are ready.
    void StartPending()
    {
        for (int s = 0; s < DS::Surface::Count; ++s) {
            if (Ready(s)) DS::Field::Get(s).Update();
        }
        for (const auto& request : g_pending) {
            auto* e = Find(request.name);
            if (!e) continue;
            if (e->active) StopEntry(*e, StopReason::Requested);  // a restart
            const auto mask = e->mode->Surfaces() | DripSim::kSurfaceBody;
            bool failed = false;
            for (int s = 0; s < DS::Surface::Count; ++s) {
                if (!(mask & DS::Surface::Mask(s)) || !Ready(s)) continue;
                auto* layer = DS::Film::Acquire(s, DS::Field::Get(s).KeepMask().data());
                if (!layer) {
                    logger::warn("Core: no free film layer on '{}' for mode '{}'", DS::Surface::Name(s), e->name);
                    if (s == DS::Surface::Body) failed = true;
                    continue;
                }
                e->layers[static_cast<std::size_t>(s)] = layer;
                e->surfaces.push_back(s);
            }
            bool ok = false;
            if (!failed) {
                try {
                    ok = e->mode->Start(&e->host, request.variant);
                } catch (const std::exception& ex) {
                    logger::error("Core: mode '{}' threw in Start: {}", e->name, ex.what());
                } catch (...) {
                    logger::error("Core: mode '{}' threw in Start", e->name);
                }
            }
            if (!ok) {
                for (int s = 0; s < DS::Surface::Count; ++s) {
                    if (e->layers[static_cast<std::size_t>(s)]) DS::Film::Release(s, e->layers[static_cast<std::size_t>(s)]);
                    e->layers[static_cast<std::size_t>(s)] = nullptr;
                }
                e->surfaces.clear();
                logger::info("Core: mode '{}' declined to start", e->name);
                continue;
            }
            e->active = true;
            e->finishing = false;
            std::string names;
            for (const int s : e->surfaces) names += std::format("{}{}", names.empty() ? "" : "+", DS::Surface::Name(s));
            if (DS::LogAllow("core-started")) logger::info("Core: mode '{}' started (variant {}, surfaces {})", e->name, request.variant, names);
        }
        g_pending.clear();
        g_last = g_lastField = g_lastRender = std::chrono::steady_clock::now();
        TeardownIfIdle();
    }
}

namespace DS::Core
{
    DripSim::IDripSim* Api()
    {
        return &g_api;
    }

    bool AnyActive()
    {
        return AnyEntryActive() || !g_pending.empty() || AnySurfaceInUse();
    }

    void StopAll(StopReason reason)
    {
        for (auto& e : g_modes) StopEntry(*e, reason);
        g_pending.clear();
        g_stops.clear();
        if (!DS::Spike::GravityViewActive()) {
            for (int s = 0; s < DS::Surface::Count; ++s) ResetSurface(s);
        }
    }

    bool WantsKey(std::uint32_t key)
    {
        if (key == kKeyF10) return true;  // F10 (re-read the settings) is always on
        if (!DS::Params::Get().hotkeys) return false;  // Hotkeys=0 (the default): no other key
        if ((key == kKeyG || key == kKeyH) && DS::Params::Get().debugKeys) return true;
        std::scoped_lock lock(g_hotkeyMutex);
        return std::ranges::any_of(g_hotkeys, [key](const Hotkey& h) { return h.key == key; });
    }

    // F10 and the tuner (DripSimReloadSettings): every INI is read again; the modes apply their values at once.
    void ReloadAll(bool notify)
    {
        const auto summary = DS::Params::Load();
        for (auto& e : g_modes) {
            try {
                e->mode->Reload();
            } catch (...) {
                logger::error("Core: mode '{}' threw in Reload", e->name);
            }
        }
        if (notify) Notify(std::format("DripSim: settings reloaded ({}, {} modes)", summary, g_modes.size()).c_str());
    }

    void OnKey(std::uint32_t key, bool shift, bool ctrl)
    {
        if (!CanUseHotkeys()) return;
        if (key == kKeyF10) return ReloadAll(true);  // always on
        if (!DS::Params::Get().hotkeys) return;
        if (DS::Params::Get().debugKeys) {  // the author's debug keys (DebugKeys=1 in DripSim.ini; off for players: Ctrl is the crouch key)
            if (key == kKeyH && ctrl && shift) return DS::Spike::RequestBodyDump();
            if (key == kKeyH && ctrl) return DS::Spike::DumpPlayerShapes();
            if (key == kKeyG && ctrl && shift) return DS::Spike::ToggleGravityView();
        }
        if (key == kKeyF10) return ReloadAll(true);
        std::vector<Hotkey> matches;
        {
            std::scoped_lock lock(g_hotkeyMutex);
            for (const auto& h : g_hotkeys) {
                if (h.key == key && h.shift == shift && h.ctrl == ctrl) matches.push_back(h);
            }
        }
        for (const auto& h : matches) {
            try {
                h.callback(h.context);
            } catch (...) {
                logger::error("Core: a hotkey callback threw");
            }
        }
    }

    // The last job's results go to the overlay textures (the worker is idle after WaitRender).
    // Returns false when the worker is still busy: the whole frame is then skipped (the modes pause for a moment), never waited for - the first render after a start is a
    // full one (every tile of four surfaces, ~0.3 s) and must not freeze the game.
    bool CollectRender()
    {
        {
            std::scoped_lock lock(g_async->m);
            if (!g_async->done) return false;
        }
        if (!g_async->started) return true;
        for (int s = 0; s < DS::Surface::Count; ++s) {
            if (!g_async->did[static_cast<std::size_t>(s)]) continue;
            g_async->did[static_cast<std::size_t>(s)] = false;
            if (!Ready(s)) continue;
            Tex::SetTiles(s, DS::Film::Pixels(s), DS::Film::ChangedTiles(s));
            if (const auto* normals = DS::Film::NormalPixels(s)) Tex::SetNormalTiles(s, normals, DS::Film::ChangedTiles(s));
            if (const auto* gloss = DS::Film::SpecPixels(s)) Tex::SetSpecTiles(s, gloss, DS::Film::ChangedTiles(s));
            ++g_renders;
        }
        g_renderMs += g_async->ms;
        if (g_async->ms > 0.0) g_lastJobMs = g_async->ms;
        if (g_session.on && g_async->ms > 0.0) {
            ++g_session.jobs;
            g_session.jobSum += g_async->ms;
            g_session.jobMax = std::max(g_session.jobMax, g_async->ms);
        }
        g_async->ms = 0;
        return true;
    }

    double MsSince(std::chrono::steady_clock::time_point t)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    }

    void EndSession(std::chrono::steady_clock::time_point now)
    {
        auto& s = g_session;
        s.on = false;
        const double seconds = std::chrono::duration<double>(now - s.start).count();
        if (seconds < 2.0 || s.frames == 0) return;
        const double n = static_cast<double>(s.frames);
        const double avg = s.frameSum / n;
        const std::string base = g_baselineMs > 0.0 ? std::format("{:.1f} ms ({:.0f} fps)", g_baselineMs, 1000.0 / g_baselineMs) : "not measured";
        g_lastSummary = std::format("Summary: {} ran {:.0f} s: game frame {:.1f} ms avg ({:.0f} fps), worst {:.0f} ms; without an effect {} | DripSim main thread {:.2f} ms a frame | "
                                    "film job {:.0f} ms avg, worst {:.0f} ms ({} jobs); {:.0f} % of the frames waited for the film",
            s.modes.empty() ? "an effect" : s.modes, seconds, avg, 1000.0 / std::max(avg, 0.001), s.frameMax, base, s.ourMs / std::max<double>(static_cast<double>(s.frames + s.skipped + s.longFrames), 1.0),
            s.jobs ? s.jobSum / static_cast<double>(s.jobs) : 0.0, s.jobMax, s.jobs, 100.0 * static_cast<double>(s.skipped) / std::max<double>(n, 1.0));
        logger::info("{}", g_lastSummary);
        if (DS::Params::Get().autoReport && (g_lastAutoReport.time_since_epoch().count() == 0 || now - g_lastAutoReport >= std::chrono::minutes(5))) {
            g_lastAutoReport = now;
            WriteReport(false);
        }
    }

    // Every frame: the frame interval goes into the running effect's summary, or into the baseline while no effect runs.
    void UpdateSession(std::chrono::steady_clock::time_point now, double interval)
    {
        const bool active = AnyEntryActive();
        auto& s = g_session;
        if (active && !s.on) {
            s = Session{};
            s.on = true;
            s.start = now;
        } else if (!active && s.on) {
            EndSession(now);
        }
        if (!active) {
            if (interval < 250.0) g_baselineMs = g_baselineMs <= 0.0 ? interval : g_baselineMs * 0.99 + interval * 0.01;
            return;
        }
        if (interval >= 250.0) {
            ++s.longFrames;
            return;
        }
        ++s.frames;
        s.frameSum += interval;
        s.frameMax = std::max(s.frameMax, interval);
        for (const auto& e : g_modes) {  // (at most a few modes)
            if (e->active && s.modes.find(e->name) == std::string::npos) s.modes += (s.modes.empty() ? "" : "+") + e->name;
        }
    }

    // Accumulates the whole OnFrame cost and writes the PERF line.
    struct FrameTimer
    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~FrameTimer()
        {
            g_perf.total += MsSince(start);
            if (g_session.on) g_session.ourMs += MsSince(start);
            if (!DS::Params::Get().perfLog || g_perf.frames < 30 || MsSince(g_perf.logged) < 5000.0) return;
            const double n = static_cast<double>(g_perf.frames);
            if (!AnyEntryActive()) {  // no effect: only the game's own frame time, the baseline to compare the effect against (a menu or a loading screen makes it large: compare in the same place)
                logger::info("PERF baseline (no effect running): game frame {:.1f} ms avg ({:.0f} fps), worst {:.0f} ms over {} frames", g_perf.frameSum / n, 1000.0 * n / std::max(g_perf.frameSum, 1.0),
                    g_perf.frameMax, g_perf.frames);
                g_perf = Perf{ g_perf.lastFrame, std::chrono::steady_clock::now() };
                return;
            }
            logger::info("PERF effect: game frame {:.1f} ms avg ({:.0f} fps), worst {:.0f} ms | our main-thread cost per frame {:.2f} ms = collect/upload-copy {:.2f} + gravity field {:.2f} + modes {:.2f} | "
                         "background render {:.1f} ms/job, {} jobs skipped a frame of {}",
                g_perf.frameSum / n, 1000.0 * n / std::max(g_perf.frameSum, 1.0), g_perf.frameMax, g_perf.total / n, g_perf.collect / n, g_perf.field / n, g_perf.modes / n, g_lastJobMs,
                g_perf.skipped, g_perf.frames);
            const auto up = DS::Spike::TextureTakeUploadStats();
            const double upFrames = std::max<double>(static_cast<double>(up.frames), 1.0);
            logger::info("PERF: render-thread uploads {} frames, {:.2f} ms each ({} calls, {:.1f} MB per frame), worst {:.1f} ms, waited {:.2f} ms/frame for the lock", up.frames, up.totalMs / upFrames,
                up.calls / std::max<std::uint64_t>(up.frames, 1), up.megabytes / upFrames, up.worstMs, up.lockWaitMs / upFrames);
            g_perf = Perf{ g_perf.lastFrame, std::chrono::steady_clock::now() };
        }
    };

    // One step per frame for every surface that is being prepared again (see Reattach).
    void ReattachStep(std::chrono::steady_clock::time_point now)
    {
        for (int id = 1; id < DS::Surface::Count; ++id) {
            auto& r = g_reattach[static_cast<std::size_t>(id)];
            if (!r.active) continue;
            const auto name = DS::Surface::Name(id);
            const auto retry = [&](const char* why) {
                if (Tex::Busy(id)) Tex::Stop(id);
                ++r.tries;
                if (r.tries >= 5) {
                    logger::warn("Core: surface '{}' could not be attached again ({}); it stays off", name, why);
                    r = Reattach{};
                    return;
                }
                logger::info("Core: surface '{}' not back yet ({}); trying again", name, why);
                r.step = 0;
                r.next = now + std::chrono::milliseconds(2000);
            };
            if (r.step == 0) {
                if (now < r.next) continue;
                r.before = DS::Body::Get(id);
                DS::Body::Request(id, false);
                r.started = now;
                r.step = 1;
            } else if (r.step == 1) {
                const auto data = DS::Body::Get(id);
                if (data && data != r.before) {
                    WaitRender();  // the film worker reads the field
                    if (!DS::Field::Get(id).Build(data)) {
                        retry("the field could not be built");
                        continue;
                    }
                    const int state = Tex::Start(id);
                    if (state == 1) {
                        r.step = 3;
                    } else if (state == 2) {
                        r.step = 2;
                        r.started = now;
                    } else {
                        retry("no texture could be started");
                        continue;
                    }
                } else if (now - r.started > std::chrono::seconds(3)) {
                    retry("no capture arrived");
                    continue;
                }
            } else if (r.step == 2) {
                const int state = Tex::Poll(id);
                if (state == 1) r.step = 3;
                else if (state == 0) retry("the placeholder texture could not be set up");
                else if (now - r.started > std::chrono::seconds(4)) retry("the placeholder texture did not load");
            }
            if (r.step == 3) {
                WaitRender();
                DS::Film::Rebind(id, DS::Field::Get(id).KeepMask().data());
                g_surf[static_cast<std::size_t>(id)].prep = Prep::Ready;
                logger::info("Core: surface '{}' is back (attached again after {} {})", name, r.tries + 1, r.tries == 0 ? "try" : "tries");
                r.step = 4;  // the normal map texture comes a moment after the colour texture: send every tile once more then
                r.next = now + std::chrono::milliseconds(1500);
            } else if (r.step == 4 && now >= r.next) {
                DS::Film::Recomposite(id);
                r = Reattach{};
            }
        }
    }

    void OnFrame()
    {
        const auto now = std::chrono::steady_clock::now();
        const FrameTimer frameTimer;
        if (g_perf.lastFrame.time_since_epoch().count() != 0) {
            const double interval = std::chrono::duration<double, std::milli>(now - g_perf.lastFrame).count();
            g_perf.frameSum += interval;
            g_perf.frameMax = std::max(g_perf.frameMax, interval);
            ++g_perf.frames;
            UpdateSession(now, interval);
        }
        g_perf.lastFrame = now;
        if (g_last.time_since_epoch().count() == 0) g_last = g_lastField = g_lastRender = now;
        const auto collectStart = std::chrono::steady_clock::now();
        const bool collected = CollectRender();
        g_perf.collect += MsSince(collectStart);
        if (!collected) {
            ++g_perf.skipped;
            if (g_session.on) ++g_session.skipped;
            // skipped: the game goes on, the film catches up. The time is kept (g_last stays): the next update moves the drops by all of it (up to 0.1 s), so they move
            // at their real speed in even steps; before, the skipped time was dropped and the drops slowed down and moved unevenly while the film was slow
            return;
        }
        ProcessStops();

        // modes that are not running get an Idle call twice a second (automatic starts)
        g_idleTimer += std::chrono::duration<float>(now - g_last).count();
        if (g_idleTimer >= 0.5f) {
            const float dt = g_idleTimer;
            g_idleTimer = 0.0f;
            if (DS::Params::Get().enabled && !DS::Spike::GravityViewActive()) {
                static bool prewarmed = false;
                if (!prewarmed) {  // allocate the film buffers (~200 MB) and the noise in the background, before anybody needs them
                    prewarmed = true;
                    std::thread([] {
                        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                        DS::Film::Prewarm();
                        logger::info("Core: film buffers prepared");
                    }).detach();
                }
                DS::World::Tick(dt);
                const bool holdOff = now < g_holdOffUntil;
                for (auto& e : g_modes) {
                    if (e->active || holdOff) continue;
                    try {
                        e->mode->Idle(&g_api, dt);
                    } catch (...) {
                        logger::error("Core: mode '{}' threw in Idle", e->name);
                    }
                }
            }
        }

        if (!g_pending.empty() && !DS::Params::Get().enabled) {
            logger::info("Core: disabled in DripSim.ini");
            g_pending.clear();
        }
        if (!g_pending.empty()) {  // modes want to start: get their surfaces ready (one step per frame), then start them
            if (!PrepareSurfaces(NeededMask(), now)) {
                g_last = now;
                return;
            }
            StartPending();
        }
        if (!AnyEntryActive()) {
            g_last = now;
            return;
        }

        if (!DS::Field::Get(DS::Surface::Body).Alive() || !Tex::Running(DS::Surface::Body)) {
            if (DS::LogAllow("core-gone")) logger::info("Core: the body or its overlay is gone; stopping all modes");
            StopAll(StopReason::BodyLost);
            g_lostTimes.push_back(now);
            while (!g_lostTimes.empty() && now - g_lostTimes.front() > std::chrono::seconds(20)) g_lostTimes.pop_front();
            if (g_lostTimes.size() >= 3) {
                g_holdOffUntil = now + std::chrono::seconds(30);
                g_lostTimes.clear();
                logger::warn("Core: the body was lost 3 times in 20 s (something rebuilds the player's 3D again and again): no automatic start for 30 s");
            }
            g_last = now;
            return;
        }
        for (int s = 1; s < DS::Surface::Count; ++s) {  // an optional surface that went away (gloves, boots, a rebuilt 3D) just stops being drawn
            if (Ready(s) && (!DS::Field::Get(s).Alive() || !Tex::Running(s))) {
                if (DS::LogAllow("core-surface-gone")) logger::info("Core: surface '{}' is gone", DS::Surface::Name(s));
                if (Tex::Busy(s)) Tex::Stop(s);
                g_surf[static_cast<std::size_t>(s)].prep = Prep::Failed;
                const bool wanted = std::ranges::any_of(g_modes, [s](const std::unique_ptr<Entry>& e) { return e->active && e->layers[static_cast<std::size_t>(s)]; });
                if (wanted) {  // a running mode draws on it: try to get it back
                    auto& r = g_reattach[static_cast<std::size_t>(s)];
                    r = Reattach{};
                    r.active = true;
                    r.next = now + std::chrono::milliseconds(1000);
                }
            }
        }
        ReattachStep(now);
        const float dt = static_cast<float>(std::clamp(std::chrono::duration<double>(now - g_last).count(), 0.0, 0.1));
        g_last = now;
        if (now - g_lastField >= std::chrono::milliseconds(33)) {
            const auto fieldStart = std::chrono::steady_clock::now();
            for (int s = 0; s < DS::Surface::Count; ++s) {
                if (Ready(s)) DS::Field::Get(s).Update();
            }
            g_lastField = now;
            g_perf.field += MsSince(fieldStart);
        }
        DS::World::Tick(dt);

        const auto modesStart = std::chrono::steady_clock::now();
        for (int s = 0; s < DS::Surface::Count; ++s) {
            if (Ready(s)) DS::Film::BeginFrame(s);
        }
        for (auto& e : g_modes) {
            if (!e->active) continue;
            try {
                e->mode->Update(&e->host, dt);
            } catch (const std::exception& ex) {
                logger::error("Core: mode '{}' threw in Update: {}; stopping it", e->name, ex.what());
                e->finishing = true;
            } catch (...) {
                logger::error("Core: mode '{}' threw in Update; stopping it", e->name);
                e->finishing = true;
            }
        }
        for (int s = 0; s < DS::Surface::Count; ++s) {
            if (Ready(s)) DS::Film::EndFrame(s);
        }
        g_perf.modes += MsSince(modesStart);
        ProcessStops();
        g_updateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
        ++g_frames;

        DS::Spike::TextureSetNormalMaps(DS::Params::Get().normalMaps);
        DS::Spike::TextureSetSpecMaps(DS::Params::Get().normalMaps && DS::Params::Get().specMaps);
        DS::Spike::TextureSetPremultiplied(DS::Params::Get().premultiplied);
        for (int s = 0; s < DS::Surface::Count; ++s) DS::Film::SetPremultiplied(s, DS::Spike::TexturePremultiplied(s));
        for (int s = 0; s < DS::Surface::Count; ++s) DS::Film::SetGlossCompensation(s, DS::Spike::TextureSpecRunning(s) && !DS::Spike::TexturePremultiplied(s));
        DS::Spike::TextureSetRefraction(DS::Params::Get().refraction);
        DS::Spike::TextureSetDecal(DS::Params::Get().overlayDecal);
        DS::Film::SetNormalMapsEnabled(DS::Params::Get().normalMaps);
        DS::Film::SetSpecMapsEnabled(DS::Params::Get().normalMaps && DS::Params::Get().specMaps);
        for (int s = 0; s < DS::Surface::Count; ++s) {
            if (!Ready(s)) continue;
            float power = 0.0f;
            float scale = 0.0f;
            const bool has = DS::Film::MaterialSpec(s, &power, &scale);
            Tex::SetSpec(s, has, power, scale);
        }

        // the update rate adapts to the cost: never more than ~2/3 of the time a render takes between two jobs, so heavy scenes get a slower film, not a slower game
        using Clock = std::chrono::steady_clock::duration;
        const auto interval = std::max(std::chrono::duration_cast<Clock>(kRenderInterval), std::chrono::duration_cast<Clock>(std::chrono::duration<double, std::milli>(g_lastJobMs * 1.5)));
        if (now - g_lastRender >= interval) {  // start the next render job; it is collected at the start of the next frame
            {
                std::scoped_lock lock(g_async->m);
                for (int s = 0; s < DS::Surface::Count; ++s) g_async->todo[static_cast<std::size_t>(s)] = Ready(s);
                g_async->done = false;
                g_async->work = true;
            }
            if (!g_async->started) {
                g_async->started = true;
                std::thread(WorkerMain).detach();
            }
            g_async->cv.notify_all();
            g_lastRender = now;
        }
        if (g_frames % 300 == 0) {
            int layers = 0;
            for (int s = 0; s < DS::Surface::Count; ++s) layers += DS::Film::ActiveLayers(s);
            logger::info("Core: {} layers, modes {:.2f} ms/frame, composite {:.2f} ms/frame ({} renders)", layers, g_updateMs / 300.0, g_renderMs / 300.0, g_renders);
            g_updateMs = g_renderMs = 0;
            g_renders = 0;
        }
    }

    // ------------------------------------------------------------------ the bug report (KeyReport, default Shift+F6)
    std::string g_gameInfo = "(not set)";

    void SetGameInfo(const std::string& text) { g_gameInfo = text; }
    void SetSystemInfo(const std::string& text) { g_systemInfo = text; }

    // The last `count` lines of a text file (empty when it cannot be read).
    std::vector<std::string> TailOf(const std::filesystem::path& path, std::size_t count)
    {
        std::deque<std::string> lines;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            if (lines.size() > count) lines.pop_front();
        }
        return { lines.begin(), lines.end() };
    }

    const char* PrepName(Prep p)
    {
        switch (p) {
        case Prep::None: return "not started";
        case Prep::Capturing: return "capturing the mesh";
        case Prep::Building: return "building the field";
        case Prep::Texturing: return "setting up the texture";
        case Prep::Ready: return "ready";
        case Prep::Failed: return "unavailable";
        }
        return "?";
    }

    // One file with everything a bug report needs: the versions, what is loaded, the state of the surfaces and the tail of every DripSim log. The player attaches this one file.
    void WriteReport(bool notify)
    {
        const auto directory = SKSE::log::log_directory();
        if (!directory) return;
        std::ofstream out(*directory / "DripSim_report.txt", std::ios::trunc);
        if (!out) return;
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[40] = {};
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        out << "DripSim report, " << stamp << "\n";
        out << "framework " << DS_VERSION << ", API " << DripSim::kApiVersion << " (serves " << DripSim::kApiVersionMin << ".." << DripSim::kApiVersion << ")\n";
        out << g_gameInfo << "\n";
        out << g_systemInfo << "\n";
        out << (g_session.on ? "(an effect is running now) " : "") << "last effect run: " << g_lastSummary << "\n";

        out << "\n== Address Library files in Data\\\\SKSE\\\\Plugins\n";
        std::error_code ec;
        bool anyAddress = false;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path("Data") / "SKSE" / "Plugins", ec)) {
            auto name = entry.path().filename().string();
            std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.starts_with("version-") || name.starts_with("versionlib-")) {
                out << "  " << entry.path().filename().string() << "\n";
                anyAddress = true;
            }
        }
        if (!anyAddress) out << "  none found\n";

        out << "\n== DripSim files loaded in the game\n";
        if (const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, ::GetCurrentProcessId()); snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            for (BOOL ok = ::Module32FirstW(snap, &module); ok; ok = ::Module32NextW(snap, &module)) {
                const std::wstring name = module.szModule;
                if (name.starts_with(L"DripSim")) out << "  " << std::filesystem::path(name).string() << "\n";
            }
            ::CloseHandle(snap);
        }

        const auto& params = DS::Params::Get();
        out << "\n== Framework settings (DripSim.ini, as read)\n";
        out << "  Enabled " << params.enabled << ", Hotkeys " << params.hotkeys << ", AutoReport " << params.autoReport << ", NormalMaps " << params.normalMaps << ", SpecMaps " << params.specMaps << ", PremultipliedAlpha " << params.premultiplied << ", OverlayDecal " << params.overlayDecal << ", PerfLog " << params.perfLog << ", DonorTextures "
            << params.donor << ", Refraction " << params.refraction << ", RainDensityRef " << params.rainDensityRef << "\n";

        out << "\n== Modes\n";
        for (const auto& e : g_modes) {
            out << "  " << e->name << ": " << (e->active ? "running" : "idle");
            if (e->active) {
                out << ", surfaces";
                for (const int s : e->surfaces) out << " " << DS::Surface::Name(s);
            }
            out << "\n";
        }
        out << "\n== Surfaces\n";
        for (int s = 0; s < DS::Surface::Count; ++s) {
            out << "  " << DS::Surface::Name(s) << ": " << PrepName(g_surf[static_cast<std::size_t>(s)].prep) << ", texture " << (Tex::Running(s) ? "running" : "off") << ", field "
                << (DS::Field::Get(s).Alive() ? "alive" : "gone") << "\n";
        }
        out << "  overlay textures use " << DS::Atlas::kFilm << " x " << DS::Atlas::kFilm << " pixels each (4 bytes a pixel, plus the same again for the normal map)\n";

        for (const auto& entry : std::filesystem::directory_iterator(*directory, ec)) {
            const auto name = entry.path().filename().string();
            if (!entry.is_regular_file() || !name.starts_with("DripSim") || entry.path().extension() != ".log") continue;
            const bool main = name == "DripSim.log";
            out << "\n== " << name << " (last " << (main ? 250 : 80) << " lines)\n";
            for (const auto& line : TailOf(entry.path(), main ? 250 : 80)) out << line << "\n";
        }
        out << "\n== end of report\n";
        if (notify) Notify("DripSim: report written (DripSim_report.txt in the SKSE log folder). Attach it to a bug report.");
    }

    void RegisterReportKey()
    {
        const DripSim::Ini ini("DripSim");
        std::uint32_t code = 0;
        bool shift = false;
        bool ctrl = false;
        const auto text = ini.Text("KeyReport", "Shift+F6");
        if (g_api.ParseKey(text.c_str(), &code, &shift, &ctrl) && code && g_api.RegisterHotkey(code, shift, ctrl, [](void*) { WriteReport(true); }, nullptr)) {
            logger::info("Hotkey KeyReport = {} (writes DripSim_report.txt)", text);
        } else {
            logger::info("KeyReport={} is not a usable key (or NONE): no report key", text);
        }
    }

}

// The framework's entry point for mode plugins (see api/DripSimMode.h: DripSim::RequestAPI).
extern "C" __declspec(dllexport) void* DripSimRequestAPI(std::uint32_t version)
{
    return version >= DripSim::kApiVersionMin && version <= DripSim::kApiVersion ? static_cast<void*>(DS::Core::Api()) : nullptr;
}

// For the author's tuner plugin (a separate, private DLL): read every INI again, and save the pixels of the film. Any thread; the work runs on the game's main thread.
extern "C" __declspec(dllexport) void DripSimReloadSettings()
{
    if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([] { DS::Core::ReloadAll(false); });
}

extern "C" __declspec(dllexport) void DripSimDumpFilm()
{
    if (auto* tasks = SKSE::GetTaskInterface()) {
        tasks->AddTask([] {
            DS::Spike::TextureDumpFilm();
            RE::SendHUDMessage::ShowHUDMessage("DripSim: film dump written");
        });
    }
}
