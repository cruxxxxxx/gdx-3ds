#pragma once
// G-Diffuser — resource factory registration.
#include <memory>

namespace Ship {
class ResourceLoader;
}

namespace GDiffuser {
// Call after the ResourceManager is initialized and before loading any game resource. The
// loader is passed explicitly rather than fetched from Context, so this also works when the
// global Context singleton is not set (e.g. under CreateUninitializedInstance).
void RegisterResourceFactories(std::shared_ptr<Ship::ResourceLoader> loader);
} // namespace GDiffuser
