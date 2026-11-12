// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Spike.h"

#include "Body.h"
#include "LogLimit.h"

// Capture of a skin surface (see Body.h). The vertex layout is decoded from the vertex description, so any skinned layout that carries position,
// UV and skin weights works (body: stride 32, feet: stride 40 with normals and tangents); several skin partitions are merged through their vertex maps.
// The head is a BSDynamicTriShape: its static buffer has only UV + skin (stride 16, no position flag), the morphed positions are the CPU array
// `dynamicData` (16 bytes per vertex: x y z + a w).
//   vertex desc: bits 0-3 vertex size in dwords, then 4 bits each, in dwords: position 4, uv 8, uv2 12, normal 16, tangent 20, color 24, skin 28;
//   flags from bit 44: 0x1 position, 0x2 uv, 0x4 uv2, 0x8 normal, 0x10 tangent, 0x20 color, 0x40 skinned.
// S4 (Ctrl+Shift+H) also writes the body's raw data to DripSim_body.bin for checking against a NIF (format: reference/read_body_dump.py).
namespace
{
    using REX::W32::D3D11_BUFFER_DESC;
    using REX::W32::D3D11_MAPPED_SUBRESOURCE;
    using REX::W32::ID3D11Buffer;

    constexpr std::uint64_t kFlagPosition = 0x1;
    constexpr std::uint64_t kFlagUv = 0x2;
    constexpr std::uint64_t kFlagSkinned = 0x40;

    std::mutex g_mutex;
    struct Pending
    {
        RE::NiPointer<RE::BSGeometry> geometry;
        bool file = false;
    };
    std::array<Pending, DS::Surface::Count> g_pending;
    std::array<std::shared_ptr<const DS::Body::Static>, DS::Surface::Count> g_static;

    float HalfToFloat(std::uint16_t h)
    {
        const std::uint32_t sign = (h >> 15) & 1u;
        const std::uint32_t exponent = (h >> 10) & 0x1Fu;
        const std::uint32_t mantissa = h & 0x3FFu;
        float value;
        if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
        else if (exponent == 31) value = mantissa ? std::nanf("") : INFINITY;
        else value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
        return sign ? -value : value;
    }

    bool HasSkinData(RE::BSGeometry* geometry)
    {
        const auto& data = geometry->GetGeometryRuntimeData();
        auto* skin = data.skinInstance.get();
        return skin && skin->skinData && skin->skinPartition && skin->bones && skin->skinPartition->numPartitions > 0 && skin->skinPartition->partitions[0].buffData;
    }

    // A skinned overlay shape of the surface ("<Prefix> [Ovl0..9]"); the body also falls back to the base shape by name.
    RE::BSGeometry* FindSurfaceGeometry(RE::NiAVObject* root, int surface)
    {
        if (!root) return nullptr;
        for (int i = 9; i >= 0; --i) {
            auto* object = root->GetObjectByName(RE::BSFixedString(std::format("{} [Ovl{}]", DS::Surface::Prefix(surface), i)));
            auto* geometry = object ? object->AsGeometry() : nullptr;
            if (geometry && HasSkinData(geometry)) return geometry;
        }
        if (surface == DS::Surface::Body) {
            for (const char* name : { "3BA", "3BBB", "3BAv", "CBBE" }) {
                auto* object = root->GetObjectByName(RE::BSFixedString(name));
                auto* geometry = object ? object->AsGeometry() : nullptr;
                if (geometry && HasSkinData(geometry)) return geometry;
            }
        }
        return nullptr;
    }

