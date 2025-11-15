# SPSC 队列实现要点（spsc_queue.hpp）

## 使用场景与模型
- 单生产者/单消费者（SPSC）有界环形队列，固定容量，O(1) 无锁操作
- 只能有一个线程调用 `push/emplace`，且只能有一个线程调用 `pop`

## 数据结构与索引
- 环形缓冲区（ring buffer），容量向上取 2 的幂，使用 `mask` 做取模：`idx = (idx + 1) & mask`
- 判空：`head == tail`；判满：`next(tail) == head`
- `size_approx` 通过 `(t - h) & mask` 近似计算大小

## 原子变量与内存序
- `head_`：消费者写、生产者读；`tail_`：生产者写、消费者读
- 发布-订阅配对
  - 生产者发布元素：`tail_.store(next, release)`
  - 消费者读取发布：`tail_.load(acquire)`
  - 消费者释放槽位：`head_.store(next, release)`
  - 生产者读取释放：`head_.load(acquire)`
- 自己写自己的指针（生产者读写 `tail_`、消费者读写 `head_`）时可用 `relaxed`；跨线程可见性用 `acquire/release`

## 缓存行对齐与伪共享
- `head_` 与 `tail_` 分别 `alignas(64)` 并用 padding 分隔，减少伪共享

## 内存管理与对象生命周期
- 使用 `std::aligned_storage_t` + 定位 new 构造对象，显式析构以回收
- `std::launder` 获取跨生命周期有效指针，避免未定义行为
- 析构函数中遍历剩余元素并析构，最后对齐的 `::operator delete[]`
- `emplace` 先构造后发布 `tail`（release），构造抛异常时未发布，不会被消费者看到，异常安全

## API 设计
- `try_push(const T&)` / `try_push(T&&)` / `try_emplace(...)`：满时返回 `false`，不阻塞
- `try_pop(T& out)`：空时返回 `false`，不阻塞
- `pop_discard()`：丢弃队头元素（无需移动/拷贝）

## 并发语义
- SPSC 条件下为无锁且有界等待（等待来自上层自旋/`yield`/backoff）
- FIFO 顺序由 `head/tail` 的单调推进与发布-订阅保障

## 工程细节
- 容量非 2 的幂时向上取整到 2 的幂（`next_pow2`）
- 示例使用自旋 + `std::this_thread::yield` 作为退避策略
- 需要 C++17 支持：`std::launder`、对齐的 `::operator new[]/delete[]`（`std::align_val_t`）
- `size_approx`/`empty`/`full` 在并发下为近似/瞬时判断，不能当作线性化的强语义

## 适用与限制
- 仅 SPSC；多生产者或多消费者需要不同算法（如 MPMC 队列）
- 有界队列；需要无界可用分段/链表化环或多块式扩容
- 非阻塞接口；若需阻塞可在外层加条件变量或 futex 等配合状态位使用

