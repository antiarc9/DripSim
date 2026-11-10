// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
// DripSimPreview: runs a DripSim mode without the game and writes pictures of the result.
//
// It links the SAME code as the plugins: the film renderer (Film.cpp), the gravity field (Field.cpp, standing pose) and the mode itself
// (RainMode.cpp), against a stand-in host. The picture is the real film drawn over a simple shaded body (orthographic, flat skin
// colour, one light, a rough specular term for the Sheen setting), so it shows the shapes, the opacity, the edges and the motion, not the
// game's lighting or ENB.
//
//   DripSimPreview --pack body_pack.bin --mode rain --out out --at 0.5,1.5,4,10 --view belly
//
// Options: --pack file   body data (python export_pack.py)        --mode rain          --out dir (pictures go here, and Data\SKSE\Plugins\*.ini)
//          --at t,t,..   seconds at which a picture is taken      --seconds N          run at least this long (default: last --at)
//          --view full|torso|belly|chest|thighs|z0,z1   which part of the body height (0..1) to show      --yaw deg   turn the body (0 = front)
//          --size WxH    picture size (default 900x900)           --ini K=V,K=V        settings of the mode (its INI keys)
//          --rain X      rain intensity 0..1 (forced mode, default 0.9)   --skin r,g,b   --soak D   water that comes off a dunk, skin below D game units wet (1000 = all)
//          --atlas       also write the raw 1024x1024 film        --verbose            print the mode's log lines
//          --also-rain   runs the rain mode in a second layer on top of the main mode (stacking test)
#include "Field.h"
#include "Film.h"
#include "RainMode.h"

namespace
{
    // ------------------------------------------------------------------ the pack
    std::shared_ptr<DS::Body::Static> LoadPack(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return nullptr;
        char magic[4];
        std::uint32_t nv = 0;
        std::uint32_t nt = 0;
        std::uint32_t nb = 0;
        f.read(magic, 4);
        f.read(reinterpret_cast<char*>(&nv), 4);
        f.read(reinterpret_cast<char*>(&nt), 4);
        f.read(reinterpret_cast<char*>(&nb), 4);
        if (!f || std::memcmp(magic, "DSP1", 4) != 0) return nullptr;
        auto body = std::make_shared<DS::Body::Static>();
        body->surface = DS::Surface::Body;
        body->vertexCount = nv;
        body->triangleCount = nt;
        body->boneCount = nb;
        body->position.resize(static_cast<std::size_t>(nv) * 3);
        body->uv.resize(static_cast<std::size_t>(nv) * 2);
        body->weight.resize(static_cast<std::size_t>(nv) * 4);
        body->boneIndex.resize(static_cast<std::size_t>(nv) * 4);
        body->triangle.resize(static_cast<std::size_t>(nt) * 3);
        body->skinToBoneRotation.resize(static_cast<std::size_t>(nb) * 9);
        f.read(reinterpret_cast<char*>(body->position.data()), static_cast<std::streamsize>(body->position.size() * 4));
        f.read(reinterpret_cast<char*>(body->uv.data()), static_cast<std::streamsize>(body->uv.size() * 4));
        f.read(reinterpret_cast<char*>(body->weight.data()), static_cast<std::streamsize>(body->weight.size() * 4));
        f.read(reinterpret_cast<char*>(body->boneIndex.data()), static_cast<std::streamsize>(body->boneIndex.size()));
        f.read(reinterpret_cast<char*>(body->triangle.data()), static_cast<std::streamsize>(body->triangle.size() * 2));
        f.read(reinterpret_cast<char*>(body->skinToBoneRotation.data()), static_cast<std::streamsize>(body->skinToBoneRotation.size() * 4));
        return f ? body : nullptr;
    }

