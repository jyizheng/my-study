#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Single-Producer Single-Consumer (SPSC) bounded FIFO queue using atomics only.
// - One thread must exclusively push (producer).
// - One thread must exclusively pop (consumer).
// - Lock-free, wait-free for both producer and consumer (bounded).
//
// Memory ordering:
// - Producer publishes an element with a release-store to tail_ after constructing it.
//   Consumer acquires that with an acquire-load of tail_ before reading the element.
// - Consumer releases a slot with a release-store to head_ after destroying the element.
//   Producer acquires that with an acquire-load of head_ before reusing the slot.
//
// This avoids data races on elements while maintaining FIFO order.

namespace spsc_detail {
inline bool is_pow2(std::size_t x) { return x && ((x & (x - 1)) == 0); }
inline std::size_t next_pow2(std::size_t x) {
  if (x <= 1) return 1;
  --x;
  for (std::size_t i = 1; i < sizeof(std::size_t) * 8; i <<= 1) x |= x >> i;
  return x + 1;
}
}  // namespace spsc_detail

template <class T>
class SpscQueue {
public:
  explicit SpscQueue(std::size_t capacity)
      : capacity_(spsc_detail::is_pow2(capacity) ? capacity
                                                 : spsc_detail::next_pow2(capacity)),
        mask_(capacity_ - 1),
        buffer_(static_cast<storage_t*>(::operator new[](capacity_ * sizeof(storage_t),
                                                        std::align_val_t(alignof(T))))) {
    // head_ and tail_ start at 0
  }

  ~SpscQueue() {
    // The queue must not be used by other threads during destruction.
    // Destroy any remaining constructed elements.
    std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    while (h != t) {
      pointer_at(h)->~T();
      h = inc(h);
    }
    ::operator delete[](buffer_, std::align_val_t(alignof(T)));
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Capacity is fixed and rounded up to a power of two.
  std::size_t capacity() const noexcept { return capacity_; }

  // Returns an approximation of the current size (safe under SPSC usage).
  std::size_t size_approx() const noexcept {
    const std::size_t h = head_.load(std::memory_order_acquire);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    return (t + capacity_ - h) & mask_;
  }

  bool empty() const noexcept {
    const std::size_t h = head_.load(std::memory_order_acquire);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    return h == t;
  }

  bool full() const noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t next_t = inc(t);
    const std::size_t h = head_.load(std::memory_order_acquire);  // acquire pairs with consumer's release on head_
    return next_t == h;
  }

  // Producer API
  bool try_push(const T& v) {
    return emplace_impl([&](void* p) { ::new (p) T(v); });
  }

  bool try_push(T&& v) {
    return emplace_impl([&](void* p) { ::new (p) T(std::move(v)); });
  }

  template <class... Args>
  bool try_emplace(Args&&... args) {
    return emplace_impl([&](void* p) { ::new (p) T(std::forward<Args>(args)...); });
  }

  // Consumer API
  bool try_pop(T& out) {
    std::size_t h = head_.load(std::memory_order_relaxed);  // only consumer writes head_
    const std::size_t t = tail_.load(std::memory_order_acquire);  // acquire pairs with producer's release to see data
    if (h == t) return false;  // empty

    T* elem = pointer_at(h);
    out = std::move(*elem);
    elem->~T();

    head_.store(inc(h), std::memory_order_release);  // release: publish slot free to producer
    return true;
  }

  // Convenience: returns false if empty; otherwise moves the front out.
  // Note: not exception-safe if T's move constructor throws (like try_pop).
  bool pop_discard() {
    std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    if (h == t) return false;
    T* elem = pointer_at(h);
    elem->~T();
    head_.store(inc(h), std::memory_order_release);
    return true;
  }

private:
  using storage_t = std::aligned_storage_t<sizeof(T), alignof(T)>;

  template <class Ctor>
  bool emplace_impl(Ctor&& ctor) {
    // Producer thread only
    std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t next_t = inc(t);

    // Acquire head_ to ensure we don't overwrite a slot the consumer hasn't released yet.
    const std::size_t h = head_.load(std::memory_order_acquire);
    if (next_t == h) return false;  // full

    void* slot = static_cast<void*>(pointer_at(t));
    ctor(slot);  // If this throws, we haven't published tail_ yet; safe.

    // Publish the new element.
    tail_.store(next_t, std::memory_order_release);
    return true;
  }

  T* pointer_at(std::size_t idx) noexcept {
    return std::launder(reinterpret_cast<T*>(&buffer_[idx]));
  }
  const T* pointer_at(std::size_t idx) const noexcept {
    return std::launder(reinterpret_cast<const T*>(&buffer_[idx]));
  }

  std::size_t inc(std::size_t x) const noexcept { return (x + 1) & mask_; }

  const std::size_t capacity_;
  const std::size_t mask_;
  storage_t* const buffer_;

  alignas(64) std::atomic<std::size_t> head_{0};  // updated by consumer, read by producer
  char pad1_[64 - sizeof(head_)]{};
  alignas(64) std::atomic<std::size_t> tail_{0};  // updated by producer, read by consumer
  char pad2_[64 - sizeof(tail_)]{};
};