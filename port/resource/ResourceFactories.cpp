// G-Diffuser — resource factory registration.
// Registers the Fast3D and generic factories libultraship provides, so the .o2r entries
// Torch produced (textures, vertices, display lists, matrices, lights, blobs) load as
// IResource objects. Types without a factory here cannot be served by LoadResource() at
// all -- see GDiffuser_LoadArchiveFileBytes in port/AssetLoader.cpp for the raw-bytes
// escape hatch those callers use.

#include "resource/ResourceFactories.h"

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/ResourceLoader.h"
#include "ship/resource/ResourceFactoryBinary.h"
#include "ship/resource/ResourceType.h"
#include "ship/resource/File.h"
#include "ship/resource/factory/BlobFactory.h"

#include "fast/resource/ResourceType.h"
#include "fast/resource/factory/TextureFactory.h"
#include "fast/resource/factory/VertexFactory.h"
#include "fast/resource/factory/DisplayListFactory.h"
#include "fast/resource/factory/MatrixFactory.h"
#include "fast/resource/factory/LightFactory.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

constexpr uint32_t kResourceTypeGenericArray = 0x47415252; // GARR

class GenericArrayResource final : public Ship::Resource<void> {
  public:
    using Resource::Resource;

    void* GetPointer() override {
        return Data.empty() ? nullptr : Data.data();
    }

    size_t GetPointerSize() override {
        return Data.size();
    }

    std::vector<uint8_t> Data;
};

static void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

template <typename T>
static void AppendValue(std::vector<uint8_t>& out, T value) {
    AppendBytes(out, &value, sizeof(value));
}

class ResourceFactoryBinaryGenericArrayV0 final : public Ship::ResourceFactoryBinary {
  public:
    std::shared_ptr<Ship::IResource> ReadResource(std::shared_ptr<Ship::File> file,
                                                  std::shared_ptr<Ship::ResourceInitData> initData) override {
        if (!FileHasValidFormatAndReader(file, initData)) {
            return nullptr;
        }

        auto array = std::make_shared<GenericArrayResource>(initData);
        auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);

        const uint32_t arrayType = reader->ReadUInt32();
        const uint32_t count = reader->ReadUInt32();

        for (uint32_t i = 0; i < count; i++) {
            switch (arrayType) {
                case 0: AppendValue<uint8_t>(array->Data, reader->ReadUByte()); break;  // u8
                case 1: AppendValue<int8_t>(array->Data, reader->ReadInt8()); break;    // s8
                case 2: AppendValue<uint16_t>(array->Data, reader->ReadUInt16()); break; // u16
                case 3: AppendValue<int16_t>(array->Data, reader->ReadInt16()); break;  // s16
                case 4: AppendValue<uint32_t>(array->Data, reader->ReadUInt32()); break; // u32
                case 5: AppendValue<int32_t>(array->Data, reader->ReadInt32()); break;  // s32
                case 6: AppendValue<uint64_t>(array->Data, reader->ReadUInt64()); break; // u64
                case 7: AppendValue<float>(array->Data, reader->ReadFloat()); break;    // f32
                case 8: AppendValue<double>(array->Data, reader->ReadDouble()); break;  // f64
                case 9: { // Vec2f
                    const float v[2] = { reader->ReadFloat(), reader->ReadFloat() };
                    AppendBytes(array->Data, v, sizeof(v));
                    break;
                }
                case 10: { // Vec3f
                    const float v[3] = { reader->ReadFloat(), reader->ReadFloat(), reader->ReadFloat() };
                    AppendBytes(array->Data, v, sizeof(v));
                    break;
                }
                case 11: { // Vec3s
                    const int16_t v[3] = { reader->ReadInt16(), reader->ReadInt16(), reader->ReadInt16() };
                    AppendBytes(array->Data, v, sizeof(v));
                    break;
                }
                case 12: { // Vec3i
                    const int32_t v[3] = { reader->ReadInt32(), reader->ReadInt32(), reader->ReadInt32() };
                    AppendBytes(array->Data, v, sizeof(v));
                    break;
                }
                case 13: { // Vec3iu
                    const uint32_t v[3] = { reader->ReadUInt32(), reader->ReadUInt32(), reader->ReadUInt32() };
                    AppendBytes(array->Data, v, sizeof(v));
                    break;
                }
                case 14: { // Vec4f
                    const float v[4] = { reader->ReadFloat(), reader->ReadFloat(), reader->ReadFloat(),
                                         reader->ReadFloat() };
                    AppendBytes(array->Data, v, sizeof(v));
                    break;
                }
                case 15: { // Vec4s
                    const int16_t v[4] = { reader->ReadInt16(), reader->ReadInt16(), reader->ReadInt16(),
                                           reader->ReadInt16() };
                    AppendBytes(array->Data, v, sizeof(v));
                    break;
                }
                default:
                    return nullptr;
            }
        }

        return array;
    }
};

} // namespace

namespace GDiffuser {

void RegisterResourceFactories(std::shared_ptr<Ship::ResourceLoader> loader) {
    auto reg = [&](std::shared_ptr<Ship::ResourceFactory> factory, const char* name, uint32_t type,
                   uint32_t version) {
        loader->RegisterResourceFactory(std::move(factory), RESOURCE_FORMAT_BINARY, name, type, version);
    };

    // Generic
    reg(std::make_shared<Ship::ResourceFactoryBinaryBlobV0>(), "Blob",
        static_cast<uint32_t>(Ship::ResourceType::Blob), 0);
    reg(std::make_shared<ResourceFactoryBinaryGenericArrayV0>(), "GenericArray",
        kResourceTypeGenericArray, 0);

    // Fast3D (the bulk of F-Zero X assets)
    reg(std::make_shared<Fast::ResourceFactoryBinaryTextureV0>(), "Texture",
        static_cast<uint32_t>(Fast::ResourceType::Texture), 0);
    reg(std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(), "Texture",
        static_cast<uint32_t>(Fast::ResourceType::Texture), 1);
    reg(std::make_shared<Fast::ResourceFactoryBinaryVertexV0>(), "Vertex",
        static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);
    reg(std::make_shared<Fast::ResourceFactoryBinaryDisplayListV0>(), "DisplayList",
        static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);
    reg(std::make_shared<Fast::ResourceFactoryBinaryMatrixV0>(), "Matrix",
        static_cast<uint32_t>(Fast::ResourceType::Matrix), 0);
    reg(std::make_shared<Fast::ResourceFactoryBinaryLightV0>(), "Light",
        static_cast<uint32_t>(Fast::ResourceType::Light), 0);

    // TODO: F-Zero-X-specific factories — Course, EADAnimation, EADLimb, GhostRecord,
    // Sequence, SoundFont (mirror torch/src/factories/fzerox/).
}

} // namespace GDiffuser
