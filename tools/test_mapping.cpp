#include "ns2_mapping.h"
#include "ns2_usb_reports.h"
#include "nvs.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> durable,staged;
static bool fail_commit=false;
esp_err_t nvs_open(const char *name,int,nvs_handle_t *h) {assert(std::strcmp(name,"ns2")==0);*h=1;return 0;}
esp_err_t nvs_get_blob(nvs_handle_t,const char *key,void *data,size_t *size) {
    assert(std::strcmp(key,"mapping_v1")==0);
    if(durable.empty()||*size<durable.size())return -1;
    *size=durable.size();std::memcpy(data,durable.data(),durable.size());return 0;
}
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void *data,size_t size) {
    const auto *bytes=static_cast<const uint8_t*>(data);staged.assign(bytes,bytes+size);return 0;
}
esp_err_t nvs_commit(nvs_handle_t) {if(fail_commit)return -1;durable=staged;return 0;}
void nvs_close(nvs_handle_t) {staged.clear();}

int main() {
    using namespace ns2;
    MappingConfig config;mapping_defaults(&config);
    assert(mapping_validate(config)&&mapping_is_identity(config));
    for(uint8_t target=0;target<kMappingButtonCount;target++) {
        uint8_t parsed=255;assert(mapping_button_parse(mapping_button_name(target),&parsed)&&parsed==target);
        config.source_for_target[target]=static_cast<uint8_t>((target+1)%kMappingButtonCount);
    }
    uint8_t wire[kMappingWireSize];assert(mapping_serialize(config,wire));
    MappingConfig decoded;assert(mapping_deserialize(wire,sizeof(wire),&decoded));
    assert(std::memcmp(&decoded,&config,sizeof(config))==0);
    wire[8]^=1;assert(!mapping_deserialize(wire,sizeof(wire),&decoded));wire[8]^=1;
    assert(!mapping_deserialize(wire,sizeof(wire)-1,&decoded));
    config.source_for_target[0]=25;assert(!mapping_validate(config));
    mapping_defaults(&config);

    InputSnapshot input;input.valid=true;input.buttons=1u<<19;input.raw_valid=true;
    input.motion_valid=true;input.motion[3]=7;input.source_timestamp_us=0x12345678;input.lx=3456;
    config.source_for_target[0]=20; // GL -> south
    config.source_for_target[10]=20; // GL -> left trigger
    config.source_for_target[20]=kMappingNone;
    mapping_apply(config,&input,false);
    assert((input.buttons&(1u<<0))&&(input.buttons&(1u<<13))&&!(input.buttons&(1u<<19)));
    assert(input.motion[3]==7&&input.lx==3456&&input.source_timestamp_us==0x12345678&&!input.raw_valid);
    mapping_defaults(&config);input.buttons=1u<<14;mapping_apply(config,&input,true);
    assert(input.buttons==(1u<<21)); // default Minus -> Sony touchpad
    config.source_for_target[17]=kMappingNone;input.buttons=1u<<14;mapping_apply(config,&input,true);
    assert(input.buttons==(1u<<14)); // explicit binding suppresses adaptation

    mapping_init();assert(mapping_set(0,20));assert(mapping_save());
    mapping_init();mapping_get(&decoded);assert(decoded.source_for_target[0]==20);
    assert(mapping_set(0,21));fail_commit=true;assert(!mapping_save());
    assert(!mapping_reset());mapping_get(&decoded);assert(decoded.source_for_target[0]==21);
    fail_commit=false;mapping_init();mapping_get(&decoded);assert(decoded.source_for_target[0]==20);
    assert(mapping_reset());mapping_init();mapping_get(&decoded);assert(mapping_is_identity(decoded));
    char json[2048];assert(mapping_command("mapping set ns2pro right_function left_paddle",json,sizeof(json)));
    assert(std::strstr(json,"\"right_function\":\"left_paddle\""));
    assert(mapping_command("mapping set ds5 south east",json,sizeof(json))&&std::strstr(json,"\"ok\":false"));
    assert(mapping_command("mapping set ns2pro south east junk",json,sizeof(json))&&std::strstr(json,"\"ok\":false"));
    assert(!mapping_command("input status",json,sizeof(json)));
    durable[10]^=1;mapping_init();mapping_get(&decoded);assert(mapping_is_identity(decoded));

    uint8_t report[63]={};input=InputSnapshot{};input.valid=true;input.motion_valid=true;
    for(unsigned i=0;i<12;i++)input.motion[i]=static_cast<uint8_t>(i+1);
    input.buttons=(1u<<18)|(1u<<19)|(1u<<21)|(1u<<22)|(1u<<23)|(1u<<24);
    input.source_timestamp_us=0x78563412;
    usb_sony_report_extras(&input,report,true);
    assert(report[9]==0xf6&&report[15]==7&&report[17]==11&&report[19]==9&&report[21]==1);
    assert(report[27]==0x12&&report[30]==0x78&&report[32]==0x80&&report[36]==0x80&&report[52]==0x2a);
    std::memset(report,0,sizeof(report));usb_sony_report_extras(&input,report,false);assert(report[9]==6);
    input.battery_valid=true;input.battery_percent=70;input.battery_charging=true;
    usb_sony_report_extras(&input,report,true);assert(report[52]==0x17);
    uint8_t identity[]={1,2,3,4,5,6};
    assert(usb_sony_feature(5,report,sizeof(report),identity)==40&&report[7]==0x20&&report[9]==0xe0);
    assert(usb_sony_feature(9,report,sizeof(report),identity)==19&&std::memcmp(report,identity,6)==0);
    assert(usb_sony_feature(0x20,report,sizeof(report),identity)==63&&report[27]==0x56);
    assert(usb_sony_feature(0x22,report,sizeof(report),identity)==0);
    std::memset(report,0,sizeof(report));usb_ns2_motion(input,report);
    assert(report[0x2a]==0x12&&report[0x2d]==0x78&&std::memcmp(report+0x30,input.motion,12)==0);
    std::puts("PASS: 25-button mapping, extension/trigger routing, source profile, CRC persistence/failures, Sony motion/Edge/feature codecs");
}
