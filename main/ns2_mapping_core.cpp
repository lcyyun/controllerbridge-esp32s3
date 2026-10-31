#include "ns2_mapping.h"
#include <cstring>

namespace ns2 {
namespace {
constexpr uint8_t native_bits[kMappingButtonCount] = {
    0,1,2,3,11,8,10,9,12,4,13,5,14,6,15,7,16,21,22,17,19,18,23,24,20
};
const char *const names[kMappingButtonCount] = {
    "south","east","west","north","dpad_up","dpad_down","dpad_left","dpad_right",
    "left_shoulder","right_shoulder","left_trigger","right_trigger","back","start",
    "left_stick","right_stick","guide","touchpad","mute","capture","left_paddle",
    "right_paddle","left_function","right_function","c"
};
uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i=0;i<size;i++) {
        crc ^= data[i];
        for (unsigned b=0;b<8;b++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc;
}
}
void mapping_defaults(MappingConfig *c) {
    if(c) for(uint8_t i=0;i<kMappingButtonCount;i++) c->source_for_target[i]=i;
}
bool mapping_validate(const MappingConfig &c) {
    for(uint8_t s:c.source_for_target) if(s>=kMappingButtonCount && s!=kMappingNone) return false;
    return true;
}
bool mapping_is_identity(const MappingConfig &c) {
    for(uint8_t i=0;i<kMappingButtonCount;i++) if(c.source_for_target[i]!=i) return false;
    return true;
}
uint32_t mapping_canonical_buttons(uint32_t buttons) {
    uint32_t canonical=0;
    for(uint8_t i=0;i<kMappingButtonCount;i++) if(buttons&(1u<<native_bits[i])) canonical|=1u<<i;
    return canonical;
}
void mapping_apply(const MappingConfig &c, InputSnapshot *input, bool sony_output) {
    if(!input || !input->valid || !mapping_validate(c)) return;
    const uint32_t physical=mapping_canonical_buttons(input->buttons);
    uint32_t mapped=0;
    for(uint8_t target=0;target<kMappingButtonCount;target++) {
        const uint8_t source=c.source_for_target[target];
        if(source<kMappingButtonCount && (physical&(1u<<source))) mapped|=1u<<native_bits[target];
    }
    // BL616 default NS2 adaptation: Minus -> touchpad, Capture -> Create.
    // Explicit Back/Touchpad bindings always take precedence over the default.
    if(sony_output && c.source_for_target[12]==12 && c.source_for_target[17]==17) {
        if(mapped&(1u<<14)) mapped|=1u<<21;
        mapped&=~(1u<<14);
    }
    input->buttons=mapped;
    if(!mapping_is_identity(c)) input->raw_valid=false;
}
const char *mapping_button_name(uint8_t button) {
    return button==kMappingNone?"none":button<kMappingButtonCount?names[button]:"invalid";
}
bool mapping_button_parse(const char *name,uint8_t *button) {
    if(!name || !button) return false;
    if(std::strcmp(name,"none")==0) {*button=kMappingNone;return true;}
    for(uint8_t i=0;i<kMappingButtonCount;i++) if(std::strcmp(name,names[i])==0) {*button=i;return true;}
    return false;
}
bool mapping_serialize(const MappingConfig &c,uint8_t wire[kMappingWireSize]) {
    if(!wire || !mapping_validate(c)) return false;
    std::memset(wire,0,kMappingWireSize);std::memcpy(wire,"SFM1",4);
    wire[4]=1;wire[6]=kMappingButtonCount;std::memcpy(wire+8,c.source_for_target,kMappingButtonCount);
    uint32_t crc=crc32(wire,36);for(unsigned i=0;i<4;i++) wire[36+i]=static_cast<uint8_t>(crc>>(8*i));
    return true;
}
bool mapping_deserialize(const uint8_t *wire,size_t size,MappingConfig *c) {
    if(!wire || !c || size!=kMappingWireSize || std::memcmp(wire,"SFM1",4)!=0 ||
       wire[4]!=1 || wire[5]!=0 || wire[6]!=kMappingButtonCount || wire[7]!=0) return false;
    uint32_t crc=0;for(unsigned i=0;i<4;i++) crc|=static_cast<uint32_t>(wire[36+i])<<(8*i);
    if(crc!=crc32(wire,36)) return false;
    MappingConfig candidate;std::memcpy(candidate.source_for_target,wire+8,kMappingButtonCount);
    if(!mapping_validate(candidate)) return false;
    *c=candidate;return true;
}
}
