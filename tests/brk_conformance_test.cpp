// SPDX-License-Identifier: MIT
#include "brk2_protocol.h"
#include "broker_protocol.h"
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifndef BRK_CONFORMANCE_CORPUS
#error "BRK_CONFORMANCE_CORPUS must point to the shared corpus"
#endif
namespace {
std::vector<uint8_t> unhex(const std::string& s) {
    assert(s.size() % 2 == 0);
    std::vector<uint8_t> out;
    for (std::size_t i=0;i<s.size();i+=2) out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i,2),nullptr,16)));
    return out;
}
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out; std::stringstream ss(line); std::string item;
    while(std::getline(ss,item,'|')) out.push_back(item);
    return out;
}
}
int main() {
    using namespace nvlink::broker;
    std::ifstream input(BRK_CONFORMANCE_CORPUS);
    assert(input && "shared BRK conformance corpus must be readable");
    ProtocolLimits limits; std::string line; std::size_t cases=0;
    while(std::getline(input,line)) {
        if(line.empty()||line[0]==';') continue;
        const auto c=split(line); assert(c.size()==5);
        const auto& id=c[0]; const auto& protocol=c[1]; const auto& direction=c[2];
        const bool expected=c[3]=="accept"; const auto body=unhex(c[4]);
        bool accepted=false; std::string error;
        if(protocol=="BRK1"&&direction=="request") {
            BrokerRequest r; accepted=decode_request_frame(body,&r,&error,limits);
            if(id=="brk1-request-valid"&&accepted) {
                assert(r.task_id=="t"&&r.source=="y"&&r.destination=="o"&&r.kind=="k");
                assert(r.priority==1&&r.timeout_ms==3000&&(r.payload==std::vector<uint8_t>{0xaa,0xbb}));
                assert(encode_request_frame(r,limits)==body);
            }
        } else if(protocol=="BRK1"&&direction=="response") {
            BrokerResponse r; accepted=decode_response_frame(body,&r,&error,limits);
            if(id=="brk1-response-valid"&&accepted) {
                assert(r.task_id=="t-42"&&r.status==TaskStatus::Ok&&r.selected_gpu==1&&r.latency_ms==9);
                assert((r.result==std::vector<uint8_t>{'a','b','c'}));
            }
        } else if(protocol=="BRK2"&&direction=="request") {
            BrokerRequest r; uint64_t request_id=0;
            accepted=decode_brk2_request_body(body,&r,&request_id,&error,limits);
            if(id=="brk2-request-valid"&&accepted) {
                assert(request_id==0x0102030405060708ULL);
                assert(r.task_id=="conformance-001"&&r.source=="yukki"&&r.destination=="overhauled");
                assert(r.kind=="inference"&&r.priority==7&&r.timeout_ms==2500);
                assert((r.payload==std::vector<uint8_t>{0,1,2,255}));
            }
        } else { std::cerr<<"unsupported corpus row: "<<id<<'\n'; return 2; }
        if(accepted!=expected) {
            std::cerr<<id<<": expected "<<c[3]<<", got "<<(accepted?"accept":"reject")<<" ("<<error<<")\n";
            return 1;
        }
        ++cases;
    }
    assert(cases==17);
    std::cout<<"brk_conformance_test passed: "<<cases<<" cases\n";
    return 0;
}
