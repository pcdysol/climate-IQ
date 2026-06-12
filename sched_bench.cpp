// sched_bench.cpp — measure the per-tick cost of the 1s-poll cheap path.
// Every 1s tick runs gettimeofday() + localtime() + a few int compares BEFORE
// the dedup early-out. This measures that cost so we can scale it to the ESP32.
// Build: g++ -O2 -std=c++17 sched_bench.cpp -o sched_bench

#include <cstdio>
#include <ctime>
#include <sys/time.h>
#include <cstdint>
#include <chrono>
#include <initializer_list>

int main() {
    const long N = 20'000'000;       // 20M iterations
    volatile int sink = 0;           // stop the optimizer deleting the work
    int lastMin = -1, lastWday = -1;

    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < N; ++i) {
        // --- exactly what each 1s tick does before/at the dedup ---
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        time_t t = tv.tv_sec;
        struct tm* tmp = localtime(&t);              // == getLocalTime()
        int currentMin  = tmp->tm_hour * 60 + tmp->tm_min;
        int currentWday = tmp->tm_wday;
        bool boot = (lastMin == -1 || lastWday == -1);
        if (!boot && currentMin == lastMin && currentWday == lastWday) {
            sink += currentMin;                      // dedup early-out branch
            continue;
        }
        lastMin = currentMin; lastWday = currentWday;
        sink += currentWday;
    }
    auto t1 = std::chrono::steady_clock::now();

    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double per = ns / N;
    printf("Host: %ld iters, %.1f ns/tick (gettimeofday+localtime+dedup)\n", N, per);
    printf("Host CPU for 1 tick/sec: %.6f %% of one core\n", (per / 1e9) * 100.0);

    // Conservative ESP32 scaling. ESP32 @240MHz vs a ~GHz desktop core: libc
    // time calls run perhaps 30-60x slower. Show a pessimistic 60x.
    for (int mult : {30, 60, 100}) {
        double esp_us = (per * mult) / 1000.0;       // us per tick on ESP32
        double cpu_pct = (esp_us / 1e6) * 100.0;     // once per second
        printf("ESP32 est (%3dx slower): %.2f us/tick -> %.5f %% CPU at 1 tick/sec\n",
               mult, esp_us, cpu_pct);
    }
    printf("(sink=%d)\n", sink);
    return 0;
}
