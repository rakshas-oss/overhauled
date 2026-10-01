#include "broker_protocol.h"
#include "broker_server.h"
#include "broker_service.h"
#include "wasm_sandbox.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Assertion failed: " << message << "\n";
        std::abort();
    }
}

std::vector<uint8_t> pack_doubles(const std::vector<double>& values) {
    std::vector<uint8_t> out(values.size() * sizeof(double));
    std::memcpy(out.data(), values.data(), out.size());
    return out;
}

std::vector<double> unpack_doubles(const std::vector<uint8_t>& bytes) {
    std::vector<double> out(bytes.size() / sizeof(double));
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

void test_task_wire_format() {
    using namespace nvlink::wasm;

    WasmTaskRequest req;
    req.task_id = "task-001";
    req.sandbox_id = "sandbox-alpha";
    req.module_id = "mod-inference";
    req.module_version = "1.2.3";
    req.task_kind = "inference";
    req.priority = 10;
    req.deadline_ms = 1800000000000ULL;

    WasmBufferDescriptor buf1;
    buf1.buffer_id = 1;
    buf1.flags = static_cast<uint32_t>(WasmBufferAccess::Read);
    buf1.offset = 0;
    buf1.length = 1024;
    buf1.name = "input_tensor";

    WasmBufferDescriptor buf2;
    buf2.buffer_id = 2;
    buf2.flags = static_cast<uint32_t>(WasmBufferAccess::Write);
    buf2.offset = 1024;
    buf2.length = 512;
    buf2.name = "output_tensor";

    req.buffer_descriptors = {buf1, buf2};
    req.payload = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

    std::vector<uint8_t> encoded;
    std::string error;
    const bool enc_ok = encode_wasm_task_request(req, &encoded, &error);
    require(enc_ok, "encode_wasm_task_request failed");
    require(!encoded.empty(), "encoded frame empty");

    WasmTaskRequest decoded;
    const bool dec_ok = decode_wasm_task_request(encoded, &decoded, &error);
    require(dec_ok, "decode_wasm_task_request failed");
    require(decoded.task_id == req.task_id, "task_id mismatch");
    require(decoded.sandbox_id == req.sandbox_id, "sandbox_id mismatch");
    require(decoded.module_id == req.module_id, "module_id mismatch");
    require(decoded.module_version == req.module_version, "module_version mismatch");
    require(decoded.task_kind == req.task_kind, "task_kind mismatch");
    require(decoded.priority == req.priority, "priority mismatch");
    require(decoded.deadline_ms == req.deadline_ms, "deadline_ms mismatch");
    require(decoded.buffer_descriptors.size() == 2, "buffer_descriptors count mismatch");
    require(decoded.buffer_descriptors[0].buffer_id == 1, "buf1 id mismatch");
    require(decoded.buffer_descriptors[0].name == "input_tensor", "buf1 name mismatch");
    require(decoded.buffer_descriptors[1].buffer_id == 2, "buf2 id mismatch");
    require(decoded.buffer_descriptors[1].name == "output_tensor", "buf2 name mismatch");
    require(decoded.payload == req.payload, "payload mismatch");

    // Truncated frame
    std::vector<uint8_t> truncated = encoded;
    truncated.resize(truncated.size() - 2);
    WasmTaskRequest bad_req;
    const bool trunc_ok = decode_wasm_task_request(truncated, &bad_req, &error);
    require(!trunc_ok, "truncated decode should fail");

    // Corrupted magic
    std::vector<uint8_t> bad_magic = encoded;
    bad_magic[0] ^= 0xFF;
    const bool magic_ok = decode_wasm_task_request(bad_magic, &bad_req, &error);
    require(!magic_ok, "bad magic decode should fail");

    // Trailing bytes
    std::vector<uint8_t> trailing = encoded;
    trailing.push_back(0x00);
    const bool trail_ok = decode_wasm_task_request(trailing, &bad_req, &error);
    require(!trail_ok, "trailing bytes decode should fail");

    // Test Response wire format
    WasmTaskResponse resp;
    resp.task_id = "task-001";
    resp.status = WasmTaskStatus::Ok;
    resp.protocol_version = WASM_PROTOCOL_VERSION;
    resp.selected_gpu = 1;
    resp.latency_ms = 42;
    resp.buffer_descriptors = req.buffer_descriptors;
    resp.result = {0x01, 0x02, 0x03, 0x04};
    resp.error = "";

    std::vector<uint8_t> enc_resp;
    const bool enc_resp_ok = encode_wasm_task_response(resp, &enc_resp, &error);
    require(enc_resp_ok, "encode_wasm_task_response failed");

    WasmTaskResponse dec_resp;
    const bool dec_resp_ok = decode_wasm_task_response(enc_resp, &dec_resp, &error);
    require(dec_resp_ok, "decode_wasm_task_response failed");
    require(dec_resp.task_id == resp.task_id, "resp task_id mismatch");
    require(dec_resp.status == WasmTaskStatus::Ok, "resp status mismatch");
    require(dec_resp.protocol_version == WASM_PROTOCOL_VERSION, "resp protocol_version mismatch");
    require(dec_resp.selected_gpu == 1, "resp selected_gpu mismatch");
    require(dec_resp.latency_ms == 42, "resp latency mismatch");
    require(dec_resp.result == resp.result, "resp result mismatch");
    require(dec_resp.buffer_descriptors.size() == 2, "resp buffers size mismatch");

    // Task request validation edge cases: empty fields must fail
    {
        WasmTaskRequest empty_req = req;
        empty_req.task_id = "";
        std::vector<uint8_t> dummy;
        require(!encode_wasm_task_request(empty_req, &dummy, &error), "empty task_id should fail");

        empty_req = req;
        empty_req.sandbox_id = "";
        require(!encode_wasm_task_request(empty_req, &dummy, &error), "empty sandbox_id should fail");

        empty_req = req;
        empty_req.module_id = "";
        require(!encode_wasm_task_request(empty_req, &dummy, &error), "empty module_id should fail");

        empty_req = req;
        empty_req.module_version = "";
        require(!encode_wasm_task_request(empty_req, &dummy, &error), "empty module_version should fail");

        empty_req = req;
        empty_req.task_kind = "";
        require(!encode_wasm_task_request(empty_req, &dummy, &error), "empty task_kind should fail");
    }

    // Limits enforcement
    {
        WasmLimits small_limits;
        small_limits.max_payload_bytes = 4;
        std::vector<uint8_t> dummy;
        require(!encode_wasm_task_request(req, &dummy, &error, small_limits),
                "payload exceeding max_payload_bytes should fail");

        small_limits = WasmLimits{};
        small_limits.max_buffers = 1;
        require(!encode_wasm_task_request(req, &dummy, &error, small_limits),
                "buffer count exceeding max_buffers should fail");
    }

    // Response decode error handling: bad magic, truncated, trailing bytes
    {
        std::vector<uint8_t> bad_resp = enc_resp;
        bad_resp[0] ^= 0xFF;
        WasmTaskResponse dummy_resp;
        require(!decode_wasm_task_response(bad_resp, &dummy_resp, &error), "bad magic in response decode should fail");

        bad_resp = enc_resp;
        bad_resp.resize(bad_resp.size() - 2);
        require(!decode_wasm_task_response(bad_resp, &dummy_resp, &error), "truncated response decode should fail");

        bad_resp = enc_resp;
        bad_resp.push_back(0xFF);
        require(!decode_wasm_task_response(bad_resp, &dummy_resp, &error), "trailing bytes in response decode should fail");
    }

    std::cout << "  test_task_wire_format passed\n";
}

void test_lifecycle_wire_format() {
    using namespace nvlink::wasm;

    WasmLifecycleRequest req;
    req.action = WasmLifecycleAction::Prepare;
    req.request_id = "req-123";
    req.sandbox_id = "sandbox-beta";
    req.module_id = "mod-nlp";
    req.module_version = "2.0.0";
    req.target_gpu = 2;
    req.grace_period_ms = 5000;
    req.ack_token = "token-xyz";
    req.payload = {0x11, 0x22};

    std::vector<uint8_t> encoded;
    std::string error;
    const bool enc_ok = encode_wasm_lifecycle_request(req, &encoded, &error);
    require(enc_ok, "encode_wasm_lifecycle_request failed");

    WasmLifecycleRequest dec;
    const bool dec_ok = decode_wasm_lifecycle_request(encoded, &dec, &error);
    require(dec_ok, "decode_wasm_lifecycle_request failed");
    require(dec.action == WasmLifecycleAction::Prepare, "action mismatch");
    require(dec.request_id == req.request_id, "request_id mismatch");
    require(dec.sandbox_id == req.sandbox_id, "sandbox_id mismatch");
    require(dec.module_id == req.module_id, "module_id mismatch");
    require(dec.module_version == req.module_version, "module_version mismatch");
    require(dec.target_gpu == 2, "target_gpu mismatch");
    require(dec.grace_period_ms == 5000, "grace_period_ms mismatch");
    require(dec.ack_token == "token-xyz", "ack_token mismatch");
    require(dec.payload == req.payload, "payload mismatch");

    WasmLifecycleResponse resp;
    resp.action = WasmLifecycleAction::Prepare;
    resp.status = WasmLifecycleStatus::Ok;
    resp.protocol_version = WASM_PROTOCOL_VERSION;
    resp.state = WasmModuleState::Prepared;
    resp.request_id = "req-123";
    resp.sandbox_id = "sandbox-beta";
    resp.module_id = "mod-nlp";
    resp.module_version = "2.0.0";
    resp.assigned_gpu = 2;
    resp.active_tasks = 0;
    resp.lease_token = "lease-xyz-789";
    resp.error = "";

    std::vector<uint8_t> enc_resp;
    const bool enc_resp_ok = encode_wasm_lifecycle_response(resp, &enc_resp, &error);
    require(enc_resp_ok, "encode_wasm_lifecycle_response failed");

    WasmLifecycleResponse dec_resp;
    const bool dec_resp_ok = decode_wasm_lifecycle_response(enc_resp, &dec_resp, &error);
    require(dec_resp_ok, "decode_wasm_lifecycle_response failed");
    require(dec_resp.action == WasmLifecycleAction::Prepare, "resp action mismatch");
    require(dec_resp.status == WasmLifecycleStatus::Ok, "resp status mismatch");
    require(dec_resp.state == WasmModuleState::Prepared, "resp state mismatch");
    require(dec_resp.assigned_gpu == 2, "resp assigned_gpu mismatch");
    require(dec_resp.lease_token == "lease-xyz-789", "resp lease mismatch");

    // Lifecycle validation edge cases: empty IDs must fail
    {
        WasmLifecycleRequest bad = req;
        bad.request_id = "";
        std::vector<uint8_t> dummy;
        require(!encode_wasm_lifecycle_request(bad, &dummy, &error), "empty request_id should fail");

        bad = req;
        bad.sandbox_id = "";
        require(!encode_wasm_lifecycle_request(bad, &dummy, &error), "empty sandbox_id should fail");

        bad = req;
        bad.module_id = "";
        require(!encode_wasm_lifecycle_request(bad, &dummy, &error), "empty module_id should fail");

        bad = req;
        bad.module_version = "";
        require(!encode_wasm_lifecycle_request(bad, &dummy, &error), "empty module_version should fail");
    }

    // Lifecycle decode error handling: bad magic, truncated, trailing bytes
    {
        std::vector<uint8_t> bad = encoded;
        bad[0] ^= 0xFF;
        WasmLifecycleRequest dummy;
        require(!decode_wasm_lifecycle_request(bad, &dummy, &error), "bad magic in lifecycle request decode should fail");

        bad = encoded;
        bad.resize(bad.size() - 2);
        require(!decode_wasm_lifecycle_request(bad, &dummy, &error), "truncated lifecycle request decode should fail");

        bad = encoded;
        bad.push_back(0x00);
        require(!decode_wasm_lifecycle_request(bad, &dummy, &error), "trailing bytes in lifecycle request decode should fail");

        std::vector<uint8_t> bad_resp = enc_resp;
        bad_resp[0] ^= 0xFF;
        WasmLifecycleResponse dummy_resp;
        require(!decode_wasm_lifecycle_response(bad_resp, &dummy_resp, &error), "bad magic in lifecycle response decode should fail");

        bad_resp = enc_resp;
        bad_resp.resize(bad_resp.size() - 1);
        require(!decode_wasm_lifecycle_response(bad_resp, &dummy_resp, &error), "truncated lifecycle response decode should fail");

        bad_resp = enc_resp;
        bad_resp.push_back(0x00);
        require(!decode_wasm_lifecycle_response(bad_resp, &dummy_resp, &error), "trailing bytes in lifecycle response decode should fail");
    }

    // Helper functions and diagnostics
    {
        require(is_wasm_request_kind(kWasmTaskKind), "is_wasm_request_kind task kind");
        require(is_wasm_request_kind(kWasmLifecycleKind), "is_wasm_request_kind lifecycle kind");
        require(is_wasm_request_kind(kWasmLifecyclePrepareKind), "is_wasm_request_kind prep kind");
        require(is_wasm_request_kind(kWasmLifecycleDrainKind), "is_wasm_request_kind drain kind");
        require(is_wasm_request_kind(kWasmLifecycleReleaseKind), "is_wasm_request_kind release kind");
        require(is_wasm_request_kind(kWasmLifecycleQueryKind), "is_wasm_request_kind query kind");
        require(!is_wasm_request_kind("inference"), "is_wasm_request_kind inference should be false");
        require(!is_wasm_request_kind("geospatial.frame.v1"), "is_wasm_request_kind geospatial should be false");

        require(std::string(wasm_task_status_name(WasmTaskStatus::Ok)) == "Ok", "status name Ok");
        require(std::string(wasm_lifecycle_action_name(WasmLifecycleAction::Prepare)) == "Prepare", "action name Prepare");
        require(std::string(wasm_lifecycle_status_name(WasmLifecycleStatus::Ok)) == "Ok", "lifecycle status name Ok");
        require(std::string(wasm_module_state_name(WasmModuleState::Prepared)) == "Prepared", "module state name Prepared");
    }

    std::cout << "  test_lifecycle_wire_format passed\n";
}

void test_hotswap_lifecycle_flow() {
    using namespace nvlink::wasm;

    WasmSandboxManager mgr(/*force_cpu=*/true);

    // 1. Prepare module v1.0.0
    WasmLifecycleRequest prep_v1;
    prep_v1.action = WasmLifecycleAction::Prepare;
    prep_v1.request_id = "prep-1";
    prep_v1.sandbox_id = "sb-hotswap";
    prep_v1.module_id = "mod-vision";
    prep_v1.module_version = "1.0.0";

    const WasmLifecycleResponse prep_v1_resp = mgr.handle_lifecycle(prep_v1);
    require(prep_v1_resp.status == WasmLifecycleStatus::Ok, "prep v1 should succeed");
    require(prep_v1_resp.state == WasmModuleState::Prepared, "prep v1 state should be Prepared");
    require(!prep_v1_resp.lease_token.empty(), "prep v1 should return lease token");
    require(prep_v1_resp.assigned_gpu == -1, "prep v1 assigned GPU should be -1 in force_cpu");

    // 2. Submit task on v1.0.0
    WasmTaskRequest task_v1;
    task_v1.task_id = "task-v1-1";
    task_v1.sandbox_id = "sb-hotswap";
    task_v1.module_id = "mod-vision";
    task_v1.module_version = "1.0.0";
    task_v1.task_kind = "inference";
    task_v1.priority = 1;
    task_v1.payload = pack_doubles({2.0, 4.0});

    const WasmTaskResponse task_v1_resp = mgr.handle_task(task_v1);
    require(task_v1_resp.status == WasmTaskStatus::Ok, "task v1 should succeed");
    const std::vector<double> out_doubles = unpack_doubles(task_v1_resp.result);
    require(out_doubles.size() == 2, "task v1 result size mismatch");
    require(out_doubles[0] == 4.0 && out_doubles[1] == 8.0, "task v1 computation mismatch");

    // Verify module state is now Active
    require(mgr.get_module_state("sb-hotswap", "mod-vision", "1.0.0") == WasmModuleState::Active,
            "v1 state should be Active after running task");

    // 3. Prepare new module version v2.0.0 (hotswap step 1)
    WasmLifecycleRequest prep_v2;
    prep_v2.action = WasmLifecycleAction::Prepare;
    prep_v2.request_id = "prep-2";
    prep_v2.sandbox_id = "sb-hotswap";
    prep_v2.module_id = "mod-vision";
    prep_v2.module_version = "2.0.0";

    const WasmLifecycleResponse prep_v2_resp = mgr.handle_lifecycle(prep_v2);
    require(prep_v2_resp.status == WasmLifecycleStatus::Ok, "prep v2 should succeed");
    require(prep_v2_resp.state == WasmModuleState::Prepared, "prep v2 state should be Prepared");

    // 4. Drain old module version v1.0.0 (hotswap step 4)
    WasmLifecycleRequest drain_v1;
    drain_v1.action = WasmLifecycleAction::Drain;
    drain_v1.request_id = "drain-1";
    drain_v1.sandbox_id = "sb-hotswap";
    drain_v1.module_id = "mod-vision";
    drain_v1.module_version = "1.0.0";

    const WasmLifecycleResponse drain_v1_resp = mgr.handle_lifecycle(drain_v1);
    require(drain_v1_resp.status == WasmLifecycleStatus::Ok, "drain v1 should succeed");
    require(drain_v1_resp.state == WasmModuleState::Stopped, "drain v1 with 0 inflight should be Stopped");

    // 5. Tasks to old version v1.0.0 must be rejected
    const WasmTaskResponse reject_v1_resp = mgr.handle_task(task_v1);
    require(reject_v1_resp.status != WasmTaskStatus::Ok, "task on drained/stopped v1 should be rejected");

    // 6. Submit task on new module version v2.0.0 (hotswap step 5: switch traffic)
    WasmTaskRequest task_v2;
    task_v2.task_id = "task-v2-1";
    task_v2.sandbox_id = "sb-hotswap";
    task_v2.module_id = "mod-vision";
    task_v2.module_version = "2.0.0";
    task_v2.task_kind = "inference";
    task_v2.priority = 1;
    task_v2.payload = pack_doubles({10.0, 20.0});

    const WasmTaskResponse task_v2_resp = mgr.handle_task(task_v2);
    require(task_v2_resp.status == WasmTaskStatus::Ok, "task v2 should succeed");
    const std::vector<double> out_v2 = unpack_doubles(task_v2_resp.result);
    require(out_v2.size() == 2 && out_v2[0] == 20.0 && out_v2[1] == 40.0, "task v2 computation mismatch");

    // 7. Release resources of old module version v1.0.0 (hotswap step 6)
    WasmLifecycleRequest rel_v1;
    rel_v1.action = WasmLifecycleAction::Release;
    rel_v1.request_id = "rel-1";
    rel_v1.sandbox_id = "sb-hotswap";
    rel_v1.module_id = "mod-vision";
    rel_v1.module_version = "1.0.0";

    const WasmLifecycleResponse rel_v1_resp = mgr.handle_lifecycle(rel_v1);
    require(rel_v1_resp.status == WasmLifecycleStatus::Ok, "release v1 should succeed");

    // Query released module returns NotFound
    WasmLifecycleRequest query_v1;
    query_v1.action = WasmLifecycleAction::Query;
    query_v1.request_id = "q-1";
    query_v1.sandbox_id = "sb-hotswap";
    query_v1.module_id = "mod-vision";
    query_v1.module_version = "1.0.0";
    const WasmLifecycleResponse query_v1_resp = mgr.handle_lifecycle(query_v1);
    require(query_v1_resp.status == WasmLifecycleStatus::NotFound, "query v1 after release should be NotFound");

    std::cout << "  test_hotswap_lifecycle_flow passed\n";
}

void test_drain_with_inflight_tasks() {
    using namespace nvlink::wasm;

    WasmSandboxManager mgr(/*force_cpu=*/true);

    WasmLifecycleRequest prep;
    prep.action = WasmLifecycleAction::Prepare;
    prep.request_id = "prep-inflight";
    prep.sandbox_id = "sb-inflight";
    prep.module_id = "mod-inflight";
    prep.module_version = "1.0.0";
    mgr.handle_lifecycle(prep);

    std::atomic<bool> task_running{false};
    std::atomic<bool> release_attempted{false};
    std::atomic<bool> release_was_busy{false};
    std::atomic<bool> rejected_while_draining{false};

    std::thread worker([&mgr, &task_running]() {
        WasmTaskRequest req;
        req.task_id = "slow-task";
        req.sandbox_id = "sb-inflight";
        req.module_id = "mod-inflight";
        req.module_version = "1.0.0";
        req.task_kind = "inference";

        mgr.handle_task(req, [&task_running](int32_t, const WasmTaskRequest&) {
            task_running.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            return std::vector<uint8_t>{0xCA, 0xFE};
        });
    });

    while (!task_running.load()) {
        std::this_thread::yield();
    }

    // Call drain while task is active
    WasmLifecycleRequest drain;
    drain.action = WasmLifecycleAction::Drain;
    drain.request_id = "drain-active";
    drain.sandbox_id = "sb-inflight";
    drain.module_id = "mod-inflight";
    drain.module_version = "1.0.0";

    const WasmLifecycleResponse drain_resp = mgr.handle_lifecycle(drain);
    require(drain_resp.status == WasmLifecycleStatus::Ok, "drain during task should succeed");
    require(drain_resp.state == WasmModuleState::Draining, "state should be Draining when tasks inflight");
    require(drain_resp.active_tasks >= 1, "active_tasks should be at least 1");

    // Attempting a new task while draining must be rejected with ModuleDraining
    WasmTaskRequest new_task;
    new_task.task_id = "new-task-rejected";
    new_task.sandbox_id = "sb-inflight";
    new_task.module_id = "mod-inflight";
    new_task.module_version = "1.0.0";
    new_task.task_kind = "inference";

    const WasmTaskResponse rej_resp = mgr.handle_task(new_task);
    if (rej_resp.status == WasmTaskStatus::ModuleDraining) {
        rejected_while_draining.store(true);
    }

    // Attempting to release while task is active must be rejected with Busy
    WasmLifecycleRequest rel;
    rel.action = WasmLifecycleAction::Release;
    rel.request_id = "rel-busy";
    rel.sandbox_id = "sb-inflight";
    rel.module_id = "mod-inflight";
    rel.module_version = "1.0.0";

    const WasmLifecycleResponse busy_resp = mgr.handle_lifecycle(rel);
    release_attempted.store(true);
    if (busy_resp.status == WasmLifecycleStatus::Busy) {
        release_was_busy.store(true);
    }

    worker.join();

    require(rejected_while_draining.load(), "new task should be rejected with ModuleDraining");
    require(release_was_busy.load(), "release during active tasks should return Busy");

    // Now that worker joined, state should be Stopped and release should succeed
    const WasmLifecycleResponse final_rel = mgr.handle_lifecycle(rel);
    require(final_rel.status == WasmLifecycleStatus::Ok, "release after worker completion should succeed");

    std::cout << "  test_drain_with_inflight_tasks passed\n";
}

void test_broker_service_integration() {
    using namespace nvlink::broker;
    using namespace nvlink::wasm;

    BrokerServiceConfig config;
    config.force_cpu_fallback = true;
    BrokerService service(config);

    // 1. Prepare module via BrokerRequest (kind = wasm.lifecycle.prepare.v1)
    WasmLifecycleRequest prep_req;
    prep_req.action = WasmLifecycleAction::Prepare;
    prep_req.request_id = "brk-prep-1";
    prep_req.sandbox_id = "sb-svc";
    prep_req.module_id = "mod-svc";
    prep_req.module_version = "1.0.0";

    std::vector<uint8_t> encoded_prep;
    std::string err;
    const bool prep_enc_ok = encode_wasm_lifecycle_request(prep_req, &encoded_prep, &err);
    require(prep_enc_ok, "encode prep request failed");

    BrokerRequest brk_prep;
    brk_prep.task_id = "task-prep-001";
    brk_prep.source = "sb-svc";
    brk_prep.destination = "overhauled";
    brk_prep.kind = kWasmLifecyclePrepareKind;
    brk_prep.priority = 1;
    brk_prep.timeout_ms = 3000;
    brk_prep.payload = encoded_prep;

    const BrokerResponse brk_prep_resp = service.handle_request(brk_prep);
    require(brk_prep_resp.status == TaskStatus::Ok, "broker prepare status should be Ok");
    require(brk_prep_resp.task_id == brk_prep.task_id, "task_id correlation mismatch");

    WasmLifecycleResponse wasm_prep_resp;
    const bool prep_dec_ok = decode_wasm_lifecycle_response(brk_prep_resp.result, &wasm_prep_resp, &err);
    require(prep_dec_ok, "decode prep response failed");
    require(wasm_prep_resp.status == WasmLifecycleStatus::Ok, "wasm prep status should be Ok");
    require(wasm_prep_resp.state == WasmModuleState::Prepared, "wasm prep state should be Prepared");

    // 2. Submit task via BrokerRequest (kind = wasm.task.v1)
    WasmTaskRequest task_req;
    task_req.task_id = "wasm-task-001";
    task_req.sandbox_id = "sb-svc";
    task_req.module_id = "mod-svc";
    task_req.module_version = "1.0.0";
    task_req.task_kind = "inference";
    task_req.priority = 5;
    task_req.payload = pack_doubles({1.5, 3.5});

    std::vector<uint8_t> encoded_task;
    const bool task_enc_ok = encode_wasm_task_request(task_req, &encoded_task, &err);
    require(task_enc_ok, "encode task request failed");

    BrokerRequest brk_task;
    brk_task.task_id = "task-comp-001";
    brk_task.source = "sb-svc";
    brk_task.destination = "overhauled";
    brk_task.kind = kWasmTaskKind;
    brk_task.priority = 5;
    brk_task.timeout_ms = 3000;
    brk_task.payload = encoded_task;

    const BrokerResponse brk_task_resp = service.handle_request(brk_task);
    require(brk_task_resp.status == TaskStatus::Ok, "broker task status should be Ok");
    require(brk_task_resp.selected_gpu == -1, "broker selected_gpu should be -1 in force_cpu");

    WasmTaskResponse wasm_task_resp;
    const bool task_dec_ok = decode_wasm_task_response(brk_task_resp.result, &wasm_task_resp, &err);
    require(task_dec_ok, "decode wasm task response failed");
    require(wasm_task_resp.status == WasmTaskStatus::Ok, "wasm task status should be Ok");
    const std::vector<double> out_doubles = unpack_doubles(wasm_task_resp.result);
    require(out_doubles.size() == 2 && out_doubles[0] == 3.0 && out_doubles[1] == 7.0,
            "wasm task compute result mismatch");

    // 3. Drain via BrokerRequest (kind = wasm.lifecycle.drain.v1)
    WasmLifecycleRequest drain_req;
    drain_req.action = WasmLifecycleAction::Drain;
    drain_req.request_id = "brk-drain-1";
    drain_req.sandbox_id = "sb-svc";
    drain_req.module_id = "mod-svc";
    drain_req.module_version = "1.0.0";

    std::vector<uint8_t> encoded_drain;
    encode_wasm_lifecycle_request(drain_req, &encoded_drain, &err);

    BrokerRequest brk_drain;
    brk_drain.task_id = "task-drain-001";
    brk_drain.source = "sb-svc";
    brk_drain.destination = "overhauled";
    brk_drain.kind = kWasmLifecycleDrainKind;
    brk_drain.priority = 1;
    brk_drain.timeout_ms = 3000;
    brk_drain.payload = encoded_drain;

    const BrokerResponse brk_drain_resp = service.handle_request(brk_drain);
    require(brk_drain_resp.status == TaskStatus::Ok, "broker drain status should be Ok");

    // 4. Submit task to drained module - must be rejected by broker
    const BrokerResponse brk_rej_resp = service.handle_request(brk_task);
    require(brk_rej_resp.status == TaskStatus::Rejected, "task on drained module should be Rejected");

    std::cout << "  test_broker_service_integration passed\n";
}

void test_broker_server_e2e_tcp() {
    using namespace nvlink::broker;
    using namespace nvlink::wasm;

    BrokerServerConfig server_config;
    server_config.port = 19183;
    server_config.service_config.force_cpu_fallback = true;

    BrokerServer server(server_config);
    std::thread server_thread([&server]() { server.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    require(sock >= 0, "socket creation failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19183);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    require(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
            "failed to connect to broker server TCP port");

    auto send_and_recv = [sock](const BrokerRequest& req) -> BrokerResponse {
        ProtocolLimits limits;
        const std::vector<uint8_t> frame = encode_request_frame(req, limits);
        const uint32_t len_be = htonl(static_cast<uint32_t>(frame.size()));

        std::vector<uint8_t> wire;
        wire.resize(4 + frame.size());
        std::memcpy(wire.data(), &len_be, 4);
        std::memcpy(wire.data() + 4, frame.data(), frame.size());

        std::size_t sent = 0;
        while (sent < wire.size()) {
            const ssize_t n = ::send(sock, wire.data() + sent, wire.size() - sent, 0);
            require(n > 0, "send error");
            sent += static_cast<std::size_t>(n);
        }

        uint32_t resp_len_be = 0;
        std::size_t read_len = 0;
        while (read_len < 4) {
            const ssize_t n = ::recv(sock, reinterpret_cast<char*>(&resp_len_be) + read_len, 4 - read_len, 0);
            require(n > 0, "recv error length");
            read_len += static_cast<std::size_t>(n);
        }
        const uint32_t resp_len = ntohl(resp_len_be);

        std::vector<uint8_t> resp_frame(resp_len);
        std::size_t read_frame = 0;
        while (read_frame < resp_len) {
            const ssize_t n = ::recv(sock, resp_frame.data() + read_frame, resp_len - read_frame, 0);
            require(n > 0, "recv error frame");
            read_frame += static_cast<std::size_t>(n);
        }

        BrokerResponse resp;
        std::string err;
        require(decode_response_frame(resp_frame, &resp, &err, limits), "decode response frame failed");
        return resp;
    };

    // 1. Prepare module over TCP
    WasmLifecycleRequest prep;
    prep.action = WasmLifecycleAction::Prepare;
    prep.request_id = "tcp-prep-1";
    prep.sandbox_id = "sb-tcp";
    prep.module_id = "mod-tcp";
    prep.module_version = "1.0.0";

    std::vector<uint8_t> enc_prep;
    std::string err;
    encode_wasm_lifecycle_request(prep, &enc_prep, &err);

    BrokerRequest req1;
    req1.task_id = "tcp-task-1";
    req1.source = "sb-tcp";
    req1.destination = "overhauled";
    req1.kind = kWasmLifecyclePrepareKind;
    req1.priority = 1;
    req1.timeout_ms = 3000;
    req1.payload = enc_prep;

    const BrokerResponse resp1 = send_and_recv(req1);
    require(resp1.status == TaskStatus::Ok, "tcp prepare response should be Ok");

    // 2. Submit task over TCP
    WasmTaskRequest task;
    task.task_id = "tcp-wasm-task-1";
    task.sandbox_id = "sb-tcp";
    task.module_id = "mod-tcp";
    task.module_version = "1.0.0";
    task.task_kind = "inference";
    task.payload = pack_doubles({5.0, 10.0});

    std::vector<uint8_t> enc_task;
    encode_wasm_task_request(task, &enc_task, &err);

    BrokerRequest req2;
    req2.task_id = "tcp-task-2";
    req2.source = "sb-tcp";
    req2.destination = "overhauled";
    req2.kind = kWasmTaskKind;
    req2.priority = 2;
    req2.timeout_ms = 3000;
    req2.payload = enc_task;

    const BrokerResponse resp2 = send_and_recv(req2);
    require(resp2.status == TaskStatus::Ok, "tcp task response should be Ok");

    WasmTaskResponse wasm_resp2;
    require(decode_wasm_task_response(resp2.result, &wasm_resp2, &err), "decode tcp wasm task response failed");
    require(wasm_resp2.status == WasmTaskStatus::Ok, "wasm status should be Ok");
    const std::vector<double> out_doubles = unpack_doubles(wasm_resp2.result);
    require(out_doubles.size() == 2 && out_doubles[0] == 10.0 && out_doubles[1] == 20.0,
            "tcp task compute doubles mismatch");

    ::close(sock);
    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    std::cout << "  test_broker_server_e2e_tcp passed\n";
}

} // namespace

int main() {
    std::cout << "Running wasm_sandbox_test...\n";
    test_task_wire_format();
    test_lifecycle_wire_format();
    test_hotswap_lifecycle_flow();
    test_drain_with_inflight_tasks();
    test_broker_service_integration();
    test_broker_server_e2e_tcp();
    std::cout << "All wasm_sandbox_test passed successfully!\n";
    return 0;
}
