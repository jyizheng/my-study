//nvcc fence_example.cu -o fence_example -arch=compute_86 -code=sm_86

#include <stdio.h>

// GPU核函数
__global__ void memory_fence_kernel(int* data, int* flag)
{
    // 获取当前线程在块内的ID
    int tid = threadIdx.x;

    // 线程0作为生产者
    if (tid == 0)
    {
        // 1. 写入数据
        *data = 42;

        // --- 内存栅栏 ---
        // 确保对 *data 的写入（值为42）
        // 在对 *flag 的写入（值为1）之前，对块内所有其他线程可见。
        // 如果没有这个栅栏，其他线程可能会先看到 flag=1，但读到的 data 还是旧值。
        __threadfence_block();

        // 2. 写入标志，表示数据已准备好
        *flag = 1;
    }
    // 其他线程作为消费者
    else
    {
        // 3. 等待生产者设置好标志
        // (注意：在实际代码中，这种循环等待(spin-wait)会消耗大量资源，效率很低)
        while (*flag == 0) {
            // 等待...
        }

        // --- 内存栅栏 ---
        // 确保读取 *data 的操作，在读取 *flag (值为1)之后发生。
        // 这可以防止编译器或硬件重排指令，导致先读取data再读取flag。
        __threadfence_block();
        
        // 4. 读取数据
        int consumed_data = *data;
        
        // 由于有栅栏的存在，这里可以保证 consumed_data 的值一定是 42
        if (consumed_data != 42) {
            printf("Thread %d read wrong data: %d\n", tid, consumed_data);
        }
    }
}

// 主机代码
int main()
{
    int* d_data, *d_flag;
    int h_flag = 0;

    // 在GPU上分配内存
    cudaMalloc(&d_data, sizeof(int));
    cudaMalloc(&d_flag, sizeof(int));

    // 初始化flag为0
    cudaMemcpy(d_flag, &h_flag, sizeof(int), cudaMemcpyHostToDevice);

    // 启动一个包含128个线程的线程块
    memory_fence_kernel<<<1, 128>>>(d_data, d_flag);

    // 等待GPU完成
    cudaDeviceSynchronize();
    printf("Kernel finished.\n");

    // 释放内存
    cudaFree(d_data);
    cudaFree(d_flag);

    return 0;
}

