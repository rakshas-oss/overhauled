// SPDX-License-Identifier: MIT
#include "brk2_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nvlink::broker {
namespace {
constexpr uint16_t MSG_REQUEST = 3;
constexpr uint16_t MSG_RESPONSE = 4;
constexpr uint16_t FLAG_RESPONSE = 1;
constexpr uint16_t FLAG_END_STREAM = 4;
constexpr uint8_t WT_U8 = 1, WT_U32 = 3, WT_U64 = 4, WT_I64 = 5, WT_UTF8 = 8, WT_BYTES = 9, WT_UUID = 12;
constexpr uint8_t FIELD_CRITICAL = 1;
struct FieldView { uint16_t id; uint8_t type; uint8_t flags; const uint8_t* data; std::size_t size; };

uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]); }
uint32_t read32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t read64(const uint8_t* p) { uint64_t v=0; for(int i=0;i<8;++i) v=(v<<8)|p[i]; return v; }
void put16(std::vector<uint8_t>& b,uint16_t v){b.push_back(uint8_t(v>>8));b.push_back(uint8_t(v));}
void put32(std::vector<uint8_t>& b,uint32_t v){for(int i=3;i>=0;--i)b.push_back(uint8_t(v>>(i*8)));}
void put64(std::vector<uint8_t>& b,uint64_t v){for(int i=7;i>=0;--i)b.push_back(uint8_t(v>>(i*8));}
void put_field(std::vector<uint8_t>& p,uint16_t id,uint8_t type,const uint8_t* data,std::size_t n,uint8_t flags=0){
    if(n>std::numeric_limits<uint32_t>::max()) throw std::runtime_error("BRK2 field too large");
    put16(p,id);p.push_back(type);p.push_back(flags);put32(p,static_cast<uint32_t>(n));
    if(n) p.insert(p.end(),data,data+n);
}
void field_str(std::vector<uint8_t>& p,uint16_t id,const std::string& s){put_field(p,id,WT_UTF8,reinterpret_cast<const uint8_t*>(s.data()),s.size());}
void field_u8(std::vector<uint8_t>& p,uint16_t id,uint8_t v){put_field(p,id,WT_U8,&v,1);}
void field_u64(std::vector<uint8_t>& p,uint16_t id,uint64_t v){std::vector<uint8_t>b;put64(b,v);put_field(p,id,WT_U64,b.data(),b.size());}
void field_i64(std::vector<uint8_t>& p,uint16_t id,int64_t v){std::vector<uint8_t>b;put64(b,static_cast<uint64_t>(v));put_field(p,id,WT_I64,b.data(),b.size());}
void field_bytes(std::vector<uint8_t>& p,uint16_t id,const std::vector<uint8_t>& v){put_field(p,id,WT_BYTES,v.data(),v.size());}

bool parse_fields(const std::vector<uint8_t>& body,std::vector<FieldView>* fields,std::string* error){
    std::size_t pos=BRK2_HEADER_BYTES;
    while(pos<body.size()){
        if(body.size()-pos<8){if(error)*error="truncated BRK2 TLV header";return false;}
        FieldView f{read16(body.data()+pos),body[pos+2],body[pos+3],nullptr,read32(body.data()+pos+4)};
        pos+=8;
        if(f.flags & ~uint8_t(3)){if(error)*error="invalid BRK2 field flags";return false;}
        if(f.size>body.size()-pos){if(error)*error="truncated BRK2 TLV value";return false;}
        f.data=body.data()+pos;pos+=f.size;
        if(std::any_of(fields->begin(),fields->end(),[&](const FieldView& x){return x.id==f.id;})){
            if(error)*error="duplicate BRK2 singular field";return false;
        }
        fields->push_back(f);
    }
    return true;
}
const FieldView* find(const std::vector<FieldView>& fs,uint16_t id){for(const auto& f:fs)if(f.id==id)return &f;return nullptr;}
bool get_string(const std::vector<FieldView>& fs,uint16_t id,std::string* out,std::string* error){
    const auto* f=find(fs,id);if(!f||f->type!=WT_UTF8){if(error)*error="missing or wrong-type BRK2 string field "+std::to_string(id);return false;}
    out->assign(reinterpret_cast<const char*>(f->data),f->size);
    if(out->empty()){if(error)*error="empty BRK2 required string field "+std::to_string(id);return false;}
    return true;
}
bool get_fixed(const std::vector<FieldView>& fs,uint16_t id,uint8_t type,std::size_t n,const uint8_t** data,std::string* error){
    const auto* f=find(fs,id);if(!f||f->type!=type||f->size!=n){if(error)*error="missing or invalid BRK2 field "+std::to_string(id);return false;}*data=f->data;return true;
}
std::vector<uint8_t> envelope(uint16_t type,uint16_t flags,uint64_t request_id,uint32_t schema,const std::vector<uint8_t>& payload){
    std::vector<uint8_t> b;b.reserve(BRK2_HEADER_BYTES+payload.size());
    put32(b,BRK2_MAGIC);b.push_back(BRK2_MAJOR);b.push_back(0);put16(b,BRK2_HEADER_BYTES);
    put16(b,type);put16(b,flags);put64(b,request_id);put32(b,schema);put16(b,1);put16(b,0);
    put32(b,static_cast<uint32_t>(payload.size()));b.insert(b.end(),payload.begin(),payload.end());return b;
}
} // namespace

