#include "interop_encryption.h"

#include <cassert>
#include <vector>

int main() {
    nvlink::interop::Session a;
    nvlink::interop::Session b;
    std::vector<uint8_t> shared = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::vector<uint8_t> pqc = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    std::string error;

    assert(a.initialize(shared, pqc, &error));
    assert(b.initialize(shared, pqc, &error));

    std::vector<uint8_t> aad = {'a', 'd', 'd'};
    std::vector<uint8_t> plain = {'h', 'i', 'r', 'o', 'n', 's', 's', 'a', 'n', 'd', 'b', 'e', 'r', 'r', 'y'};

    auto ct = a.encrypt(plain, aad, &error);
    assert(!ct.empty());
    auto dec = b.decrypt(ct, aad, &error);
    assert(!dec.empty());
    assert(dec == plain);
    return 0;
}