    // ------------------------------------------------------------------ the stand-in host
    class FieldView final : public DripSim::IField
    {
    public:
        explicit FieldView(const DS::Field::Instance& field) : m_field(field) {}
        std::int32_t Size() const override { return DS::Field::kSize; }
        void At(float x, float y, DripSim::Sample* out) const override
        {
            const auto s = m_field.At(x, y);
            *out = DripSim::Sample{ s.dx, s.dy, s.steep, s.mmpx, s.up, s.inner ? 1 : 0 };
        }
        bool Inner(std::int32_t x, std::int32_t y) const override { return m_field.Inner(x, y); }
        std::int32_t TriAt(std::int32_t x, std::int32_t y) const override { return m_field.TriAt(x, y); }
        float CenterZ(std::int32_t tri) const override { return m_field.CenterZ(tri); }
        float NormalY(std::int32_t tri) const override { return m_field.NormalY(tri); }
        float RestSteep(std::int32_t tri) const override { return m_field.RestSteep(tri); }
        float ZMin() const override { return m_field.ZMin(); }
        float ZMax() const override { return m_field.ZMax(); }

    private:
        const DS::Field::Instance& m_field;
    };

    class PreviewWorld final : public DripSim::IWorld
    {
    public:
        float rain = 0.9f;
        float exertion = 0.0f;
        float Exertion() const override { return exertion; }
        float RainIntensity() const override { return rain; }
        bool IsInterior() const override { return false; }
        bool SkyExposed() const override { return true; }
        const char* WeatherName() const override { return "preview"; }
        float WaterDepth() const override { return 0.0f; }
        bool IsSwimming() const override { return false; }
    };

    class Host final : public DripSim::IHost
    {
    public:
        Host(DS::Film::Layer* layer, const FieldView* field, const PreviewWorld* world) : m_layer(layer), m_field(field), m_world(world) {}
        bool Raycast(const float from[3], const float dir[3], float maxDist, DripSim::Hit* out) const override
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
        bool BodyBounds(float lo[3], float hi[3]) const override { return DS::Field::Get(DS::Surface::Body).Bounds(lo, hi); }
        DripSim::ILayer* Layer() override { return m_layer; }
        const DripSim::IField* Field() const override { return m_field; }
        const DripSim::IWorld* World() const override { return m_world; }
        void Notify(const char* text) override { std::printf("[mode] %s\n", text); }
        void Finish() override { finished = true; }
        std::int32_t SurfaceCount() const override { return 1; }
        const char* SurfaceName(std::int32_t) const override { return "body"; }
        DripSim::ILayer* SurfaceLayer(std::int32_t) override { return m_layer; }
        const DripSim::IField* SurfaceField(std::int32_t) const override { return m_field; }
        bool finished = false;

    private:
        DS::Film::Layer* m_layer;
        const FieldView* m_field;
        const PreviewWorld* m_world;
    };