bool decode_brk2_request_body(const std::vector<uint8_t>& body,BrokerRequest* out,uint64_t* request_id,std::string* error,const ProtocolLimits& limits){
    auto fail=[&](const char* s){if(error)*error=s;return false;};
    if(!out||!request_id)return fail("null BRK2 output");
    if(body.size()<BRK2_HEADER_BYTES||body.size()>limits.max_frame_bytes)return fail("invalid BRK2 frame size");
    if(read32(body.data())!=BRK2_MAGIC)return fail("invalid BRK2 magic");
    if(body[4]!=2)return fail("unsupported BRK2 major version");
    if(read16(body.data()+6)!=BRK2_HEADER_BYTES)return fail("invalid BRK2 header length");
    if(read16(body.data()+8)!=MSG_REQUEST)return fail("unexpected BRK2 message type");
    const uint16_t flags=read16(body.data()+10);
    if(flags & ~uint16_t(0x00ff))return fail("invalid BRK2 message flags");
    if(read16(body.data()+26)!=0)return fail("BRK2 reserved field must be zero");
    const std::size_t payload_len=read32(body.data()+28);
    if(payload_len>limits.max_payload_bytes||payload_len!=body.size()-BRK2_HEADER_BYTES)return fail("BRK2 payload length mismatch or limit exceeded");
    if(read32(body.data()+20)!=BRK2_SCHEMA_REQUEST||read16(body.data()+24)!=1)return fail("unsupported BRK2 request schema");
    *request_id=read64(body.data()+12);
    std::vector<FieldView> fs;if(!parse_fields(body,&fs,error))return false;
    BrokerRequest req;
    if(!get_string(fs,1,&req.task_id,error)||!get_string(fs,2,&req.source,error)||
       !get_string(fs,3,&req.destination,error)||!get_string(fs,4,&req.kind,error))return false;
    const uint8_t *p=nullptr;
    if(!get_fixed(fs,5,WT_U8,1,&p,error))return false;req.priority=p[0];
    if(!get_fixed(fs,6,WT_U32,4,&p,error))return false;req.timeout_ms=read32(p);
    if(!get_fixed(fs,7,WT_BYTES,find(fs,7)?find(fs,7)->size:0,&p,error))return false;
    const auto* payload=find(fs,7);if(payload->size>limits.max_payload_bytes)return fail("BRK2 request payload exceeds limit");
    req.payload.assign(payload->data,payload->data+payload->size);
    if(const auto* key=find(fs,8)){if(key->type!=WT_UUID||key->size!=16)return fail("invalid BRK2 idempotency key");}
    if(const auto* deadline=find(fs,9)){if(deadline->type!=WT_U64||deadline->size!=8)return fail("invalid BRK2 deadline");}
    if(req.timeout_ms==0||req.timeout_ms>BROKER_MAX_TIMEOUT_MS)return fail("BRK2 timeout_ms outside 1..300000");
    if(body.size()>limits.max_frame_bytes)return fail("BRK2 frame exceeds limit");
    *out=std::move(req);return true;
}

std::vector<uint8_t> encode_brk2_response_body(const BrokerResponse& r,uint64_t request_id,const ProtocolLimits& limits){
    if(r.result.size()>limits.max_payload_bytes)throw std::runtime_error("BRK2 response payload exceeds limit");
    std::vector<uint8_t> p;
    field_str(p,1,r.task_id);field_u8(p,2,static_cast<uint8_t>(r.status));
    field_i64(p,3,static_cast<int64_t>(r.selected_gpu));field_u64(p,4,r.latency_ms);
    field_bytes(p,5,r.result);field_str(p,6,r.error);
    if(p.size()>limits.max_payload_bytes||BRK2_HEADER_BYTES+p.size()>limits.max_frame_bytes)throw std::runtime_error("BRK2 response exceeds frame limit");
    return envelope(MSG_RESPONSE,FLAG_RESPONSE|FLAG_END_STREAM,request_id,BRK2_SCHEMA_RESPONSE,p);
}
} // namespace nvlink::broker
