// 交叉工具链冒烟测试：覆盖 rerinku-client 依赖的 C++17 特性（线程、原子、optional/variant、
// chrono、异常），编出来放到板子上跑，全部通过打印 "smoke ok"。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <sys/utsname.h>

int main() {
    utsname u{};
    uname(&u);
    std::printf("machine=%s release=%s\n", u.machine, u.release);

    std::atomic<std::uint64_t> counter{0};
    std::mutex mu;
    std::vector<int> order;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&, i] {
            for (int n = 0; n < 10000; ++n) counter.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mu);
            order.push_back(i);
        });
    }
    for (auto &t : workers) t.join();
    if (counter.load() != 40000 || order.size() != 4) {
        std::printf("threads/atomics FAILED: %llu\n", static_cast<unsigned long long>(counter.load()));
        return 1;
    }

    std::optional<std::string> opt = std::string("optional");
    std::variant<int, std::string> var = std::string("variant");
    std::string_view sv = *opt;
    auto shared = std::make_shared<std::string>(std::get<std::string>(var));

    bool caught = false;
    try {
        throw std::runtime_error("exception");
    } catch (const std::exception &e) {
        caught = std::string_view(e.what()) == "exception";
    }

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    if (!caught || sv != "optional" || *shared != "variant" || ms < 20) {
        std::printf("FAILED caught=%d ms=%lld\n", caught, static_cast<long long>(ms));
        return 1;
    }
    std::printf("smoke ok (threads, atomics, optional, variant, exceptions, chrono %lldms)\n",
                static_cast<long long>(ms));
    return 0;
}