    // Copies a GPU buffer into a CPU vector through a staging buffer. Render thread only (uses the immediate context).
    bool ReadBuffer(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, ID3D11Buffer* source, std::vector<std::uint8_t>& out)
    {
        D3D11_BUFFER_DESC desc{};
        source->GetDesc(&desc);
        desc.usage = REX::W32::D3D11_USAGE_STAGING;
        desc.bindFlags = 0;
        desc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_READ;
        desc.miscFlags = 0;
        ID3D11Buffer* staging = nullptr;
        if (device->CreateBuffer(&desc, nullptr, &staging) < 0 || !staging) return false;
        context->CopyResource(staging, source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const bool ok = context->Map(staging, 0, REX::W32::D3D11_MAP_READ, 0, &mapped) >= 0 && mapped.data;
        if (ok) {
            out.assign(static_cast<const std::uint8_t*>(mapped.data), static_cast<const std::uint8_t*>(mapped.data) + desc.byteWidth);
            context->Unmap(staging, 0);
        }
        staging->Release();
        return ok;
    }

    struct Writer
    {
        std::ofstream file;
        template <class T>
        void Put(const T& value) { file.write(reinterpret_cast<const char*>(&value), sizeof(T)); }
        void Bytes(const void* data, std::size_t size) { file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)); }
        void Text(std::string_view text)
        {
            Put(static_cast<std::uint32_t>(text.size()));
            Bytes(text.data(), text.size());
        }
    };

    struct PartData
    {
        std::vector<std::uint8_t> vertices;
        std::vector<std::uint8_t> indices;
        std::uint64_t desc = 0;
    };

    void WriteDump(RE::BSGeometry* geometry, RE::NiSkinInstance* skin, RE::NiSkinData* skinData, RE::NiSkinPartition* partition, const std::vector<PartData>& parts)
    {
        auto directory = SKSE::log::log_directory();
        if (!directory) return;
        Writer out{ std::ofstream(*directory / "DripSim_body.bin", std::ios::binary | std::ios::trunc) };
        if (!out.file) {
            logger::error("S4: cannot write DripSim_body.bin");
            return;
        }
        const auto boneCount = skinData->GetBoneCount();
        out.Bytes("DSB2", 4);
        out.Put(partition->vertexCount);
        out.Put(boneCount);
        for (std::uint32_t i = 0; i < boneCount; ++i) {
            auto* bone = skin->bones[i];
            out.Text(bone && bone->name.c_str() ? bone->name.c_str() : "");
            out.Bytes(&skinData->GetBoneDataSkinToBone(i), sizeof(RE::NiTransform));  // skin -> bone (bind pose)
            RE::NiTransform world = bone ? bone->world : RE::NiTransform();            // the bone's world transform right now
            out.Bytes(&world, sizeof world);
        }
        RE::NiTransform root = skin->rootParent ? skin->rootParent->world : RE::NiTransform();
        out.Bytes(&root, sizeof root);
        RE::NiTransform shape = geometry->world;
        out.Bytes(&shape, sizeof shape);
        out.Put(partition->numPartitions);
        for (std::uint32_t p = 0; p < partition->numPartitions; ++p) {
            const auto& part = partition->partitions[p];
            out.Put(part.vertices);
            out.Put(part.triangles);
            out.Put(part.numBones);
            out.Put(part.bonesPerVertex);
            out.Bytes(part.bones, static_cast<std::size_t>(part.numBones) * sizeof(std::uint16_t));
            if (part.vertexMap) out.Bytes(part.vertexMap, static_cast<std::size_t>(part.vertices) * sizeof(std::uint16_t));
            else for (std::uint16_t v = 0; v < part.vertices; ++v) out.Put(v);
            out.Put(parts[p].desc);
            out.Put(static_cast<std::uint32_t>(parts[p].vertices.size()));
            out.Put(static_cast<std::uint32_t>(parts[p].indices.size()));
            out.Bytes(parts[p].vertices.data(), parts[p].vertices.size());
            out.Bytes(parts[p].indices.data(), parts[p].indices.size());
        }
        out.file.flush();
        logger::info("S4: wrote DripSim_body.bin ({} bytes)", static_cast<std::size_t>(out.file.tellp()));
        if (auto* console = RE::ConsoleLog::GetSingleton()) console->Print("DripSim S4: body skin data written to DripSim_body.bin");
    }

    void Capture(int surface, RE::BSGeometry* geometry, bool writeFile)
    {
        const char* label = DS::Surface::Name(surface);
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        auto* device = RE::BSGraphics::Renderer::GetDevice();
        auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
        const auto& data = geometry->GetGeometryRuntimeData();
        auto* skin = data.skinInstance.get();
        auto* skinData = skin ? skin->skinData.get() : nullptr;
        auto* partition = skin ? skin->skinPartition.get() : nullptr;
        if (!device || !context || !skinData || !partition || !skin->bones) {
            logger::warn("Capture {}: missing data (device {}, context {}, skinData {}, partition {}, bones {})", label, device != nullptr, context != nullptr, skinData != nullptr,
                partition != nullptr, skin && skin->bones);
            return;
        }
        std::vector<PartData> parts(partition->numPartitions);
        for (std::uint32_t p = 0; p < partition->numPartitions; ++p) {
            const auto& part = partition->partitions[p];
            auto* buffers = part.buffData;
            if (!buffers || !buffers->vertexBuffer || !buffers->indexBuffer ||
                !ReadBuffer(device, context, reinterpret_cast<ID3D11Buffer*>(buffers->vertexBuffer), parts[p].vertices) ||
                !ReadBuffer(device, context, reinterpret_cast<ID3D11Buffer*>(buffers->indexBuffer), parts[p].indices)) {
                logger::error("Capture {}: partition {} has no readable GPU buffers (buffData {})", label, p, buffers != nullptr);
                return;
            }
            std::memcpy(&parts[p].desc, &buffers->vertexDesc, std::min(sizeof parts[p].desc, sizeof buffers->vertexDesc));
            if (DS::LogAllow("capture-partition")) logger::info("Capture {}: partition {}: vertices={} triangles={} bones={} perVertex={} vertexBytes={} indexBytes={} desc={:#x}", label, p, part.vertices, part.triangles,
                part.numBones, part.bonesPerVertex, parts[p].vertices.size(), parts[p].indices.size(), parts[p].desc);
        }
        if (writeFile && surface == DS::Surface::Body) WriteDump(geometry, skin, skinData, partition, parts);

        // ---- decode into one static record
        auto body = std::make_shared<DS::Body::Static>();
        body->surface = surface;
        body->shape.reset(geometry);
        body->vertexCount = partition->vertexCount;
        body->boneCount = skinData->GetBoneCount();
        body->boneRefs.reserve(body->boneCount);
        for (std::uint32_t i = 0; i < body->boneCount; ++i) body->boneRefs.emplace_back(skin->bones[i]);  // a reference each (see Body.h)
        const std::size_t nv = body->vertexCount;
        body->position.assign(nv * 3, 0.0f);
        body->uv.assign(nv * 2, 0.0f);
        body->weight.assign(nv * 4, 0.0f);
        body->boneIndex.assign(nv * 4, 0);
        const float* dynamicPositions = nullptr;  // head: x y z w per vertex
        std::size_t dynamicCount = 0;
        if (auto* dynamic = geometry->AsDynamicTriShape()) {
            auto& runtime = dynamic->GetDynamicTrishapeRuntimeData();
            dynamicPositions = static_cast<const float*>(runtime.dynamicData);
            dynamicCount = runtime.dataSize / 16;
        }
        for (std::uint32_t p = 0; p < partition->numPartitions; ++p) {
            const auto& part = partition->partitions[p];
            const auto desc = parts[p].desc;
            const std::uint64_t flags = (desc >> 44) & 0x3FF;
            const std::size_t stride = (desc & 0xF) * 4;
            const bool staticPosition = (flags & kFlagPosition) != 0;
            if ((!staticPosition && (!dynamicPositions || dynamicCount < nv)) || !(flags & kFlagUv) || !(flags & kFlagSkinned) || stride < 16) {
                logger::warn("Capture {}: partition {} layout not usable (flags {:#x}, stride {}); the surface is unavailable", label, p, flags, stride);
                return;
            }
            const std::size_t posOff = ((desc >> 4) & 0xF) * 4;
            const std::size_t uvOff = ((desc >> 8) & 0xF) * 4;
            const std::size_t skinOff = ((desc >> 28) & 0xF) * 4;
            if (skinOff + 12 > stride || uvOff + 4 > stride || (staticPosition && posOff + 12 > stride)) {
                logger::warn("Capture {}: partition {} attribute offsets do not fit the stride {}; the surface is unavailable", label, p, stride);
                return;
            }
            if (parts[p].vertices.size() < static_cast<std::size_t>(part.vertices) * stride || (partition->numPartitions > 1 && !part.vertexMap)) {
                logger::warn("Capture {}: partition {} buffer too small or no vertex map; the surface is unavailable", label, p);
                return;
            }
            for (std::uint32_t j = 0; j < part.vertices; ++j) {
                const std::size_t gid = part.vertexMap ? part.vertexMap[j] : j;
                if (gid >= nv) continue;
                const std::uint8_t* v = parts[p].vertices.data() + static_cast<std::size_t>(j) * stride;
                if (staticPosition) std::memcpy(&body->position[gid * 3], v + posOff, 12);
                else std::memcpy(&body->position[gid * 3], dynamicPositions + gid * 4, 12);
                std::uint16_t uv[2];
                std::memcpy(uv, v + uvOff, 4);
                body->uv[gid * 2] = HalfToFloat(uv[0]);
                body->uv[gid * 2 + 1] = HalfToFloat(uv[1]);
                std::uint16_t w[4];
                std::memcpy(w, v + skinOff, 8);
                for (int k = 0; k < 4; ++k) {
                    body->weight[gid * 4 + static_cast<std::size_t>(k)] = HalfToFloat(w[k]);
                    const std::uint8_t local = v[skinOff + 8 + static_cast<std::size_t>(k)];
                    body->boneIndex[gid * 4 + static_cast<std::size_t>(k)] = part.bones && local < part.numBones ? static_cast<std::uint8_t>(part.bones[local]) : local;  // palette -> skin bone
                }
            }
            const auto* indices = reinterpret_cast<const std::uint16_t*>(parts[p].indices.data());
            const std::size_t count = std::min<std::size_t>(part.triangles, parts[p].indices.size() / 6);
            for (std::size_t t = 0; t < count; ++t) {
                for (int k = 0; k < 3; ++k) {
                    const std::size_t local = indices[t * 3 + static_cast<std::size_t>(k)];
                    body->triangle.push_back(static_cast<std::uint16_t>(part.vertexMap && local < part.vertices ? part.vertexMap[local] : local));
                }
            }
        }
        body->triangleCount = static_cast<std::uint32_t>(body->triangle.size() / 3);
        body->skinToBoneRotation.resize(static_cast<std::size_t>(body->boneCount) * 9);
        body->skinToBoneTranslate.resize(static_cast<std::size_t>(body->boneCount) * 3);
        body->skinToBoneScale.resize(body->boneCount);
        for (std::uint32_t b = 0; b < body->boneCount; ++b) {
            const auto& transform = skinData->GetBoneDataSkinToBone(b);
            body->skinToBoneTranslate[static_cast<std::size_t>(b) * 3] = transform.translate.x;
            body->skinToBoneTranslate[static_cast<std::size_t>(b) * 3 + 1] = transform.translate.y;
            body->skinToBoneTranslate[static_cast<std::size_t>(b) * 3 + 2] = transform.translate.z;
            body->skinToBoneScale[b] = transform.scale;
            const auto& rotate = transform.rotate;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) body->skinToBoneRotation[static_cast<std::size_t>(b) * 9 + static_cast<std::size_t>(r * 3 + c)] = rotate.entry[r][c];
            }
        }
        if (DS::LogAllow("capture-shape")) logger::info("Capture {}: shape '{}' {} vertices, {} triangles, {} bones, {} partitions", label, geometry->name.c_str(), body->vertexCount, body->triangleCount, body->boneCount,
            partition->numPartitions);
        std::scoped_lock lock(g_mutex);
        g_static[static_cast<std::size_t>(surface)] = std::move(body);
    }
}

