// Compatibility name for the legacy FB2 parser.
// New parser/storage code includes FsByteReader directly.
#pragma once

#include "../../../src/reader/core/io/FsByteReader.h"

using FsFileReader = FsByteReader;
