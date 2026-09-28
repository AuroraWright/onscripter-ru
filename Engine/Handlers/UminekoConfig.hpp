/**
 *  Shared Config additions for Umineko Project language scripts.
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Accepts a preprocessed, null-terminated script. Returns the number of added
// labels, or zero if the script does not have a supported Umineko Config menu.
uint32_t extendUminekoConfig(std::vector<uint8_t> &script, size_t length);
