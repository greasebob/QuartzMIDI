#pragma once
// Public key for add-on signatures, in the format `tools\sign-addon.ps1 -PublicKey`
// prints. The build generates build\addon-key\AddonKey.hpp; without it the key is
// all zeros and no add-on loads.
#include "../engine/AddonHost.hpp"

#if __has_include("../build/addon-key/AddonKey.hpp")
#include "../build/addon-key/AddonKey.hpp"
#else
inline constexpr addon::PublicKey kOwnerKey{};
#endif
