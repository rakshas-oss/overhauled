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
        // Minimal valid core Wasm module: empty function type, one function,
        // exported as "run", with an empty body.
        const std::vector<std::uint8_t> provider = {
            0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
            0x01,0x04,0x01,0x60,0x00,0x00,
            0x03,0x02,0x01,0x00,
            0x07,0x07,0x01,0x03,0x72,0x75,0x6e,0x00,0x00,
            0x0a,0x04,0x01,0x02,0x00,0x0b
        };
        if (!runtime.compile_and_register("provider", provider, &error)) {
            std::cerr << "compile/register failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        if (!runtime.contains("provider") || !runtime.instantiate("provider", &error)) {
            std::cerr << "instantiate failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        if (runtime.compile_and_register("provider", provider, &error)) {
            std::cerr << "duplicate namespace unexpectedly accepted\n";
            return EXIT_FAILURE;
        }
        std::cout << "Wasm composition runtime smoke test passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
