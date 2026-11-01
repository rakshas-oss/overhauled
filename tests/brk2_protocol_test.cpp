// SPDX-License-Identifier: MIT
#include "brk2_protocol.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
void u16(std::vector<uint8_t>& b,uint16_t v){b.push_back(uint8_t(v>>8));b.push_back(uint8_t(v));}
void u32(std::vector<uint8_t>& b,uint32_t v){for(int i=3;i>=0;--i)b.push_back(uint8_t(v>>(i*8)));}
void u64(std::vector<uint8_t>& b,uint64_t v){for(int i=7;i>=0;--i)b.push_back(uint8_t(v>>(i*8)));}
void field(std::vector<uint8_t>& p,uint16_t id,uint8_t wt,const std::vector<uint8_t>& v,uint8_t flags=1){
    u16(p,id);p.push_back(wt);p.push_back(flags);u32(p,static_cast<uint32_t>(v.size()));p.insert(p.end(),v.begin(),v.end());
}
std::vector<uint8_t> str(const char* s){return std::vector<uint8_t>(s,s+std::char_traits<char>::length(s));}
std::vector<uint8_t> request_body(){
    std::vector<uint8_t> p;
    field(p,1,8,str("interop-1"));field(p,2,8,str("yukki"));field(p,3,8,str("overhauled"));field(p,4,8,str("inference"));
    field(p,5,1,{7});std::vector<uint8_t> t;u32(t,2500);field(p,6,3,t);
    field(p,7,9,{0,1,2,255});std::vector<uint8_t> d;u64(d,1900000000000000000ULL);field(p,9,4,d,0);
    std::vector<uint8_t> b;u32(b,0x42524b32);b.push_back(2);b.push_back(0);u16(b,32);u16(b,3);u16(b,0);
    u64(b,0x0102030405060708ULL);u32(b,1);u16(b,1);u16(b,0);u32(b,static_cast<uint32_t>(p.size()));
    b.insert(b.end(),p.begin(),p.end());return b;
}
}
int main(){
    using namespace nvlink::broker;
    ProtocolLimits limits;limits.max_frame_bytes=4096;limits.max_payload_bytes=1024;
    const auto body=request_body();
    BrokerRequest request;uint64_t id=0;std::string error;
    assert(decode_brk2_request_body(body,&request,&id,&error,limits));
    assert(id==0x0102030405060708ULL);
    assert(request.task_id=="interop-1"&&request.source=="yukki"&&request.destination=="overhauled");
    assert(request.kind=="inference"&&request.priority==7&&request.timeout_ms==2500);
    assert((request.payload==std::vector<uint8_t>{0,1,2,255}));

    BrokerResponse response;response.task_id=request.task_id;response.status=TaskStatus::Ok;
    response.selected_gpu=-1;response.latency_ms=4;response.result=request.payload;
    const auto encoded=encode_brk2_response_body(response,id,limits);
    assert(encoded.size()>=32);
    assert(encoded[0]=='B'&&encoded[1]=='R'&&encoded[2]=='K'&&encoded[3]=='2');
    assert(encoded[8]==0&&encoded[9]==4);
    assert(encoded[12]==1&&encoded[19]==8); // response preserves request correlation id

    auto malformed=body;malformed[30]=1;
    assert(!decode_brk2_request_body(malformed,&request,&id,&error,limits));
    auto truncated=body;truncated.pop_back();
    assert(!decode_brk2_request_body(truncated,&request,&id,&error,limits));
    auto oversized=limits;oversized.max_frame_bytes=32;
    assert(!decode_brk2_request_body(body,&request,&id,&error,oversized));
    std::cout<<"brk2_protocol_test passed\n";
}