namespace DS::Body
{
    std::shared_ptr<const Static> Get(int surface)
    {
        if (surface < 0 || surface >= DS::Surface::Count) return nullptr;
        std::scoped_lock lock(g_mutex);
        return g_static[static_cast<std::size_t>(surface)];
    }

    void Request(int surface, bool writeFile)
    {
        if (surface < 0 || surface >= DS::Surface::Count) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root = player && player->Is3DLoaded() ? player->Get3D(false) : nullptr;
        auto* geometry = FindSurfaceGeometry(root, surface);
        if (!geometry) {
            logger::warn("Capture {}: no skinned '{} [Ovl#]' shape found", DS::Surface::Name(surface), DS::Surface::Prefix(surface));
            return;
        }
        std::scoped_lock lock(g_mutex);
        g_pending[static_cast<std::size_t>(surface)].geometry.reset(geometry);
        g_pending[static_cast<std::size_t>(surface)].file = writeFile;
        if (DS::LogAllow("capture-request")) logger::info("Capture {}: requested from '{}'", DS::Surface::Name(surface), geometry->name.c_str());
    }
}

namespace DS::Spike
{
    void RequestBodyDump()
    {
        DS::Body::Request(DS::Surface::Body, true);
    }

    // Render thread, just before Present.
    void BodyRenderWork()
    {
        for (int surface = 0; surface < DS::Surface::Count; ++surface) {
            RE::NiPointer<RE::BSGeometry> geometry;
            bool file = false;
            {
                std::scoped_lock lock(g_mutex);
                auto& pending = g_pending[static_cast<std::size_t>(surface)];
                geometry = std::move(pending.geometry);
                file = pending.file;
            }
            if (geometry) Capture(surface, geometry.get(), file);
        }
    }
}
