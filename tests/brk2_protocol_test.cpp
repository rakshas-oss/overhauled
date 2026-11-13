// SPDX-License-Identifier: MIT
#include "brk2_protocol.h"

#include <cassert>
#include <cstddef>
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
std::vector<uint8_t> request_body_with_field_count(std::size_t target_count){
    auto body=request_body();
    constexpr std::size_t base_fields=8;
    assert(target_count>=base_fields && target_count<=4096);
    for(std::size_t i=0;i<target_count-base_fields;++i){
        const uint16_t id=static_cast<uint16_t>(1000+i);
        u16(body,id);body.push_back(1);body.push_back(0);u32(body,1);body.push_back(0x5a);
    }
    const uint32_t payload_size=static_cast<uint32_t>(body.size()-32);
    body[28]=static_cast<uint8_t>(payload_size>>24);
    body[29]=static_cast<uint8_t>(payload_size>>16);
    body[30]=static_cast<uint8_t>(payload_size>>8);
    body[31]=static_cast<uint8_t>(payload_size);
    return body;
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

    // C++ and Rust both cap BRK2 messages at 4096 TLV fields.
    ProtocolLimits field_limits;
    field_limits.max_frame_bytes=1024*1024;
    field_limits.max_payload_bytes=1024*1024;
    auto at_limit=request_body_with_field_count(4096);
    assert(decode_brk2_request_body(at_limit,&request,&id,&error,field_limits));
    auto over_limit=request_body_with_field_count(4097);
    assert(!decode_brk2_request_body(over_limit,&request,&id,&error,field_limits));
    assert(error=="too many BRK2 TLV fields");

    BrokerResponse response;response.task_id=request.task_id;response.status=TaskStatus::Ok;
    response.selected_gpu=-1;response.latency_ms=4;response.result=request.payload;
    const auto encoded=encode_brk2_response_body(response,id,limits);
    assert(encoded.size()>=32);
    assert(encoded[0]=='B'&&encoded[1]=='R'&&encoded[2]=='K'&&encoded[3]=='2');
    assert(encoded[8]==0&&encoded[9]==4);
    assert(encoded[12]==1&&encoded[19]==8); // response preserves request correlation id

    auto malformed=body;malformed[26]=1;
    assert(!decode_brk2_request_body(malformed,&request,&id,&error,limits));
    auto truncated=body;truncated.pop_back();
    assert(!decode_brk2_request_body(truncated,&request,&id,&error,limits));
    auto oversized=limits;oversized.max_frame_bytes=32;
    assert(!decode_brk2_request_body(body,&request,&id,&error,oversized));
    // Deterministic mutation corpus: every byte/bit flip and every truncation
    // must be rejected safely or decoded, never crash or read out of bounds.
    for (std::size_t cut = 0; cut < body.size(); ++cut) {
        std::vector<uint8_t> truncated_body(body.begin(), body.begin() + cut);
        BrokerRequest fuzz_request; uint64_t fuzz_id = 0; std::string fuzz_error;
        (void)decode_brk2_request_body(truncated_body, &fuzz_request, &fuzz_id, &fuzz_error, limits);
    }
    for (std::size_t index = 0; index < body.size(); ++index) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto mutated = body;
            mutated[index] ^= static_cast<uint8_t>(1u << bit);
            BrokerRequest fuzz_request; uint64_t fuzz_id = 0; std::string fuzz_error;
            (void)decode_brk2_request_body(mutated, &fuzz_request, &fuzz_id, &fuzz_error, limits);
        }
    }
    std::cout<<"brk2_protocol_test passed\\n";
}
