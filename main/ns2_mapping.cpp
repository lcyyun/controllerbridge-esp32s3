#include "ns2_mapping.h"
#include <cstdio>
#include <cstring>
#include "nvs.h"
#include "freertos/FreeRTOS.h"

namespace ns2 {
namespace {
MappingConfig current;
bool dirty=false;
portMUX_TYPE mapping_lock=portMUX_INITIALIZER_UNLOCKED;
void replace(const MappingConfig &config,bool changed) {
    portENTER_CRITICAL(&mapping_lock);current=config;dirty=changed;portEXIT_CRITICAL(&mapping_lock);
}
bool persist(const MappingConfig &config) {
    uint8_t wire[kMappingWireSize];if(!mapping_serialize(config,wire)) return false;
    nvs_handle_t handle=0;
    if(nvs_open("ns2",NVS_READWRITE,&handle)!=ESP_OK) return false;
    esp_err_t rc=nvs_set_blob(handle,"mapping_v1",wire,sizeof(wire));
    if(rc==ESP_OK) rc=nvs_commit(handle);
    nvs_close(handle);return rc==ESP_OK;
}
}
void mapping_init() {
    MappingConfig config;mapping_defaults(&config);
    nvs_handle_t handle=0;
    if(nvs_open("ns2",NVS_READONLY,&handle)==ESP_OK) {
        uint8_t wire[kMappingWireSize];size_t size=sizeof(wire);
        if(nvs_get_blob(handle,"mapping_v1",wire,&size)==ESP_OK) mapping_deserialize(wire,size,&config);
        nvs_close(handle);
    }
    replace(config,false);
}
void mapping_get(MappingConfig *out) {
    if (!out) return;
    portENTER_CRITICAL(&mapping_lock);
    *out = current;
    portEXIT_CRITICAL(&mapping_lock);
}
bool mapping_set(uint8_t target,uint8_t source) {
    if(target>=kMappingButtonCount || (source>=kMappingButtonCount && source!=kMappingNone)) return false;
    portENTER_CRITICAL(&mapping_lock);current.source_for_target[target]=source;dirty=true;portEXIT_CRITICAL(&mapping_lock);
    return true;
}
bool mapping_save() {
    MappingConfig config;mapping_get(&config);if(!persist(config)) return false;
    portENTER_CRITICAL(&mapping_lock);if(std::memcmp(&current,&config,sizeof(config))==0) dirty=false;portEXIT_CRITICAL(&mapping_lock);
    return true;
}
bool mapping_reset() {
    MappingConfig config;mapping_defaults(&config);if(!persist(config)) return false;replace(config,false);return true;
}
int mapping_format_json(char *out,size_t capacity) {
    MappingConfig config;bool changed;
    portENTER_CRITICAL(&mapping_lock);config=current;changed=dirty;portEXIT_CRITICAL(&mapping_lock);
    int written=std::snprintf(out,capacity,"{\"ok\":true,\"profile\":\"ns2pro\",\"identity\":%s,\"dirty\":%s,\"entries\":{",mapping_is_identity(config)?"true":"false",changed?"true":"false");
    if(written<0 || static_cast<size_t>(written)>=capacity)return -1;
    size_t used=written;
    for(uint8_t i=0;i<kMappingButtonCount;i++) {
        written=std::snprintf(out+used,capacity-used,"%s\"%s\":\"%s\"",i?",":"",mapping_button_name(i),mapping_button_name(config.source_for_target[i]));
        if (written < 0 || static_cast<size_t>(written) >= capacity - used) return -1;
        used += written;
    }
    written=std::snprintf(out+used,capacity-used,"}}");
    return written<0 || static_cast<size_t>(written)>=capacity-used ? -1 : static_cast<int>(used)+written;
}
bool mapping_command(const char *command,char *out,size_t capacity) {
    if(std::strncmp(command,"mapping",7)!=0 || (command[7] && command[7]!=' ')) return false;
    char op[16]={},profile[16]={},target[32]={},source[32]={},extra[2]={};
    int count=std::sscanf(command,"mapping %15s %15s %31s %31s %1s",op,profile,target,source,extra);
    bool ok=false;
    if(count==1 || (count==2 && std::strcmp(profile,"ns2pro")==0)) {
        if(std::strcmp(op,"get")==0)ok=true;
        else if(std::strcmp(op,"save")==0)ok=mapping_save();
        else if(std::strcmp(op,"reset")==0)ok=mapping_reset();
    } else if(count==4 && std::strcmp(op,"set")==0 && std::strcmp(profile,"ns2pro")==0) {
        uint8_t dst,src;ok=mapping_button_parse(target,&dst)&&mapping_button_parse(source,&src)&&mapping_set(dst,src);
    } else if(count==3 && std::strcmp(op,"set")==0) {
        uint8_t dst,src;ok=mapping_button_parse(profile,&dst)&&mapping_button_parse(target,&src)&&mapping_set(dst,src);
    }
    if(!ok)std::snprintf(out,capacity,"{\"ok\":false,\"error\":\"invalid_mapping_command_or_save_failed\"}");
    else if(mapping_format_json(out,capacity)<0)std::snprintf(out,capacity,"{\"ok\":false,\"error\":\"response_too_large\"}");
    return true;
}
}
