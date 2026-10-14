#include "multiplayer/player_context.h"
#include <map>
#include <cstdio>
#include <stdexcept>
using namespace nwii::runtime::multiplayer;
struct Memory {
 std::map<uint32_t,uint32_t> words;
 uint32_t failed_read=0,failed_write=0;
 unsigned writes=0;
 GuestGlobalsIO io(){return {this,
  [](void* u,uint32_t a,uint32_t& v)noexcept {auto& m=*static_cast<Memory*>(u);
   if(a==m.failed_read || !m.words.contains(a))return false;v=m.words[a];return true;},
  [](void* u,uint32_t a,uint32_t v)noexcept {auto& m=*static_cast<Memory*>(u);++m.writes;
   if(a==m.failed_write){m.failed_write=0;return false;}m.words[a]=v;return true;}};}
};
int main(){
 unsigned checks=0,failed=0;
 auto check=[&](bool b,const char* name){++checks;if(!b){++failed;std::printf("FAIL %s\n",name);}};
 Memory m;for(auto a:PlayerContextScope::locations)m.words[a]=a+4;
 for(unsigned i=0;i<4;++i)m.words[0x805CB0B8+4*i]=0x80A00000+i*0x100;
 const auto original=m.words;
 {
  PlayerContextScope host;check(host.enter(m.io(),0x80B00000,0),"host context enters");
  const auto host_words=m.words;
  {
   PlayerContextScope client;check(client.enter(m.io(),0x80C00000,1),"client context enters");
   check(m.words[0x8057578C]==0x80C00000 && m.words[0x805757B8]==0x80C00000,"both player aliases belong to client");
   check(m.words[0x80571C60]==0x80599D5C && m.words[0x80575C54]==1 && m.words[0x80575C58]==0x80A00100,"pad, channel and Core switch together");
   check(!client.enter(m.io(),0x80D00000,2),"active scope refuses a second enter");
  }
  check(m.words==host_words,"nested scope restores host globals");
 }
 check(m.words==original,"host scope restores original engine globals");
 try {PlayerContextScope scope;check(scope.enter(m.io(),0x80C00000,1),"exception path enters");throw std::runtime_error("test");}
 catch(...){}
 check(m.words==original,"exception unwinding restores globals");
 for(auto a:PlayerContextScope::locations){
  PlayerContextScope scope;m.failed_read=a;auto writes=m.writes;
  check(!scope.enter(m.io(),0x80C00000,1) && m.writes==writes && m.words==original,"failed snapshot makes no writes");m.failed_read=0;
 }
 for(auto a:PlayerContextScope::locations){
  PlayerContextScope scope;m.failed_write=a;
  check(!scope.enter(m.io(),0x80C00000,1) && m.words==original && !scope.active(),"partial enter rolls back");
 }
 {
  PlayerContextScope scope;check(scope.enter(m.io(),0x80C00000,1),"restore retry setup");
  m.failed_write=0x8057578C;check(!scope.leave() && scope.active(),"restore failure prevents resuming guest");
  check(scope.leave() && m.words==original && !scope.active(),"restore retry completes");
 }
 for(auto actor:{0u,0x80B00001u,0x81800000u}){
  PlayerContextScope scope;auto writes=m.writes;
  check(!scope.enter(m.io(),actor,1) && m.writes==writes,"invalid actor pointer rejected");
 }
 PlayerContextScope bad;check(!bad.enter(m.io(),0x80C00000,4),"invalid controller channel rejected");
 m.words[0x805CB0BC]=0;auto writes=m.writes;
 check(!bad.enter(m.io(),0x80C00000,1) && m.writes==writes,"missing controller Core rejected");
 std::printf("RESULT %u/%u\n",checks-failed,checks);return failed?1:0;
}
