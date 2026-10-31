#include "manager_protocol.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int calls;
static uint8_t selected;
static void interfere();
int64_t mock_time = 100;
namespace ns2 {
size_t usb_execute_manager_command(const char *command, char *out, size_t capacity) {
    ++calls;
    if (std::strcmp(command, "interleave") == 0) interfere();
    const std::string result = std::strcmp(command, "large") == 0 ? std::string(3000, 'x') : std::string(command);
    std::snprintf(out, capacity, "%s", result.c_str());
    return result.size();
}
}
using namespace ns2::manager;
static StatusCode select_mode(uint8_t mode) { selected = mode; return StatusCode::Ok; }
static void le16(uint8_t *p, unsigned n) { p[0]=n; p[1]=n>>8; }
static unsigned u16(const uint8_t *p) { return p[0] | unsigned(p[1])<<8; }
static std::array<uint8_t,63> fragment(unsigned op, const std::string &body, unsigned offset=0, unsigned count=41, unsigned tx=42) {
    std::array<uint8_t,63> f{};
    count=std::min<unsigned>(count,body.size()-offset);
    std::memcpy(f.data(),"NSM1",4); f[4]=1; f[5]=1; f[7]=(offset+count<body.size());
    f[8]=tx; le16(f.data()+12,op); le16(f.data()+16,body.size()); le16(f.data()+18,offset);
    f[20]=count; f[21]=22; std::memcpy(f.data()+22,body.data()+offset,count); return f;
}
static void send(const std::array<uint8_t,63>& f) { receive_feature_request(f.data(),f.size(),{},select_mode); }
static std::vector<uint8_t> reply(unsigned status=0, unsigned tx=42) {
    std::vector<uint8_t> all; std::array<uint8_t,63> r{};
    do {
        assert(build_feature_response(r.data(),r.size())==r.size());
        assert(std::memcmp(r.data(),"NSM1",4)==0 && r[6]==1 && r[21]==22);
        assert(r[8]==tx);
        assert(u16(r.data()+14)==status); assert(u16(r.data()+18)==all.size());
        all.insert(all.end(),r.begin()+22,r.begin()+22+r[20]);
        assert(all.size()<=u16(r.data()+16));
    } while(r[7]&1);
    assert(all.size()==u16(r.data()+16)); return all;
}
static void interfere() {
    auto bad=fragment(4,"competing",0,41,99);bad[0]='!';send(bad);
    reply(8); // malformed competing request cannot replace the active request
}
int main() {
    init();
    send(fragment(0x10,"status")); assert(calls==0); reply(8); task();
    auto r=reply(); assert(std::string(r.begin(),r.end())=="status" && calls==1);
    std::string text(512,'a');
    for(unsigned offset=0;offset<text.size();offset+=41) send(fragment(4,text,offset));
    task(); r=reply(); assert(std::string(r.begin(),r.end())==text);
    send(fragment(0x10,"large")); task(); r=reply(); assert(r.size()==3000);
    send(fragment(4,text,41)); reply(4);
    send(fragment(4,text,0)); mock_time+=2000001; send(fragment(4,text,41)); reply(4);
    send(fragment(4,text,0)); auto bad=fragment(4,text,41); bad[8]=43; send(bad); reply(4,43);
    bad=fragment(4,"ok"); bad[7]=1; send(bad); reply(4);
    bad=fragment(4,"ok"); le16(bad.data()+16,513); send(bad); reply(5);
    bad=fragment(4,"ok"); bad[4]=2; send(bad); reply(2);
    bad=fragment(4,"ok"); bad[21]=21; send(bad); reply(1);
    send(fragment(0xffff,"")); task(); reply(3);
    send(fragment(0x10,std::string("a\0b",3))); task(); reply(7);
    for(uint8_t mode=0;mode<4;mode++) {
        send(fragment(5,std::string({char(0xa1),0,char(mode)}))); assert(selected!=9); task(); reply(); assert(selected==mode);
    }
    send(fragment(5,std::string({char(0xa1),0,4}))); task(); reply(7);
    send(fragment(1,"")); task(); assert(reply().size()>32);
    send(fragment(2,"")); task(); assert(reply().size()>8);
    send(fragment(1,"unexpected")); task(); reply(7);
    // Pending and executing commands retain their transaction under interference.
    send(fragment(0x10,"interleave"));
    auto competing=fragment(4,"competing",0,41,99);competing[0]='!';send(competing);
    reply(8);task();r=reply();assert(std::string(r.begin(),r.end())=="interleave");
    // An unread response is never silently replaced by another client.
    send(fragment(4,"unread"));task();send(fragment(4,"competing",0,41,99));
    std::array<uint8_t,22> short_get{};assert(build_feature_response(short_get.data(),short_get.size())==0);
    r=reply();assert(std::string(r.begin(),r.end())=="unread");
    send(fragment(4,"next",0,41,43));task();r=reply(0,43);assert(std::string(r.begin(),r.end())=="next");
    // A malformed continuation resets assembly, and cannot later resume it.
    send(fragment(4,text,0));bad=fragment(4,text,41);bad[7]=0;send(bad);reply(4);
    send(fragment(4,text,41));reply(4);send(fragment(4,"recovered"));task();reply();
    // A newly queued command cannot execute twice.
    const int before=calls; send(fragment(0x10,"status")); task(); task(); reply(); assert(calls==before+1);
    puts("PASS NSM1: deferred execution, fragmentation, bounds, timeout, transaction/order, 4 roles, large response, concurrent interference, unread reply, malformed recovery");
}
