#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "ns2_input.h"

namespace ns2 {
inline void usb_ns2_motion(const InputSnapshot &input, uint8_t *report) {
    for (unsigned i=0;i<4;i++) report[0x2a+i]=static_cast<uint8_t>(input.source_timestamp_us>>(8*i));
    if (input.motion_valid) std::memcpy(report+0x30,input.motion,kMotionSampleSize);
}

// Raw sensor ordering and neutral-contact flags follow the BL616 Sony codec.
inline void usb_sony_report_extras(const InputSnapshot *input, uint8_t *report, bool edge) {
    report[32]=report[36]=0x80;
    report[52]=0x2a; // compatibility placeholder; never used as physical telemetry
    if (!input || !input->valid) return;
    const uint32_t buttons=input->buttons;
    if(buttons&(1u<<21))report[9]|=0x02;
    if(buttons&(1u<<22))report[9]|=0x04;
    if(edge) {
        if(buttons&(1u<<23))report[9]|=0x10;
        if(buttons&(1u<<24))report[9]|=0x20;
        if(buttons&(1u<<19))report[9]|=0x40;
        if(buttons&(1u<<18))report[9]|=0x80;
    }
    if(input->motion_valid) {
        std::memcpy(report+15,input->motion+6,2);
        std::memcpy(report+17,input->motion+10,2);
        std::memcpy(report+19,input->motion+8,2);
        std::memcpy(report+21,input->motion,6);
    }
    for(unsigned i=0;i<4;i++)report[27+i]=static_cast<uint8_t>(input->source_timestamp_us>>(8*i));
    if(input->battery_valid) {
        const uint8_t level=std::min<unsigned>(10,(input->battery_percent+5)/10);
        report[52]=level|(input->battery_charging?0x10:input->battery_percent>=100?0x20:0);
    }
}

// Generated device calibration/identity: no physical DualSense exists on BLE.
// Report lengths and nominal calibration match BL616 fallback features.
inline size_t usb_sony_feature(uint8_t id,uint8_t *buffer,size_t capacity,const uint8_t *identity) {
    uint8_t payload[63]={};
    size_t size;
    switch(id) {
    case 0x05: {
        size=40;
        constexpr int16_t calibration[]={0,0,0,8192,-8192,8192,-8192,8192,-8192,
                                        1024,-1024,8192,-8192,8192,-8192,8192,-8192};
        for(size_t i=0;i<sizeof(calibration)/sizeof(calibration[0]);i++) {
            payload[2*i]=static_cast<uint8_t>(calibration[i]);
            payload[2*i+1]=static_cast<uint8_t>(static_cast<uint16_t>(calibration[i])>>8);
        }
        break;
    }
    case 0x09:
        size=19;
        if(identity)std::memcpy(payload,identity,6);
        break;
    case 0x20:
        size=63;payload[24]=2;payload[27]=0x56;payload[28]=2;payload[43]=0x21;payload[44]=2;
        break;
    default:return 0;
    }
    if(!buffer)return 0;
    size=std::min(size,capacity);std::memcpy(buffer,payload,size);return size;
}
}
