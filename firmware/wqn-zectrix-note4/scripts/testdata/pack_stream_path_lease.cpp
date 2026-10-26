#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "runtime/pack_stream_path_lease.h"

using wqn::runtime::PackStreamPathLease;
static unsigned checks = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); std::abort(); } } while (false)

int main()
{
    static_assert(!std::is_copy_constructible_v<PackStreamPathLease>);
    static_assert(!std::is_move_constructible_v<PackStreamPathLease>);
    { PackStreamPathLease empty(""); PackStreamPathLease null(nullptr); CHECK(!empty && !null); }
    CHECK(!wqn::runtime::HasActivePackStreamPath());
    const std::string oversized(wqn::runtime::kPackStreamPathMaxBytes, 'x');
    { PackStreamPathLease rejected(oversized.c_str()); CHECK(!rejected); }
    const std::string path = "/storage/np_same_0123456789ab.tmp";
    std::string equal_copy = path;
    {
        PackStreamPathLease first(path.c_str()); CHECK(first);
        CHECK(wqn::runtime::HasActivePackStreamPath());
        { PackStreamPathLease rejected(equal_copy.c_str()); CHECK(!rejected); }
        { PackStreamPathLease rejected(path.c_str()); CHECK(!rejected); }
        { PackStreamPathLease different("/storage/np_other_0123456789ab.tmp"); CHECK(different); }
        PackStreamPathLease still_rejected(path.c_str()); CHECK(!still_rejected);
    }
    { PackStreamPathLease again(path.c_str()); CHECK(again); }
    CHECK(!wqn::runtime::HasActivePackStreamPath());
    {
        std::array<std::string, wqn::runtime::kPackStreamPathCapacity + 1> paths;
        for (size_t i = 0; i < paths.size(); ++i) paths[i] = "/storage/np_" + std::to_string(i) + ".tmp";
        std::array<std::optional<PackStreamPathLease>, wqn::runtime::kPackStreamPathCapacity> leases;
        for (size_t i = 0; i < leases.size(); ++i) { leases[i].emplace(paths[i].c_str()); CHECK(*leases[i]); }
        { PackStreamPathLease full(paths.back().c_str()); CHECK(!full); }
        leases[3].reset();
        PackStreamPathLease freed_slot(paths.back().c_str()); CHECK(freed_slot);
        PackStreamPathLease still_full(paths[3].c_str()); CHECK(!still_full);
    }
    CHECK(!wqn::runtime::HasActivePackStreamPath());
    for (unsigned i = 0; i < 1000; ++i) { PackStreamPathLease lease(path.c_str()); CHECK(lease); }

    std::atomic<unsigned> ready{0}, winners{0}; std::atomic<bool> release{false};
    std::vector<std::thread> tasks;
    for (unsigned i = 0; i < 8; ++i) tasks.emplace_back([&] {
        PackStreamPathLease lease(path.c_str());
        if (lease) winners.fetch_add(1, std::memory_order_relaxed);
        ready.fetch_add(1, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ready.load(std::memory_order_acquire) != 8 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(ready.load() == 8 && winners.load() == 1);
    release.store(true, std::memory_order_release);
    for (auto& task : tasks) task.join();
    { PackStreamPathLease lease(path.c_str()); CHECK(lease); }
    CHECK(!wqn::runtime::HasActivePackStreamPath());
    std::printf("Production pack path admission: %u checks PASS; host task-lock port, not ESP32 SMP HIL\n", checks);
}
