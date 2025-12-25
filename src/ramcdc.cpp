#include <inttypes.h>
#include <x86intrin.h>
#include "ramcdc.h"
#include <sys/time.h>

#define SSE_REGISTER_SIZE_BITS 128
#define SSE_REGISTER_SIZE_BYTES 16

#define AVX256_REGISTER_SIZE_BITS 256
#define AVX256_REGISTER_SIZE_BYTES 32

#define AVX512_REGISTER_SIZE_BITS 512
#define AVX512_REGISTER_SIZE_BYTES 64
#define AVX512_REGISTER_SIZE_INT32 16


static int window_size = 0;
static int ramcdc_max_chunk_size = 0;
static __m256i *avx256_array;
static __m512i *avx512_array;

static uint64_t get_return_position_avx512(char *buff, uint64_t start_position, uint64_t end_position, uint8_t max_value){
        
    uint64_t num_vectors = (end_position - start_position) / AVX512_REGISTER_SIZE_BYTES;
    uint64_t curr_scan_start;

    // Structures to store bytes from data stream and comparison results in 128-bit SSE format
    __m512i xmm_array;
    uint64_t cmp_mask;

    // Load max_value into xmm-format
    __m512i max_val_xmm = _mm512_set1_epi8((char)max_value);
    
    for(uint64_t i = 0; i < num_vectors; i++){
        curr_scan_start = start_position + (i * AVX512_REGISTER_SIZE_BYTES);
        // Load data into xmm register
        xmm_array = _mm512_loadu_si512((__m512i const *)(buff + curr_scan_start));
        
        /* 
        Compare values with max_value. If a byte in xmm_array is geq max_val_xmm,  
        the corresponding bit of cmp_mask is set to 1.
        */

        // Return a mask with the most significant bit of GEQ comparison byte-wise
        cmp_mask = _mm512_cmpge_epu8_mask(xmm_array, max_val_xmm);

        // Return index of first non-zero bit in mask
        // This corresponds to the first non-zero byte in cmp_array 
        if(cmp_mask)
            return curr_scan_start + (__builtin_ffsll(cmp_mask) - 1);
    }

    return end_position;
}

static uint8_t find_maximum_avx512(char *buff, uint64_t start_pos, uint64_t end_pos, __m512i *xmm_array){

    // Assume window_size is a multiple of AVX512_REGISTER_SIZE_BYTES for now
    // Assume num_vectors is even for now - True for most common window sizes. Can fix later via specific check

    __mmask64 cmp_mask = UINT64_MAX;

    uint64_t num_vectors = (end_pos - start_pos) / AVX512_REGISTER_SIZE_BYTES;

    uint64_t step = 2;
    uint64_t half_step = 1;

    // Load contents into __m512i structures
    // Could be optimized later as only 16 xmm registers are avaiable per CPU in 64-bit
    for(uint64_t i = start_pos; i < num_vectors; i++)
        xmm_array[i] = _mm512_loadu_si512((__m512i const *)(buff + start_pos + (AVX512_REGISTER_SIZE_BYTES * i)));
    
    // Repeat vmaxu until a single register is remaining with maximum values
    // Each iteration calculates maximums between a pair of registers and moves it into the first register in the pair
    // Finally, only one will be left with the maximum values from all pairs
    while(step <= num_vectors){
        
        for(uint64_t i = 0; i < num_vectors; i+=step)
            xmm_array[i] = _mm512_maskz_max_epu8(cmp_mask, xmm_array[i], xmm_array[i+half_step]);
        
        // Multiply step by 2
        half_step = step;
        step = step << 1;
    
    }

    // Move the final set of values from the xmm into local memory    
    uint8_t result_store[AVX512_REGISTER_SIZE_BYTES] = {0};

    _mm512_storeu_si512((__m512i *)&result_store, xmm_array[0]);

    // Sequentially scan the last remaining bytes (512 in this case) to find the max value
    uint8_t max_val = 0;
    for(uint64_t i = 0; i < AVX512_REGISTER_SIZE_BYTES; i++){
        if(result_store[i] > max_val)
            max_val = result_store[i];
    }

    // Return maximum value
    return max_val;
}

void ramcdc_init(int average_chunk_size){
    // 如果要实现8KB平均长度，论文VectorCDC指定了avg为8448，这样windows size就能是8192；
    window_size = average_chunk_size - 256;
    ramcdc_max_chunk_size = average_chunk_size * 4;

    uint64_t num_vectors = window_size / AVX512_REGISTER_SIZE_BYTES;
    avx512_array = new __m512i[num_vectors]();
}

int ramcdc_avx_512(unsigned char *p, int n){
    uint32_t i = 0;
    uint8_t max_value = (uint8_t)p[i];
    i++;
    if (n > ramcdc_max_chunk_size)
        n = ramcdc_max_chunk_size;
    else if(n < window_size)
        return n;
 
    max_value = find_maximum_avx512((char*)p, 0, window_size, avx512_array);
    return get_return_position_avx512((char*)p, window_size, n, max_value);
}

int ramcdc_avx_256(const char *p, int n){
    return 0;
}

// __attribute__((noinline))
int ramcdc(const char *p, int n){
    uint32_t i = 0;
    uint8_t max_value = (uint8_t)p[i];
    i++;
    if (n > ramcdc_max_chunk_size)
        n = ramcdc_max_chunk_size;
    else if(n < window_size)
        return n;

    for(i = 0; i < window_size; i++){
        if ((uint8_t)p[i] >= max_value){
            max_value = (uint8_t)p[i];
        }
    }

    for (i = window_size; i < n; i++) {
        if ((uint8_t)p[i] >= max_value)
            return i;
    }

    return n;
}