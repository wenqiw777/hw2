/*
 * Cache Detection for P-cores and E-cores
 * Uses macOS QoS classes to target different core types
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#if defined(__aarch64__)
static inline uint64_t get_time(void) {
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#else
static inline uint64_t get_time(void) {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif

static inline void memory_barrier(void) {
#if defined(__aarch64__)
    __asm__ __volatile__ ("dmb sy" ::: "memory");
#else
    __asm__ __volatile__ ("mfence" ::: "memory");
#endif
}

static void create_pointer_chase(size_t *array, size_t count) {
    for (size_t i = 0; i < count; i++)
        array[i] = (i + 1) % count;

    for (size_t i = count - 1; i > 0; i--) {
        size_t j = (size_t)rand() % (i + 1);
        size_t temp = array[i];
        array[i] = array[j];
        array[j] = temp;
    }
}

int probe_cache_line_size(void) {
    const size_t ARRAY_SIZE = 16 * 1024 * 1024;
    volatile char *array = (volatile char *)malloc(ARRAY_SIZE);
    if (!array) return 64;

    for (size_t i = 0; i < ARRAY_SIZE; i++)
        array[i] = (char)i;

    int strides[] = {8, 16, 32, 64, 128, 256};
    double norm_times[6];

    for (int s = 0; s < 6; s++) {
        int stride = strides[s];
        size_t num_accesses = ARRAY_SIZE / stride;
        volatile char sum = 0;

        memory_barrier();
        uint64_t start = get_time();
        for (size_t i = 0; i < num_accesses; i++)
            sum += array[i * stride];
        uint64_t end = get_time();

        norm_times[s] = (double)(end - start) / num_accesses * stride;
        (void)sum;
    }

    int detected = 64;
    for (int s = 2; s < 5; s++) {
        if (norm_times[s] / norm_times[s-1] > 1.3 &&
            norm_times[s+1] / norm_times[s] < 1.3) {
            detected = strides[s];
            break;
        }
    }

    free((void *)array);
    return detected;
}

void probe_cache_sizes(size_t *l1_size, size_t *l2_size) {
    size_t sizes[] = {
        8*1024, 16*1024, 32*1024, 48*1024, 64*1024, 96*1024, 128*1024,
        192*1024, 256*1024, 384*1024, 512*1024, 768*1024, 1024*1024,
        2*1024*1024, 4*1024*1024, 8*1024*1024, 16*1024*1024
    };
    int num_sizes = 17;
    double times[17];

    srand(12345);

    for (int s = 0; s < num_sizes; s++) {
        size_t size = sizes[s];
        size_t count = size / sizeof(size_t);
        size_t *array = (size_t *)malloc(size);
        if (!array) break;

        create_pointer_chase(array, count);

        size_t idx = 0;
        for (size_t i = 0; i < count * 2; i++)
            idx = array[idx];

        memory_barrier();
        uint64_t start = get_time();
        for (size_t i = 0; i < count * 4; i++)
            idx = array[idx];
        uint64_t end = get_time();

        volatile size_t dummy = idx; (void)dummy;
        times[s] = (double)(end - start) / (count * 4);
        free(array);
    }

    *l1_size = 0;
    *l2_size = 0;

    for (int s = 1; s < num_sizes; s++) {
        double ratio = times[s] / times[s-1];
        if (*l1_size == 0 && sizes[s-1] <= 128*1024 && ratio > 1.3)
            *l1_size = sizes[s-1];
        else if (*l2_size == 0 && sizes[s-1] > 128*1024 && ratio > 1.3)
            *l2_size = sizes[s-1];
    }
}

void run_tests(const char *core_type) {
    printf("\n=== %s ===\n", core_type);

    int line_size = probe_cache_line_size();
    size_t l1_size, l2_size;
    probe_cache_sizes(&l1_size, &l2_size);

    printf("Cache Line Size: %d bytes\n", line_size);
    printf("L1 Data Cache:   %zu KB\n", l1_size / 1024);
    if (l2_size > 0) {
        if (l2_size >= 1024*1024)
            printf("L2 Cache:        %zu MB\n", l2_size / (1024*1024));
        else
            printf("L2 Cache:        %zu KB\n", l2_size / 1024);
    }
}

int main(void) {
#ifdef __APPLE__
    /* Run on P-cores (high priority) */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    run_tests("Performance Cores (P-cores)");

    /* Run on E-cores (low priority) */
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
    run_tests("Efficiency Cores (E-cores)");
#else
    printf("Core affinity requires macOS with Apple Silicon.\n");
    run_tests("Default Core");
#endif

    return 0;
}
