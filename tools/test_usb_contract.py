"""Compile actual USB descriptors/dispatch and pinned DWC2 allocation on the host.

No hardware claim: register writes, BLE, reboot and NVS I/O are replaced by test
adapters. Descriptor bytes, TinyUSB descriptor macros/allocation, JSON formatters,
command parsers/handlers and mapping implementation come from production files.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def between(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--module', type=Path, default=ROOT/'module/module.json')
    args = parser.parse_args()
    work = ROOT/'test-results/usb-contract'
    work.mkdir(parents=True, exist_ok=True)
    (work/'esp_err.h').write_text('#pragma once\n#include "nvs.h"\n')
    (work/'tusb_config.h').write_text('''#pragma once
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_HID 1
#define CFG_TUD_VENDOR 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_HID_EP_BUFSIZE 64
''')
    usb = (ROOT/'main/ns2_usb.cpp').read_text()
    driver = (ROOT/'managed_components/espressif__tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c').read_text()
    port = (ROOT/'managed_components/espressif__tinyusb/src/portable/synopsys/dwc2/dwc2_esp32.h').read_text()
    assert '.ep_count = 7' in port and '.ep_in_count = 5' in port and '.ep_fifo_size = 1024' in port
    assert 'gahbcfg |= GAHBCFG_TX_FIFO_EPMTY_LVL' in driver
    uac = (ROOT/'main/ns2_usb_uac1.cpp').read_text()
    assert 'CONFIG_TINYUSB_MODE_DMA' in uac and '#error' in uac
    parts = [r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "tusb.h"
#include "class/audio/audio.h"
#include "ns2_protocol.h"
#include "manager_protocol.h"
#include "ns2_config.h"
#include "ns2_input.h"
#include "ns2_ble.h"
#include "ns2_mapping.h"
#include "ns2_json.h"
#include "ns2_feedback.h"
#include "ns2_usb_descriptor_checks.h"
#include "nvs.h"
#define ESP_LOGW(...)
''']
    # Keep mapping's real NVS serialization/command code, with test-only storage.
    mapping_test = (ROOT/'tools/test_mapping.cpp').read_text()
    parts += [between(mapping_test, 'static std::vector<uint8_t> durable', 'int main()')]
    parts += ['namespace ns2 {\n', between(usb, 'const char *TAG =', 'const uint8_t kMsOs10StringDescriptor[]'),
              between(usb, 'struct UsbState {', 'portMUX_TYPE s_feature_lock')]
    parts += [r'''
std::string reply;
BleStats mock_ble;
InputSnapshot mock_input;
OutputMode configured_mode=OutputMode::Nintendo;
bool fail_save=false;
unsigned reboot_calls=0,forget_calls=0,rumble_calls=0;
int64_t mock_time_us=1000;
int64_t esp_timer_get_time(){return mock_time_us;}
void queue_feature_json(const char *s){reply=s;}
void queue_feature_error(const char *s){reply="{\"ok\":false,\"error\":\""+std::string(s)+"\"}";}
void schedule_restart(){++reboot_calls;}
esp_err_t config_save(){return fail_save?-1:ESP_OK;}
void config_set_usb(bool,bool,bool,uint16_t,uint16_t,uint16_t,uint8_t,uint16_t,OutputMode m){configured_mode=m;}
void config_set_output_mode(OutputMode m){configured_mode=m;}
const char *config_output_mode_name(OutputMode m){const char *names[]={"nintendo","xinput","dualsense","dualsense_edge"};return names[unsigned(m)];}
void ble_get_stats(BleStats *out){*out=mock_ble;}
void ble_start_scan(){mock_ble.scanning=true;}
void ble_pair(){mock_ble.pair_mode=true;}
void ble_reconnect(){mock_ble.connected=true;}
void ble_disconnect(){mock_ble.connected=false;}
void ble_forget(){++forget_calls;mock_ble.saved_target_valid=false;}
void ble_set_auto_connect(bool enabled){mock_ble.auto_connect=enabled;}
bool input_get_snapshot(InputSnapshot *out){*out=mock_input;return out->valid;}
uint32_t input_last_age_ms(){return mock_input.input_age_ms;}
void format_ble_addr(const uint8_t*,char *out,size_t n){std::snprintf(out,n,"00:00:00:00:00:00");}
const char *input_kind_name(InputReportKind){return "FD2";}
void input_format_motion_json(const InputSnapshot*,char*out,size_t n){std::snprintf(out,n,"\"motion_valid\":false");}
struct StickAxes12 {uint16_t x,y;};
struct StaticQueue_t {std::vector<uint8_t> item;size_t size=0;bool ready=false;};
using QueueHandle_t=StaticQueue_t*;
constexpr int pdTRUE=1;
QueueHandle_t xQueueCreateStatic(unsigned,size_t size,uint8_t*,StaticQueue_t *storage){storage->size=size;return storage;}
int xQueueOverwrite(QueueHandle_t queue,const void *item){const auto*p=static_cast<const uint8_t*>(item);queue->item.assign(p,p+queue->size);queue->ready=true;return pdTRUE;}
int xQueueReceive(QueueHandle_t queue,void *out,unsigned){if(!queue->ready)return 0;std::memcpy(out,queue->item.data(),queue->size);queue->ready=false;return pdTRUE;}
void xQueueReset(QueueHandle_t queue){queue->ready=false;}
''', between(usb, 'OutputMode saved_output_mode()', 'struct StickAxes12 {'),
        between(usb, 'const char *skip_spaces(', 'uint32_t command_address('),
        between(usb, 'uint16_t normalize_stick_axis(', 'OutputMode next_output_mode('),
        between(usb, 'int16_t axis_to_xinput_normalized(', 'uint8_t axis_to_u8_normalized('),
        between(usb, 'int clamp_int(', 'int map_switch_amp_to_ble('),
        between(usb, 'struct Ds5AudioPacket {', 'void usb_event_cb(')]
    parts += [r'''
void set_report_rate(uint32_t n){s_usb.report_rate_hz=n;s_usb.report_interval_us=1000000/n;}
void stop_rumble(){s_usb.rumble_active=false;}
void inject_mode_rumble_test(bool,bool,uint16_t,uint16_t){++rumble_calls;}
void inject_mode_rumble(uint8_t,uint8_t,uint16_t){++rumble_calls;}
unsigned applied_kind=0,applied_data=0;
bool bridge_xinput_output_to_ble(const uint8_t*p,size_t){++rumble_calls;applied_kind=3;applied_data=p[0];return true;}
bool bridge_ds5_output_to_ble(uint8_t,const uint8_t*p,size_t){++rumble_calls;applied_kind=2;applied_data=p[0];return true;}
bool bridge_hid_output_to_ble(const uint8_t*p,size_t){++rumble_calls;applied_kind=1;applied_data=p[0];return true;}
''', between(usb, 'void process_host_feedback()', 'bool hid_guard_timed_out()'),
        between(usb, 'void format_settings_json(', 'void receive_feature_command(')]
    parts += [r'''
// Actual pinned allocator runs against simulated DWC2 registers.
struct dwc2_regs_t {uint32_t grxfsiz=0,gahbcfg=0,dieptxf0=0,dieptxf[7]={},gdfifocfg=0;};
struct dwc2_controller_t {uint8_t ep_count,ep_in_count;uint16_t ep_fifo_size;};
static dwc2_controller_t _dwc2_controller[]={{7,5,1024}};
static struct {uint16_t dfifo_top=0;uint8_t allocated_epin_count=0;} _dcd_data;
static dwc2_regs_t regs;
#define DWC2_REG(rhport) (&regs)
#undef TU_ASSERT
#define TU_ASSERT(x) do{if(!(x))return false;}while(0)
#define GAHBCFG_TX_FIFO_EPMTY_LVL (1u<<7)
#define DIEPTXF0_TX0FD_Pos 16
#define DIEPTXF_INEPTXFD_Pos 16
#define GDFIFOCFG_EPINFOBASE_SHIFT 16
bool dma_device_enabled(const dwc2_regs_t*){return false;}
''', between(driver, 'TU_ATTR_ALWAYS_INLINE static inline uint16_t calc_device_grxfsiz', '//--------------------------------------------------------------------\n// Endpoint')]
    parts += [r'''
template<size_t N> bool allocate(const uint8_t (&bytes)[N],bool mic) {
    regs={};_dcd_data={};regs.gahbcfg=GAHBCFG_TX_FIFO_EPMTY_LVL;dfifo_device_init(0);
    unsigned interface=255;
    for(size_t offset=9;offset<N;offset+=bytes[offset]){
        const auto*d=bytes+offset;
        if(d[1]==4)interface=d[2];
        if(d[1]==5 && !(mic && interface==2 && d[2]==0x82))
            if(!dfifo_alloc(0,d[2],usb_descriptor_checks::le16(d+4)))return false;
    }
    return true;
}
void descriptor_tests(){
    assert(allocate(kConfigurationDescriptor,false));
    assert(allocate(kXInputConfigurationDescriptor,false));
    assert(allocate(kDualSenseConfigurationDescriptor,true));
    assert(regs.grxfsiz==222 && _dcd_data.dfifo_top==224);
    assert((regs.dieptxf[3]>>16)==16); // Sony64-byte IN requires16words, no new NSM TX FIFO.
    assert(!allocate(kDualSenseConfigurationDescriptor,false));
    uint8_t damaged[sizeof(kConfigurationDescriptor)];std::memcpy(damaged,kConfigurationDescriptor,sizeof(damaged));
    damaged[13]=0; // Claim zero endpoint on the existing HID interface.
    assert(!usb_descriptor_checks::configuration(damaged,2,0,kHidEpIn,64,sizeof(kHidReportDescriptor)));
    damaged[13]=2;damaged[2]--;
    assert(!usb_descriptor_checks::configuration(damaged,2,0,kHidEpIn,64,sizeof(kHidReportDescriptor)));
    char escaped[32];assert(json_escape_bytes("a\"\\\n\x01",6,escaped,sizeof(escaped)));
    assert(std::strcmp(escaped,"a\\\"\\\\\\u000a\\u0001")==0);
    char tiny[2];assert(!json_escape_bytes("\n",1,tiny,sizeof(tiny)) && tiny[0]==0);
    char short_status[96];format_ble_status_json(short_status,sizeof(short_status));
    assert(std::strcmp(short_status,"{\"ok\":false,\"error\":\"reply_too_large\"}")==0);
}
void feedback_tests(){
    s_host_feedback_queue=xQueueCreateStatic(1,sizeof(HostFeedbackPacket),s_host_feedback_queue_buffer,&s_host_feedback_queue_storage);
    mock_input.valid=true;mock_input.source_epoch=7;mock_time_us=1000;
    uint8_t active[]={99},stop[]={0};
    assert(queue_host_feedback(HostFeedbackKind::Nintendo,active,1));
    assert(rumble_calls==0); // USB producer never mutates effect state.
    process_host_feedback();assert(rumble_calls==1 && applied_kind==1 && applied_data==99);
    assert(queue_host_feedback(HostFeedbackKind::Sony,active,1));
    assert(queue_host_feedback(HostFeedbackKind::Sony,stop,1));
    process_host_feedback();assert(rumble_calls==2 && applied_kind==2 && applied_data==0);
    assert(queue_host_feedback(HostFeedbackKind::XInput,active,1));
    mock_input.source_epoch++;process_host_feedback();assert(rumble_calls==2);
    assert(queue_host_feedback(HostFeedbackKind::XInput,active,1));
    mock_input.valid=false;process_host_feedback();assert(rumble_calls==2);
    assert(!queue_host_feedback(HostFeedbackKind::XInput,active,1));
    mock_input.valid=true;assert(queue_host_feedback(HostFeedbackKind::XInput,active,1));
    mock_time_us+=250001;process_host_feedback();assert(rumble_calls==2);
    assert(queue_host_feedback(HostFeedbackKind::XInput,active,1));
    xQueueReset(s_host_feedback_queue);process_host_feedback();assert(rumble_calls==2);
    assert(!queue_host_feedback(HostFeedbackKind::XInput,nullptr,1));
    assert(!queue_host_feedback(HostFeedbackKind::XInput,active,129));
    assert(queue_host_feedback(HostFeedbackKind::XInput,active,1));
    process_host_feedback();assert(rumble_calls==3 && applied_kind==3);
    mock_input={};
}
} // namespace ns2
int main(){
    using namespace ns2;descriptor_tests();feedback_tests();mapping_init();mock_ble.started=true;
    mock_ble.encrypted=true;mock_ble.pairing_key_saved=true;mock_ble.persist_pending=true;mock_ble.persist_error=-123;
    std::strcpy(mock_ble.last_name,"quote\" slash\\ line\n");
    // Actual advertised identities, in the module's role order.
    std::printf("{\"devices\":[[%u,%u,\"%s\"],[%u,%u,\"%s\"],[%u,%u,\"%s\"],[%u,%u,\"%s\"]]}\n",
        kDualSenseDeviceDescriptor.idVendor,kDualSenseDeviceDescriptor.idProduct,kDualSenseStringDescriptor[2],
        kDualSenseEdgeDeviceDescriptor.idVendor,kDualSenseEdgeDeviceDescriptor.idProduct,kDualSenseEdgeStringDescriptor[2],
        kXInputDeviceDescriptor.idVendor,kXInputDeviceDescriptor.idProduct,kXInputStringDescriptor[2],
        kDeviceDescriptor.idVendor,kDeviceDescriptor.idProduct,kStringDescriptor[2]);
    std::string command;
    while(std::getline(std::cin,command)) {
        reply.clear();handle_feature_command(command.c_str());
        assert(!reply.empty());std::cout<<reply<<'\n';
    }
}
''']
    generated=work/'contract.cpp'
    generated.write_text('\n'.join(parts))
    exe=work/'contract.exe'
    env=os.environ.copy()
    env['PATH']=str(Path(args.compiler).parent)+os.pathsep+env['PATH']
    subprocess.run([args.compiler,'-std=c++20','-Wno-attributes','-Wno-volatile','-I'+str(work),
                    '-I'+str(ROOT/'tools/mapping_host_stubs'),'-I'+str(ROOT/'main'),
                    '-I'+str(ROOT/'managed_components/espressif__tinyusb/src'),str(generated),
                    str(ROOT/'main/ns2_mapping_core.cpp'),str(ROOT/'main/ns2_mapping.cpp'),'-o',str(exe)],check=True,env=env)
    module=json.loads(args.module.read_text(encoding='utf-8-sig'))
    assert len(module['hidDevices'])==4
    for profile in module['hidDevices']:
        assert profile['managerFeatureReportId']==126 and profile['managerUsagePage']==0xff00 and profile['managerUsageId']==1
    values={'profile':'ns2pro','target':'south','source':'left_paddle','rumbleScalePercent':60,
            'rumbleHoldMs':140,'rumbleTickMs':12,'rumbleStopPackets':3,'reportRateHz':250,
            'usbRaw':'off','liveParsing':'on'}
    commands=list(module['statusCommands'])+list(module['selfTestCommands'])
    commands += [item['command'] for item in module['usbRoles']+module['inputSources']]
    commands += [v for item in module['wirelessControllers'] for k,v in item.items() if k.endswith('Command')]
    commands += [module['primaryStatusCommand'],module['inputStatusCommand'],module['saveSettingsCommand']]
    commands += [s.format(**values) for s in module['settingsCommandTemplates']]
    commands += [item['request'].format(**values) for item in module['operations'].values()]
    commands += [module['rumbleCommandTemplate'].format(target=v) for v in ['left','right','both']]
    commands += ['rumble stop','audio status','usb raw on','web parse off','status']
    invalid=['role set ds4','input set ds5','usb rate 0','usb rate 1001','mapping set ns2pro invalid south']
    result=subprocess.run([str(exe)],input='\n'.join(commands+invalid)+'\n',text=True,capture_output=True,check=True,env=env)
    responses=[json.loads(line) for line in result.stdout.splitlines()]
    identities=responses.pop(0)['devices']
    assert identities==[[d['vendorId'],d['productId'],d['legacyManagerProductName']] for d in module['hidDevices']]
    assert len(responses)==len(commands)+len(invalid)
    for response in responses[len(commands):]:
        assert response.get('ok') is False,response
    for command,response in zip(commands,responses):
        assert response.get('ok') is True,(command,response)
        if command.startswith('role set '):
            expected={'ds5':'dualsense','dse':'dualsense_edge','xbox':'xinput','ns2pro':'nintendo'}[command.split()[-1]]
            assert response['pending_output_mode']==expected,(command,response)
        if command=='status':
            assert response['last_name']=='quote" slash\\ line\n'
            assert response['encrypted'] and response['pairing_key_saved'] and response['persist_pending']
            assert response['persist_error']==-123
    (work/'result.json').write_text(json.dumps({'module':str(args.module),'commands':commands,'passed':True},indent=2))
    print(f'PASS: actual USB descriptors + pinned DWC2 allocation, deferred feedback/epoch/expiry/latest-stop, JSON escaping, {len(commands)} module/runtime commands')


if __name__=='__main__':
    main()
