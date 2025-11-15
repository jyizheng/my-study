#include "spsc_queue.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

int main() {
  constexpr std::size_t N = 1'000'000;
  SpscQueue<std::uint64_t> q(1 << 14);  // capacity rounded to power of two if needed

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < N; ++i) {
      while (!q.try_push(i)) {
        // queue full, backoff
        std::this_thread::yield();
      }
    }
  });

  std::vector<std::uint64_t> consumed;
  consumed.reserve(N);

  std::thread consumer([&] {
    std::uint64_t x;
    for (std::size_t i = 0; i < N; ++i) {
      while (!q.try_pop(x)) {
        // queue empty, backoff
        std::this_thread::yield();
      }
      consumed.push_back(x);
    }
  });

  producer.join();
  consumer.join();

  // Verify FIFO order
  bool ok = true;
  if (consumed.size() != N) ok = false;
  for (std::size_t i = 0; i < consumed.size(); ++i) {
    if (consumed[i] != i) {
      ok = false;
      std::cerr << "Out of order at " << i << ": got " << consumed[i] << ", expected " << i << "\n";
      break;
    }
  }

  std::cout << (ok ? "OK\n" : "FAILED\n");
  return ok ? 0 : 1;
}