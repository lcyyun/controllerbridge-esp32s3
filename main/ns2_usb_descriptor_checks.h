#pragma once
#include <cstddef>
#include <cstdint>

namespace ns2::usb_descriptor_checks {
constexpr unsigned le16(const uint8_t *p) { return p[0] | (unsigned(p[1]) << 8); }

template<size_t N>
constexpr bool configuration(const uint8_t (&bytes)[N], unsigned interfaces,
                             unsigned manager_interface, uint8_t manager_ep, unsigned manager_packet_size, size_t manager_report_size) {
    if (N < 9 || bytes[0] != 9 || bytes[1] != 2 || le16(bytes+2) != N || bytes[4] != interfaces) return false;
    unsigned interface_mask=0, seen_endpoints=0, declared_endpoints=0, actual_endpoints=0;
    unsigned current_interface=255, current_class=0, manager_endpoints=0;
    bool manager_hid=false;
    for (size_t offset=9; offset<N;) {
        if (offset+2>N) return false;
        const unsigned length=bytes[offset], type=bytes[offset+1];
        if (length<2 || offset+length>N) return false;
        const uint8_t *d=bytes+offset;
        if (type==4) {
            if (current_interface!=255 && declared_endpoints!=actual_endpoints) return false;
            if (length!=9 || d[2]>=interfaces) return false;
            current_interface=d[2];current_class=d[5];interface_mask|=1u<<d[2];
            declared_endpoints=d[4];actual_endpoints=0;
            if (current_interface==manager_interface && (d[3]!=0 || d[4]!=2 || d[5]!=3)) return false;
        } else if (type==0x21 && current_class==3) {
            if (length!=9) return false;
            if (current_interface==manager_interface) {
                manager_hid=d[6]==0x22 && le16(d+7)==manager_report_size;
            }
        } else if (type==5) {
            if (length<7 || current_interface==255 || (d[2]&0x0f)==0 || (d[2]&0x0f)>6) return false;
            const unsigned endpoint_bit=(d[2]&0x0f)+((d[2]&0x80)?8:0);
            if (seen_endpoints&(1u<<endpoint_bit)) return false;
            seen_endpoints|=1u<<endpoint_bit;++actual_endpoints;
            if (current_interface==manager_interface && (d[2]&0x80)) {
                if (d[2]!=manager_ep || (d[3]&3)!=3 || le16(d+4)!=manager_packet_size || d[6]==0) return false;
                ++manager_endpoints;
            }
        }
        offset+=length;
    }
    return declared_endpoints==actual_endpoints && interface_mask==((1u<<interfaces)-1) &&
           manager_endpoints==1 && manager_hid;
}

template<size_t N>
constexpr unsigned report_bits(const uint8_t (&bytes)[N], unsigned report, unsigned main_tag) {
    unsigned size=0,count=0,id=0,bits=0;
    for (size_t offset=0; offset<N;) {
        const unsigned prefix=bytes[offset++];
        if (prefix==0xfe) return 0; // these descriptors contain short items only
        const unsigned length=(prefix&3)==3?4:prefix&3;
        if (offset+length>N) return 0;
        unsigned value=0;
        for (unsigned n=0;n<length;n++) value|=unsigned(bytes[offset+n])<<(8*n);
        const unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if (type==1) {
            if(tag==7)size=value;
            if(tag==8)id=value;
            if(tag==9)count=value;
        } else if (type==0 && tag==main_tag && id==report) bits+=size*count;
        offset+=length;
    }
    return bits;
}

// A Windows HID collection is selected by its top-level usage. Report IDs must
// belong to that collection, and its maximum feature payload determines the
// buffer HidSharp allocates (63 payload bytes plus the report ID).
template<size_t N>
constexpr unsigned vendor_feature_bits(const uint8_t (&bytes)[N], unsigned report=0) {
    unsigned size=0,count=0,id=0,page=0,usage=0,depth=0;
    unsigned bits[256]={};
    bool manager=false;
    for(size_t offset=0;offset<N;) {
        const unsigned prefix=bytes[offset++];
        if(prefix==0xfe)return 0;
        const unsigned length=(prefix&3)==3?4:prefix&3;
        if(offset+length>N)return 0;
        unsigned value=0;
        for(unsigned n=0;n<length;n++)value|=unsigned(bytes[offset+n])<<(8*n);
        const unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if(type==1) {
            if(tag==0)page=value;
            if(tag==7)size=value;
            if(tag==8)id=value;
            if(tag==9)count=value;
        } else if(type==2 && tag==0)usage=value;
        else if(type==0) {
            if(tag==10) {
                if(depth==0)manager=page==0xff00 && usage==1 && value==1;
                ++depth;
            } else if(tag==12) {
                if(depth==0)return 0;
                --depth;
                if(depth==0)manager=false;
            } else if(tag==11 && manager) {
                if(id>=256)return 0;
                bits[id]+=size*count;
            }
            usage=0;
        }
        offset+=length;
    }
    if(depth!=0 || report>=256)return 0;
    if(report!=0)return bits[report];
    unsigned maximum=0;
    for(unsigned value:bits)if(value>maximum)maximum=value;
    return maximum;
}

struct FifoBudget { bool valid; unsigned rx_words; unsigned tx_words; unsigned in_endpoints; };

// Exact allocation arithmetic from pinned TinyUSB 0.19.0~3 dcd_dwc2.c:
// calc_device_grxfsiz + dfifo_alloc, S3 ep_count7/ep_in_count5/1024 bytes.
// The UAC1 driver never allocates the descriptor-only microphone endpoint.
// Slave mode sets TX_FIFO_EMPTY_LVL, so TX is one rounded packet, not doubled.
template<size_t N>
constexpr FifoBudget fifo(const uint8_t (&bytes)[N], bool microphone_unsupported) {
    FifoBudget result{true,62,16,1}; // EP0 OUT shared RX and EP0 IN64
    unsigned current_interface=255;
    for (size_t offset=9;offset<N;) {
        if(offset+2>N || bytes[offset]<2 || offset+bytes[offset]>N) return {false,0,0,0};
        const uint8_t *d=bytes+offset;
        if(d[1]==4)current_interface=d[2];
        if(d[1]==5) {
            if (!(microphone_unsupported && current_interface==2 && d[2]==0x82)) {
                const unsigned words=(le16(d+4)+3)/4;
                if(d[2]&0x80) {
                    if((d[2]&0x0f)>4)return {false,0,0,0};
                    result.tx_words+=words;++result.in_endpoints;
                } else {
                    const unsigned rx=13+1+2*(words+1)+2*7;
                    if(rx>result.rx_words)result.rx_words=rx;
                }
                if(result.rx_words+result.tx_words>256 || result.in_endpoints>5)result.valid=false;
            }
        }
        offset+=bytes[offset];
    }
    return result;
}
}
