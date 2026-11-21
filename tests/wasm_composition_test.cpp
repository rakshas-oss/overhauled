// SPDX-License-Identifier: MIT
#include "wasm_composition.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main() {
    try {
        nvlink::wasm::WasmCompositionRuntime runtime;
        std::string error;

        // provider.run: () -> i32, returning 42.
        const std::vector<std::uint8_t> provider = {
            0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
            0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
            0x03,0x02,0x01,0x00,
            0x07,0x07,0x01,0x03,0x72,0x75,0x6e,0x00,0x00,
            0x0a,0x06,0x01,0x04,0x00,0x41,0x2a,0x0b
        };
        // consumer.run imports provider.run and returns its result.
        const std::vector<std::uint8_t> consumer = {
            0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
            0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
            0x02,0x10,0x01,0x08,0x70,0x72,0x6f,0x76,0x69,0x64,0x65,0x72,
            0x03,0x72,0x75,0x6e,0x00,0x00,
            0x03,0x02,0x01,0x00,
            0x07,0x07,0x01,0x03,0x72,0x75,0x6e,0x00,0x01,
            0x0a,0x06,0x01,0x04,0x00,0x10,0x00,0x0b
        };

        if (!runtime.compile_and_register("provider", provider, &error)) {
            std::cerr << "provider compile/register failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        if (!runtime.compile_and_register("consumer", consumer, &error)) {
            std::cerr << "consumer compile/register failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        if (!runtime.contains("provider") || !runtime.contains("consumer")) {
            std::cerr << "registered modules not found\n";
            return EXIT_FAILURE;
        }
        if (!runtime.instantiate("consumer", &error)) {
            std::cerr << "consumer instantiation failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        std::int32_t result = 0;
        if (!runtime.invoke_i32("consumer", "run", &result, &error) || result != 42) {
            std::cerr << "cross-module invocation failed: " << error
                      << ", result=" << result << '\n';
            return EXIT_FAILURE;
        }
        if (runtime.instantiate("consumer", &error)) {
            std::cerr << "duplicate instantiation unexpectedly accepted\n";
            return EXIT_FAILURE;
        }
        if (runtime.compile_and_register("provider", provider, &error)) {
            std::cerr << "duplicate namespace unexpectedly accepted\n";
            return EXIT_FAILURE;
        }
        if (runtime.invoke_i32("missing", "run", &result, &error)) {
            std::cerr << "invocation on unknown module unexpectedly succeeded\n";
            return EXIT_FAILURE;
        }

        // Change the consumer's shared function type from () -> i32 to
        // () -> i64. The linker validates registered imports immediately,
        // so the mismatch must be rejected by compile_and_register.
        auto wrong_signature = consumer;
        for (auto& byte : wrong_signature) {
            if (byte == 0x7f) {
                byte = 0x7e;
                break;
            }
        }
        nvlink::wasm::WasmCompositionRuntime mismatch_runtime;
        if (!mismatch_runtime.compile_and_register("provider", provider, &error)) {
            std::cerr << "mismatch provider registration failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        if (mismatch_runtime.compile_and_register("consumer", wrong_signature, &error)) {
            std::cerr << "signature-mismatched import unexpectedly registered\n";
            return EXIT_FAILURE;
        }

        nvlink::wasm::WasmCompositionRuntime::Limits tiny_limits;
        tiny_limits.max_module_bytes = 4;
        nvlink::wasm::WasmCompositionRuntime limited_runtime(tiny_limits);
        if (limited_runtime.compile_and_register("provider", provider, &error)) {
            std::cerr << "oversized module unexpectedly accepted\n";
            return EXIT_FAILURE;
        }

        std::cout << "Wasm cross-module composition test passed (result=42)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
