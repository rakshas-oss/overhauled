#include <nvlink_placement.h>

int main() {
    auto topology = nvlink::GpuTopology::synthetic(2);
    nvlink::Placer placer(topology);
    return placer.assign_home(7) >= 0 ? 0 : 1;
}