    // ------------------------------------------------------------------ PNG (stored deflate blocks: big but trivial)
    std::uint32_t Crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0)
    {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t n = 0; n < 256; ++n) {
                std::uint32_t c = n;
                for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[n] = c;
            }
            return t;
        }();
        crc = ~crc;
        for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return ~crc;
    }

    void WritePng(const std::string& path, const std::vector<std::uint8_t>& rgb, int width, int height)
    {
        std::vector<std::uint8_t> raw;
        raw.reserve(static_cast<std::size_t>(width * 3 + 1) * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y) {
            raw.push_back(0);
            raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y) * width * 3, rgb.begin() + static_cast<std::ptrdiff_t>(y + 1) * width * 3);
        }
        std::vector<std::uint8_t> z{ 0x78, 0x01 };
        for (std::size_t pos = 0; pos < raw.size();) {
            const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
            z.push_back(pos + n >= raw.size() ? 1 : 0);
            z.push_back(static_cast<std::uint8_t>(n & 0xFF));
            z.push_back(static_cast<std::uint8_t>(n >> 8));
            z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
            z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
            z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
            pos += n;
        }
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (const auto c : raw) {
            a = (a + c) % 65521u;
            b = (b + a) % 65521u;
        }
        const std::uint32_t adler = (b << 16) | a;
        for (int shift = 24; shift >= 0; shift -= 8) z.push_back(static_cast<std::uint8_t>(adler >> shift));

        std::ofstream f(path, std::ios::binary);
        const auto be32 = [&](std::uint32_t v) {
            const std::uint8_t bytes[4] = { static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v) };
            f.write(reinterpret_cast<const char*>(bytes), 4);
        };
        const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
            be32(static_cast<std::uint32_t>(data.size()));
            std::vector<std::uint8_t> body(type, type + 4);
            body.insert(body.end(), data.begin(), data.end());
            f.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
            be32(Crc32(body.data(), body.size()));
        };
        const std::uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        f.write(reinterpret_cast<const char*>(signature), 8);
        std::vector<std::uint8_t> header;
        for (const auto v : { static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) }) {
            header.insert(header.end(), { static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v) });
        }
        header.insert(header.end(), { 8, 2, 0, 0, 0 });
        chunk("IHDR", header);
        chunk("IDAT", z);
        chunk("IEND", {});
    }

    // ------------------------------------------------------------------ the picture: the film over a shaded body
    struct View
    {
        int width = 900;
        int height = 900;
        float zLow = 0.0f;   // fractions of the body height
        float zHigh = 1.0f;
        float yawDegrees = 0.0f;
        float skin[3] = { 214.0f, 160.0f, 140.0f };  // the stand-in skin colour (--skin r,g,b)
    };

    struct Vec3
    {
        float x, y, z;
    };

    Vec3 Normalize(Vec3 v)
    {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 1e-9f ? Vec3{ v.x / l, v.y / l, v.z / l } : Vec3{ 0, 0, 1 };
    }

    float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    std::vector<std::uint8_t> RenderBody(const DS::Body::Static& body, const DS::Film::Layer& layer, const std::uint32_t* film, const std::uint32_t* filmNormals, const View& view, float zMin, float zMax)
    {
        const std::size_t nv = body.vertexCount;
        std::vector<Vec3> normal(nv, Vec3{ 0, 0, 0 });
        for (std::uint32_t t = 0; t < body.triangleCount; ++t) {
            const std::uint16_t i0 = body.triangle[t * 3];
            const std::uint16_t i1 = body.triangle[t * 3 + 1];
            const std::uint16_t i2 = body.triangle[t * 3 + 2];
            const float* p0 = &body.position[static_cast<std::size_t>(i0) * 3];
            const float* p1 = &body.position[static_cast<std::size_t>(i1) * 3];
            const float* p2 = &body.position[static_cast<std::size_t>(i2) * 3];
            const Vec3 e1{ p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
            const Vec3 e2{ p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
            const Vec3 n{ e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
            for (const auto i : { i0, i1, i2 }) {
                normal[i].x += n.x;
                normal[i].y += n.y;
                normal[i].z += n.z;
            }
        }
        for (auto& n : normal) n = Normalize(n);

        // orthographic: the camera looks at the body from +y (the front) after turning it by the yaw; screen right = -x
        const float yaw = view.yawDegrees * 3.14159265f / 180.0f;
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);
        const float zLow = zMin + view.zLow * (zMax - zMin);
        const float zHigh = zMin + view.zHigh * (zMax - zMin);
        const float scale = static_cast<float>(view.height) / std::max(zHigh - zLow, 1.0f);
        const auto toScreen = [&](const float* p, float& sx, float& sy_, float& depth) {
            const float xr = p[0] * cy - p[1] * sy;
            const float yr = p[0] * sy + p[1] * cy;
            sx = static_cast<float>(view.width) * 0.5f - xr * scale;
            sy_ = static_cast<float>(view.height) - (p[2] - zLow) * scale;
            depth = yr;
        };
        const Vec3 light = Normalize({ -0.45f, 0.55f, 0.7f });
        const Vec3 half = Normalize({ light.x, light.y + 1.0f, light.z });  // the camera sits on +y

        std::vector<float> zbuffer(static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.height), -1e9f);
        std::vector<std::uint8_t> image(zbuffer.size() * 3, 0);
        for (std::size_t i = 0; i < zbuffer.size(); ++i) {  // a dark backdrop
            image[i * 3] = 14;
            image[i * 3 + 1] = 15;
            image[i * 3 + 2] = 18;
        }
        const auto& look = layer.GetLook();
        const float alphaScale = layer.Alpha();
        for (std::uint32_t t = 0; t < body.triangleCount; ++t) {
            const std::uint16_t idx[3] = { body.triangle[t * 3], body.triangle[t * 3 + 1], body.triangle[t * 3 + 2] };
            float sx[3];
            float sy_[3];
            float depth[3];
            for (int k = 0; k < 3; ++k) toScreen(&body.position[static_cast<std::size_t>(idx[k]) * 3], sx[k], sy_[k], depth[k]);
            const float area = (sx[1] - sx[0]) * (sy_[2] - sy_[0]) - (sx[2] - sx[0]) * (sy_[1] - sy_[0]);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ sx[0], sx[1], sx[2] }))));
            const int x1 = std::min(view.width - 1, static_cast<int>(std::ceil(std::max({ sx[0], sx[1], sx[2] }))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ sy_[0], sy_[1], sy_[2] }))));
            const int y1 = std::min(view.height - 1, static_cast<int>(std::ceil(std::max({ sy_[0], sy_[1], sy_[2] }))));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float py = static_cast<float>(y) + 0.5f;
                    const float w0 = ((sx[1] - px) * (sy_[2] - py) - (sx[2] - px) * (sy_[1] - py)) / area;
                    const float w1 = ((sx[2] - px) * (sy_[0] - py) - (sx[0] - px) * (sy_[2] - py)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    const float d = w0 * depth[0] + w1 * depth[1] + w2 * depth[2];
                    const auto zi = static_cast<std::size_t>(y) * static_cast<std::size_t>(view.width) + static_cast<std::size_t>(x);
                    if (d <= zbuffer[zi]) continue;
                    Vec3 n{ w0 * normal[idx[0]].x + w1 * normal[idx[1]].x + w2 * normal[idx[2]].x, w0 * normal[idx[0]].y + w1 * normal[idx[1]].y + w2 * normal[idx[2]].y,
                        w0 * normal[idx[0]].z + w1 * normal[idx[1]].z + w2 * normal[idx[2]].z };
                    n = Normalize(n);
                    const Vec3 nr{ n.x * cy - n.y * sy, n.x * sy + n.y * cy, n.z };  // the normal turned with the body
                    if (nr.y <= 0.0f) continue;                                       // back-facing
                    zbuffer[zi] = d;
                    const float u = w0 * body.uv[static_cast<std::size_t>(idx[0]) * 2] + w1 * body.uv[static_cast<std::size_t>(idx[1]) * 2] + w2 * body.uv[static_cast<std::size_t>(idx[2]) * 2];
                    const float v = w0 * body.uv[static_cast<std::size_t>(idx[0]) * 2 + 1] + w1 * body.uv[static_cast<std::size_t>(idx[1]) * 2 + 1] + w2 * body.uv[static_cast<std::size_t>(idx[2]) * 2 + 1];
                    const float lambert = 0.28f + 0.72f * std::max(0.0f, Dot(nr, light));
                    float skin[3] = { view.skin[0] * lambert, view.skin[1] * lambert, view.skin[2] * lambert };
                    // the film at this texel (bilinear)
                    constexpr float film_size = static_cast<float>(DS::Film::kSize);
                    const float fx = std::clamp(u * film_size - 0.5f, 0.0f, film_size - 1.01f);
                    const float fy = std::clamp(v * film_size - 0.5f, 0.0f, film_size - 1.01f);
                    const int ix = static_cast<int>(fx);
                    const int iy = static_cast<int>(fy);
                    const float tx = fx - static_cast<float>(ix);
                    const float ty = fy - static_cast<float>(iy);
                    float c[4] = { 0, 0, 0, 0 };  // premultiplied r g b a
                    for (int j = 0; j < 4; ++j) {
                        const std::uint32_t p = film[static_cast<std::size_t>(iy + (j >> 1)) * DS::Film::kSize + static_cast<std::size_t>(ix + (j & 1))];
                        const float wgt = ((j & 1) ? tx : 1.0f - tx) * ((j >> 1) ? ty : 1.0f - ty);
                        const float a = static_cast<float>(p >> 24) / 255.0f;
                        c[0] += wgt * a * static_cast<float>(p & 0xFF);
                        c[1] += wgt * a * static_cast<float>((p >> 8) & 0xFF);
                        c[2] += wgt * a * static_cast<float>((p >> 16) & 0xFF);
                        c[3] += wgt * a;
                    }
                    const float a = c[3] * alphaScale;
                    float out[3] = { skin[0], skin[1], skin[2] };
                    if (a > 1e-4f) {
                        const float inv = alphaScale / std::max(c[3] * alphaScale, 1e-4f);
                        Vec3 nf = nr;  // the shading normal of the film: its normal map when there is one
                        if (filmNormals) {
                            const std::uint32_t q = filmNormals[static_cast<std::size_t>(iy + (ty > 0.5f ? 1 : 0)) * DS::Film::kSize + static_cast<std::size_t>(ix + (tx > 0.5f ? 1 : 0))];
                            const Vec3 ms{ static_cast<float>(q & 0xFF) / 127.5f - 1.0f, static_cast<float>((q >> 16) & 0xFF) / 127.5f - 1.0f, static_cast<float>((q >> 8) & 0xFF) / 127.5f - 1.0f };  // R = x, B = y, G = z
                            nf = Normalize({ ms.x * cy - ms.y * sy, ms.x * sy + ms.y * cy, ms.z });
                        }
                        const float lambertFilm = 0.28f + 0.72f * std::max(0.0f, Dot(nf, light));
                        float spec = 0.0f;
                        if (look.specScale > 0.0f && look.specPower > 0.0f) spec = std::pow(std::max(0.0f, Dot(nf, half)), look.specPower) * look.specScale * 0.35f;
                        for (int k = 0; k < 3; ++k) {
                            const float filmColour = c[k] / std::max(c[3], 1e-4f) * (0.40f + 0.60f * lambertFilm);
                            out[k] = skin[k] * (1.0f - a) + filmColour * a + 255.0f * spec * std::min(1.0f, a * 3.0f) * inv * a;
                        }
                    }
                    for (int k = 0; k < 3; ++k) image[zi * 3 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::clamp(out[k], 0.0f, 255.0f));
                }
            }
        }
        return image;
    }

    // The modes by name (nullptr = unknown; a public build only knows "rain"), and the INI each mode reads.
    DripSim::IMode* ModeByName(const std::string& name)
    {
        return name == "rain" ? RainModeInstance() : nullptr;
    }

    std::string IniName(const std::string& mode)
    {
        return "DripSimRain";
    }

    // ------------------------------------------------------------------ command line
    struct Args
    {
        std::string pack = "body_pack.bin";
        std::string mode = "rain";
        std::string out = "preview_out";
        std::vector<float> at{ 1.0f, 3.0f, 6.0f };
        float seconds = 0.0f;
        std::string viewName = "belly";
        View view;
        std::string ini;
        int variant = 0;
        float rain = 0.9f;
        bool alsoRain = false;  // a second layer with the rain mode on top of the main mode
        float soak = 0.0f;  // rain mode: a soak (water to this many game units; 1000 = all of the body) instead of rain
        int seed = 7;
        bool atlas = false;
    };

    std::vector<std::string> Split(const std::string& text, char sep)
    {
        std::vector<std::string> parts;
        std::string cur;
        for (const char c : text) {
            if (c == sep) {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        parts.push_back(cur);
        return parts;
    }

    bool ParseArgs(int argc, char** argv, Args& a)
    {
        for (int i = 1; i < argc; ++i) {
            const std::string k = argv[i];
            const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
            if (k == "--pack") a.pack = next();
            else if (k == "--mode") a.mode = next();
            else if (k == "--out") a.out = next();
            else if (k == "--at") {
                a.at.clear();
                for (const auto& s : Split(next(), ',')) a.at.push_back(std::stof(s));
            } else if (k == "--seconds") a.seconds = std::stof(next());
            else if (k == "--view") a.viewName = next();
            else if (k == "--yaw") a.view.yawDegrees = std::stof(next());
            else if (k == "--size") {
                const auto p = Split(next(), 'x');
                if (p.size() == 2) {
                    a.view.width = std::stoi(p[0]);
                    a.view.height = std::stoi(p[1]);
                }
            } else if (k == "--ini") a.ini = next();
            else if (k == "--variant") a.variant = std::stoi(next());
            else if (k == "--rain") a.rain = std::stof(next());
            else if (k == "--soak") a.soak = std::stof(next());
            else if (k == "--also-rain") a.alsoRain = true;
            else if (k == "--skin") {
                const auto p = Split(next(), ',');
                if (p.size() == 3) {
                    for (int c = 0; c < 3; ++c) a.view.skin[c] = std::stof(p[static_cast<std::size_t>(c)]);
                }
            }
            else if (k == "--seed") a.seed = std::stoi(next());
            else if (k == "--atlas") a.atlas = true;
            else if (k == "--verbose") logger::verbose = true;
            else {
                std::printf("unknown option %s (see the header of Preview.cpp)\n", k.c_str());
                return false;
            }
        }
        return ModeByName(a.mode) != nullptr;
    }

    void SetView(Args& a)
    {
        const auto& n = a.viewName;
        if (n == "full") a.view.zLow = 0.0f, a.view.zHigh = 1.0f;
        else if (n == "torso") a.view.zLow = 0.5f, a.view.zHigh = 0.95f;
        else if (n == "belly") a.view.zLow = 0.52f, a.view.zHigh = 0.78f;
        else if (n == "chest") a.view.zLow = 0.7f, a.view.zHigh = 0.92f;
        else if (n == "thighs") a.view.zLow = 0.2f, a.view.zHigh = 0.43f;
        else {
            const auto p = Split(n, ',');
            if (p.size() == 2) a.view.zLow = std::stof(p[0]), a.view.zHigh = std::stof(p[1]);
        }
    }
}

int main(int argc, char** argv)
{
    Args args;
    if (!ParseArgs(argc, argv, args)) return 2;
    SetView(args);
    std::sort(args.at.begin(), args.at.end());
    const float runFor = std::max(args.seconds, args.at.empty() ? 0.0f : args.at.back());

    const auto body = LoadPack(args.pack);
    if (!body) {
        std::printf("cannot read the body pack '%s' (python plugin/tools/preview/export_pack.py)\n", args.pack.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(args.out) / "Data" / "SKSE" / "Plugins", ec);
    const std::string iniName = IniName(args.mode);
    {  // the mode reads Data\SKSE\Plugins\<name>.ini relative to the current directory: give it one with the requested settings
        std::ofstream ini(std::filesystem::path(args.out) / "Data" / "SKSE" / "Plugins" / (iniName + ".ini"));
        if (args.mode == "rain") ini << "ForcedIntensity=" << args.rain << "\n";
        if (args.mode == "rain" && args.soak > 0.0f) ini << "EnableSoak=1\n";
        for (const auto& kv : Split(args.ini, ',')) {
            if (!kv.empty()) ini << kv << "\n";
        }
    }
    if (args.alsoRain && args.mode != "rain") {
        std::ofstream rainIni(std::filesystem::path(args.out) / "Data" / "SKSE" / "Plugins" / "DripSimRain.ini");
        rainIni << "ForcedIntensity=" << args.rain << "\n";
    }
    std::filesystem::current_path(args.out, ec);

    auto& field = DS::Field::Get(DS::Surface::Body);
    if (!field.Build(body)) {
        std::printf("the field could not be built\n");
        return 1;
    }
    field.Update();
    const FieldView fieldView(field);
    PreviewWorld world;
    world.rain = args.rain;
    DS::Film::SetNormalMapsEnabled(true);
    auto* layer = DS::Film::Acquire(DS::Surface::Body, field.KeepMask().data());
    Host host(layer, &fieldView, &world);
    DripSim::IMode* mode = ModeByName(args.mode);
    if (args.mode == "rain" && args.soak > 0.0f) {
        world.rain = 0.0f;
        RainLoadSettings();  // (the soak request is ignored unless EnableSoak=1 is loaded)
        RainRequestSoak(1.0f, args.soak);
    }
    const std::int32_t variant = args.soak > 0.0f ? 2 : 1;
    if (!mode->Start(&host, variant)) {
        std::printf("the mode declined to start\n");
        return 1;
    }
    // stacking test: the rain mode in a second layer on top of the main mode
    DS::Film::Layer* layer2 = args.alsoRain && args.mode != "rain" ? DS::Film::Acquire(DS::Surface::Body, field.KeepMask().data()) : nullptr;
    Host host2(layer2, &fieldView, &world);
    DripSim::IMode* mode2 = layer2 ? RainModeInstance() : nullptr;
    if (mode2 && !mode2->Start(&host2, 1)) {
        std::printf("the second (rain) mode declined to start\n");
        return 1;
    }

    constexpr float dt = 1.0f / 30.0f;
    std::size_t nextShot = 0;
    const auto begin = std::chrono::steady_clock::now();
    double renderMs = 0;
    double modeMs = 0;
    int frames = 0;
    for (float t = 0.0f; t <= runFor + dt * 0.5f; t += dt, ++frames) {
        const auto m0 = std::chrono::steady_clock::now();
        DS::Film::BeginFrame(DS::Surface::Body);
        mode->Update(&host, dt);
        if (mode2) mode2->Update(&host2, dt);
        DS::Film::EndFrame(DS::Surface::Body);
        const auto m1 = std::chrono::steady_clock::now();
        DS::Film::Render(DS::Surface::Body);
        const auto m2 = std::chrono::steady_clock::now();
        modeMs += std::chrono::duration<double, std::milli>(m1 - m0).count();
        renderMs += std::chrono::duration<double, std::milli>(m2 - m1).count();
        while (nextShot < args.at.size() && t + dt * 0.5f >= args.at[nextShot]) {
            const auto name = std::format("{}_{}_t{:04.1f}.png", args.mode, args.viewName, args.at[nextShot]);
            const auto pixels = RenderBody(*body, layer2 ? *layer2 : *layer, DS::Film::Pixels(DS::Surface::Body), DS::Film::NormalPixels(DS::Surface::Body), args.view, field.ZMin() - 10.0f, field.ZMax() + 10.0f);
            WritePng(name, pixels, args.view.width, args.view.height);
            if (args.atlas) {
                constexpr int kAtlas = DS::Film::kSize;
                std::vector<std::uint8_t> atlas(static_cast<std::size_t>(kAtlas) * kAtlas * 3);
                const auto* film = DS::Film::Pixels(DS::Surface::Body);
                for (std::size_t i = 0; i < static_cast<std::size_t>(kAtlas) * kAtlas; ++i) {
                    const float a = static_cast<float>(film[i] >> 24) / 255.0f;
                    atlas[i * 3] = static_cast<std::uint8_t>(20.0f * (1 - a) + static_cast<float>(film[i] & 0xFF) * a);
                    atlas[i * 3 + 1] = static_cast<std::uint8_t>(20.0f * (1 - a) + static_cast<float>((film[i] >> 8) & 0xFF) * a);
                    atlas[i * 3 + 2] = static_cast<std::uint8_t>(20.0f * (1 - a) + static_cast<float>((film[i] >> 16) & 0xFF) * a);
                }
                WritePng(std::format("{}_atlas_t{:04.1f}.png", args.mode, args.at[nextShot]), atlas, kAtlas, kAtlas);
            }
            std::printf("t=%.1fs -> %s\n", args.at[nextShot], name.c_str());
            ++nextShot;
        }
        if (host.finished) break;
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("simulated %d frames (%.1f s) in %.1f s; mode %.2f ms/frame, film render %.2f ms/frame (on this machine)\n", frames, static_cast<double>(frames) * dt, total,
        modeMs / std::max(frames, 1), renderMs / std::max(frames, 1));
    std::printf("layer tiles rendered per frame: %.1f (of %d)\n", static_cast<double>(DS::Film::TilesRendered()) / std::max(frames, 1), DS::Film::kTiles * DS::Film::kTiles);
    if (mode2) mode2->Stop(&host2, DripSim::StopReason::Requested);
    mode->Stop(&host, DripSim::StopReason::Requested);
    return 0;
}
