#include "input/message_context.h"
#include "input/hd_translator.h"
#include <map>
#include <string>
#include <cstdio>
using namespace nwii::runtime::input;
static int checks=0,failures=0;
static void check(bool ok,const char* name){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",name);}
struct Memory {
    std::map<uint32_t,uint8_t> bytes;
    int invalid_reads=0;
    void write32(uint32_t a,uint32_t value){for(int i=0;i<4;++i)bytes[a+i]=uint8_t(value>>(24-i*8));}
    bool valid(uint32_t a,uint32_t n){for(uint32_t i=0;i<n;++i)if(!bytes.contains(a+i))return false;return true;}
    uint8_t read8(uint32_t a){if(!valid(a,1)){++invalid_reads;return 0;}return bytes[a];}
    uint32_t read32(uint32_t a){uint32_t v=0;for(int i=0;i<4;++i)v=(v<<8)|read8(a+i);return v;}
    MessageContext observe(){return read_message_context([&](auto a){return read32(a);},[&](auto a){return read8(a);},[&](auto a,auto n){return valid(a,n);});}
    void state(const std::string& label){
        for(uint32_t i=0;i<0x3C;++i)bytes[0x81000000+i]=0;
        for(uint32_t i=0;i<8;++i)bytes[0x81001000+i]=0;
        write32(0x80575488,0x81000000);write32(0x81000038,0x81001000);write32(0x81001004,0x81002000);
        bytes.erase(bytes.lower_bound(0x81002000),bytes.end());
        for(size_t i=0;i<=label.size();++i)bytes[0x81002000+uint32_t(i)]=uint8_t(label.c_str()[i]);
    }
};
int main(){
    for(const char* name:{"In","OutputText","WaitKeyChangePage0","WaitKeyChangePage1","WaitKeyMsgEnd0",
                         "WaitKeyMsgEnd1","WaitKeyMsgEnd2","WaitKeySelectQuestion","MapOpen","MapClose",
                         "DemoIn","DemoVisible","DemoOut","Out"}) {
        Memory m;m.state(std::string("dLytMsgWindow_c::StateID_")+name);const auto seen=m.observe();
        GameContext c;c.valid=true;c.riding=2;c.message_known=seen.known;c.message_modal=seen.modal;c.event_state=0;
        LiveFlags flags;flags.player=true;
        check(seen.known&&seen.modal&&classify(c,flags)==Situation::Event&&!m.invalid_reads,
              "a real message state uses confirmation instead of flight controls");
    }
    for(const char* name:{"Invisible","ExplainIn","ExplainVisible","ExplainOut"}) {
        Memory m;m.state(std::string("dLytMsgWindow_c::StateID_")+name);const auto seen=m.observe();
        GameContext c;c.valid=true;c.riding=2;c.message_known=seen.known;c.message_modal=seen.modal;
        LiveFlags flags;flags.player=true;
        check(seen.known&&!seen.modal&&classify(c,flags)==Situation::Flight&&!m.invalid_reads,
              "hidden messages and passive instructions preserve flight controls");
    }
    Memory missing;missing.write32(0x80575488,0);
    check(!missing.observe().known&&!missing.invalid_reads,"absent message window is never dereferenced");
    Memory wrong;wrong.state("SomeOtherClass::StateID_OutputText");
    check(!wrong.observe().known&&!wrong.invalid_reads,"unrelated state names cannot steal gameplay input");
    Memory damaged;damaged.state("dLytMsgWindow_c::StateID_OutputText");damaged.write32(0x81001004,0xFFFFFFFF);
    check(!damaged.observe().known&&!damaged.invalid_reads,"invalid state name pointers are rejected before reading");
    Memory unknown;unknown.state("dLytMsgWindow_c::StateID_Unknown");
    check(!unknown.observe().known&&!unknown.invalid_reads,"unknown states preserve the existing event rules");
    std::printf("RESULT %d/%d checks passed\n",checks-failures,checks);return failures?1:0;
}
