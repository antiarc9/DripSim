// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "DripSimAPI.h"

#include <cstdint>
#include <future>
#include <random>
#include <vector>

// Water after a swim as a draining sheet (a thin-film flow on one skin surface). The skin that was under water starts with an even sheet; it runs down the live gravity
// direction of the pose, faster where it is thicker (a film flows with the square of its thickness), gathers into streams, leaves a thin wet film behind where it
// stopped, and that film dries. A few sources near the top of the wet skin (the neck, the shoulders: water from the hair and the chin) keep feeding streams while the soak runs.
// The water is a 512 x 512 thickness map over the surface's atlas (2 atlas pixels a cell), handed to the film with ILayer::SetFlow (API 5).
struct FlowParams
{
    float speed = 1.0f;       // multiplies the flow speed
    float pin = 0.07f;        // thickness below which water holds still (it only dries)
    float film = 0.035f;      // thickness the moving water leaves behind (the wet film)
    float dryTime = 90.0f;    // seconds a resting film takes to dry
    float streamRate = 1.0f;  // water the stream sources add
    float gather = 1.0f;      // how strongly water is pulled sideways into streams
    float drawn = 1.0f;       // the sheet is drawn this many times thicker than it is simulated (the look; the flow does not change)
};

class SoakFlow
{
public:
    static constexpr int kN = 512;     // cells a side
    static constexpr int kTile = 32;   // cells a side of a tile (activity tracking and the upload to the film)
    static constexpr int kTiles = kN / kTile;

    void Reset();
    // The skin below zLine (skin space, the units of IField::CenterZ) gets a sheet of this thickness; `streams` sources near the top of that skin.
    void Flood(const DripSim::IField& field, float zLine, float sheet, int streams, std::mt19937& rng);
    // Main thread, every frame. source: 0..1, how strongly the stream sources still give water (the soak running down). The simulation runs on a worker thread;
    // the changed parts of the map go to the layer between two runs. drawn: the map is drawn this many times thicker.
    void Step(const DripSim::IField& field, DripSim::ILayer& layer, float dt, const FlowParams& p, float source, float drawn);
    // Clears the layer's map once all the water is gone (call it when Empty()).
    void Push(DripSim::ILayer& layer, float drawn);
    void Wait();  // waits for a running step (the object must not move or go away while one runs: RainMode keeps it in place and Reset / the destructor wait)
    ~SoakFlow() { Wait(); }
    SoakFlow() = default;
    SoakFlow(SoakFlow&& other) noexcept { other.Wait(); *this = std::move(other); }
    SoakFlow& operator=(SoakFlow&& other) noexcept
    {
        Wait();
        other.Wait();
        Assign(std::move(other));
        return *this;
    }
    bool Empty() const { return !m_used; }
    float Water() const { return m_total; }  // sum of the thickness over all cells (for the log)
    double Ms() const { return m_ms; }       // main-thread time of the last Step
    double JobMs() const { return m_jobMs; } // worker time of the last run
    int Twins() const { return m_twins; }    // cells that pass water across a seam (for the log)

private:
    void RefreshGravity(const DripSim::IField& field, bool all);
    void Tick(float dt, const FlowParams& p, float source);
    void Dump(float dt);
    void Assign(SoakFlow&& other);
    void BuildOut(float drawn);  // worker
    void Send(DripSim::ILayer& layer, float drawn);
    bool m_outReady = false;  // m_out holds a new map to send
    std::future<void> m_job;
    double m_jobMs = 0.0;

    std::vector<float> m_h;       // thickness now
    std::vector<float> m_d;       // change of this tick
    std::vector<float> m_sent;    // what the film has (the smoothed map, x drawn)
    std::vector<float> m_out;     // the map handed over (x drawn, smoothed)
    std::vector<float> m_tmp;
    std::vector<float> m_gx, m_gy, m_steep, m_mm;  // downhill direction (unit, atlas axes), 0..1, millimetres a cell
    std::vector<std::uint8_t> m_inner;  // 1 = on the skin (inside a UV island)
    std::vector<float> m_tc, m_ts;  // per cell: cos, sin of the fixed turn of the downhill direction (streams wander)
    std::vector<int> m_twin;      // per cell near an island edge: the cell across the UV seam (-1 = none: an open edge)
    std::vector<std::uint8_t> m_wake;  // per tile: got water across a seam this step
    int m_twins = 0;
    std::vector<int> m_gsrc;      // per cell: the cell whose downhill direction it uses (itself, or the nearest one 3 px or more from the island edge)
    std::vector<float> m_keep;    // per cell factor (0.8..1.2) on the pin: the water stops a little unevenly (the film it leaves is even)
    std::vector<int> m_sources;
    std::vector<std::uint8_t> m_active;  // per tile: has water
    std::vector<std::uint8_t> m_moving;  // per tile: has water that moved in the last step
    float m_lastDt = 0.0f;
    float m_pushAcc = 0.0f;
    float m_acc = 0.0f;
    float m_gravAge = 0.0f;
    float m_total = 0.0f;
    double m_ms = 0.0;
    bool m_used = false;
    bool m_layerHasFlow = false;
    int m_cell = 2;  // atlas pixels a cell
    float m_dumpAcc = 0.0f;  // development dump (DRIPSIM_FLOW_DUMP)
    int m_dumpIndex = 0;
};
