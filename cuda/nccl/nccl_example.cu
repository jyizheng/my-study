// nvcc nccl_simple_transfer.cu -o nccl_transfer_example -lnccl

#include <stdio.h>
#include <stdlib.h>
#include <cuda_runtime.h>
#include <nccl.h>

// --- NCCL/CUDA Error Checking Macro ---
// This is crucial for debugging
#define NCCLCHECK(cmd) do {                         \
  ncclResult_t r = cmd;                             \
  if (r!= ncclSuccess) {                            \
    printf("Failed, NCCL error %s:%d '%s'\n",        \
        __FILE__,__LINE__,ncclGetErrorString(r));   \
    exit(EXIT_FAILURE);                             \
  }                                                 \
} while(0)

#define CUDACHECK(cmd) do {                         \
  cudaError_t e = cmd;                              \
  if (e != cudaSuccess) {                           \
    printf("Failed, CUDA error %s:%d '%s'\n",        \
        __FILE__, __LINE__, cudaGetErrorString(e)); \
    exit(EXIT_FAILURE);                             \
  }                                                 \
} while(0)

int main(int argc, char* argv[])
{
    // 1. --- INITIALIZATION ---
    int nGpus;
    CUDACHECK(cudaGetDeviceCount(&nGpus));
    if (nGpus < 2) {
        printf("This example requires at least 2 GPUs.\n");
        return 0;
    }

    // Define the data size (large enough to trigger high-bandwidth protocol)
    // 16 million floats = 16 * 4 bytes = 64 MB
    const int DATA_ELEMENTS = 16 * 1024 * 1024;
    const size_t dataSize = DATA_ELEMENTS * sizeof(float);

    // Create an NCCL communicator for 2 GPUs
    ncclComm_t comms[2];
    int devs[] = { 0, 1 };
    NCCLCHECK(ncclCommInitAll(comms, 2, devs));
    
    // Allocate GPU memory and create CUDA streams for each GPU
    float* sendBuff;
    float* recvBuff;
    cudaStream_t s0, s1;

    CUDACHECK(cudaSetDevice(0));
    CUDACHECK(cudaMalloc(&sendBuff, dataSize));
    CUDACHECK(cudaStreamCreate(&s0));

    CUDACHECK(cudaSetDevice(1));
    CUDACHECK(cudaMalloc(&recvBuff, dataSize));
    CUDACHECK(cudaStreamCreate(&s1));

    // Prepare source data on the host (CPU)
    float* h_sendBuff = (float*)malloc(dataSize);
    for (int i = 0; i < DATA_ELEMENTS; i++) {
        h_sendBuff[i] = (float)i;
    }

    // Copy source data from host to the sending GPU (GPU 0)
    CUDACHECK(cudaSetDevice(0));
    CUDACHECK(cudaMemcpy(sendBuff, h_sendBuff, dataSize, cudaMemcpyHostToDevice));

    // 2. --- NCCL SEND/RECV OPERATION ---
    // Start NCCL group calls for atomic execution
    NCCLCHECK(ncclGroupStart());

    // Send data from GPU 0 (rank 0) to GPU 1 (rank 1)
    // This is the core operation that will use a "Simple-like" protocol internally
    NCCLCHECK(ncclSend(sendBuff, DATA_ELEMENTS, ncclFloat, 1, comms[0], s0));
    
    // Receive data on GPU 1 (rank 1) from GPU 0 (rank 0)
    NCCLCHECK(ncclRecv(recvBuff, DATA_ELEMENTS, ncclFloat, 0, comms[1], s1));

    // End the group call, launching the operations
    NCCLCHECK(ncclGroupEnd());

    // Synchronize streams to make sure the transfers are complete
    CUDACHECK(cudaStreamSynchronize(s0));
    CUDACHECK(cudaStreamSynchronize(s1));
    
    printf("NCCL Send/Recv operation completed.\n");

    // 3. --- VERIFICATION ---
    float* h_recvBuff = (float*)malloc(dataSize);
    CUDACHECK(cudaSetDevice(1));
    CUDACHECK(cudaMemcpy(h_recvBuff, recvBuff, dataSize, cudaMemcpyDeviceToHost));
    
    int errors = 0;
    for (int i = 0; i < DATA_ELEMENTS; i++) {
        if (h_recvBuff[i] != h_sendBuff[i]) {
            errors++;
        }
    }

    if (errors == 0) {
        printf("Validation PASSED.\n");
    } else {
        printf("Validation FAILED with %d errors.\n", errors);
    }

    // 4. --- CLEANUP ---
    CUDACHECK(cudaSetDevice(0));
    cudaFree(sendBuff);
    cudaStreamDestroy(s0);
    ncclCommDestroy(comms[0]);

    CUDACHECK(cudaSetDevice(1));
    cudaFree(recvBuff);
    cudaStreamDestroy(s1);
    ncclCommDestroy(comms[1]);

    free(h_sendBuff);
    free(h_recvBuff);

    return 0;
}
