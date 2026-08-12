#pragma once

// PlatformIO normally supplies these through build_flags/extra_scripts. Keep
// fallbacks here so editor indexers and simulator-like tools still parse files.
#ifndef INKMOD_VERSION
#define INKMOD_VERSION "dev"
#endif

#ifndef INKMOD_BUILD_ENV
#define INKMOD_BUILD_ENV "unknown"
#endif

#ifndef INKMOD_FIRMWARE_VARIANT
#define INKMOD_FIRMWARE_VARIANT "unknown"
#endif
