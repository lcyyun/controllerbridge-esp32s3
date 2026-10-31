#pragma once

#include <cstddef>
#include <cstdint>
#include "ns2_input.h"

namespace ns2 {
// Canonical positions exactly match the BL616 mapping protocol. Profiles are
// selected by the physical controller; this firmware accepts NS2Pro BLE only.
constexpr uint8_t kMappingButtonCount = 25;
constexpr uint8_t kMappingNone = 0xff;
constexpr size_t kMappingWireSize = 40;
struct MappingConfig { uint8_t source_for_target[kMappingButtonCount]; };

void mapping_defaults(MappingConfig *config);
bool mapping_validate(const MappingConfig &config);
bool mapping_is_identity(const MappingConfig &config);
uint32_t mapping_canonical_buttons(uint32_t native_buttons);
void mapping_apply(const MappingConfig &config, InputSnapshot *input, bool sony_output);
const char *mapping_button_name(uint8_t button);
bool mapping_button_parse(const char *name, uint8_t *button);
bool mapping_serialize(const MappingConfig &config, uint8_t wire[kMappingWireSize]);
bool mapping_deserialize(const uint8_t *wire, size_t size, MappingConfig *config);

void mapping_init();
void mapping_get(MappingConfig *out);
// Runtime changes are explicit until mapping_save(). Reset saves defaults.
bool mapping_set(uint8_t target, uint8_t source);
bool mapping_save();
bool mapping_reset();
int mapping_format_json(char *out, size_t capacity);
// Returns false for a non-mapping command; malformed commands return JSON error.
bool mapping_command(const char *command, char *out, size_t capacity);
}
