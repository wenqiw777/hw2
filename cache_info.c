/*
 * Cache Information Detection Program
 * Detects cache sizes and cache line sizes using timing-based probing
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

/* High-resolution timing */
#if defined(__x86_64__) || defined(__i386__)
static inline uint64_t get_time(void) {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#elif defined(__aarch64__)
static inline uint64_t get_time(void) {
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#else
static inline uint64_t get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

static inline void memory_barrier(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__ ("mfence" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__ ("dmb sy" ::: "memory");
#else
    __asm__ __volatile__ ("" ::: "memory");
#endif
}

/* Detect cache line size via strided access */
int probe_cache_line_size(void) {
    const size_t ARRAY_SIZE = 32 * 1024 * 1024;
    const int ITERATIONS = 3;

    volatile char *array = (volatile char *)malloc(ARRAY_SIZE);
    if (!array) return 64;

    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        array[i] = (char)i;
    }

    int strides[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    int num_strides = sizeof(strides) / sizeof(strides[0]);
    double norm_times[10];

    for (int s = 0; s < num_strides; s++) {
        int stride = strides[s];
        size_t num_accesses = ARRAY_SIZE / stride;
        volatile char sum = 0;
        uint64_t total_time = 0;

        for (int iter = 0; iter < ITERATIONS; iter++) {
            memory_barrier();
            uint64_t start = get_time();
            memory_barrier();

            for (size_t i = 0; i < num_accesses; i++) {
                sum += array[i * stride];
            }

            memory_barrier();
            uint64_t end = get_time();
            total_time += (end - start);
        }

        double avg_time = (double)total_time / (ITERATIONS * num_accesses);
        norm_times[s] = avg_time * stride;
        (void)sum;
    }

    int detected = 64;
    for (int s = 2; s < num_strides - 1; s++) {
        double growth_before = norm_times[s] / norm_times[s-1];
        double growth_after = norm_times[s+1] / norm_times[s];
        if (growth_before > 1.3 && growth_after < 1.3) {
            detected = strides[s];
            break;
        }
    }

    free((void *)array);
    return detected;
}

/* Create pointer-chase pattern */
static void create_pointer_chase(size_t *array, size_t count) {
    for (size_t i = 0; i < count; i++) {
        array[i] = (i + 1) % count;
    }

    for (size_t i = count - 1; i > 0; i--) {
        size_t j = (size_t)rand() % (i + 1);
        size_t temp = array[i];
        array[i] = array[j];
        array[j] = temp;
    }

    size_t *visited = (size_t *)calloc(count, sizeof(size_t));
    size_t *chase = (size_t *)malloc(count * sizeof(size_t));

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        chase[pos] = array[i];
        visited[pos] = 1;
        pos = array[i];
    }

    for (size_t i = 0; i < count; i++) {
        array[i] = chase[i];
    }

    free(visited);
    free(chase);
}

/* Detect cache sizes via pointer-chase */
void probe_cache_sizes(size_t *l1_size, size_t *l2_size, size_t *l3_size) {
    const int ITERATIONS = 5;

    size_t sizes[] = {
        4*1024, 8*1024, 16*1024, 32*1024, 48*1024, 64*1024, 96*1024, 128*1024,
        192*1024, 256*1024, 384*1024, 512*1024, 768*1024, 1024*1024, 1536*1024,
        2*1024*1024, 3*1024*1024, 4*1024*1024, 6*1024*1024, 8*1024*1024,
        12*1024*1024, 16*1024*1024, 24*1024*1024, 32*1024*1024
    };
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    double times[32];

    srand(12345);

    for (int s = 0; s < num_sizes; s++) {
        size_t size = sizes[s];
        size_t count = size / sizeof(size_t);
        size_t accesses = count * 4;

        size_t *array = (size_t *)malloc(size);
        if (!array) break;

        create_pointer_chase(array, count);

        size_t idx = 0;
        for (size_t i = 0; i < count; i++) {
            idx = array[idx];
        }

        uint64_t total_time = 0;
        for (int iter = 0; iter < ITERATIONS; iter++) {
            idx = 0;
            memory_barrier();
            uint64_t start = get_time();
            memory_barrier();

            for (size_t a = 0; a < accesses; a++) {
                idx = array[idx];
            }

            memory_barrier();
            uint64_t end = get_time();
            total_time += (end - start);
        }

        volatile size_t dummy = idx;
        (void)dummy;

        times[s] = (double)total_time / (ITERATIONS * accesses);
        free(array);
    }

    *l1_size = 0;
    *l2_size = 0;
    *l3_size = 0;

    for (int s = 1; s < num_sizes; s++) {
        double ratio = times[s] / times[s-1];
        if (*l1_size == 0 && sizes[s-1] <= 192*1024 && ratio > 1.3) {
            *l1_size = sizes[s-1];
        } else if (*l2_size == 0 && sizes[s-1] > 192*1024 && sizes[s-1] <= 16*1024*1024 && ratio > 1.3) {
            *l2_size = sizes[s-1];
        } else if (*l3_size == 0 && sizes[s-1] > 4*1024*1024 && ratio > 1.5) {
            *l3_size = sizes[s-1];
        }
    }
}

/* Detect cache associativity */
int probe_associativity(void) {
    const int ITERATIONS = 100;
    const int ACCESSES_PER_ITER = 10000;
    size_t SET_STRIDE = 4096;
    size_t ARRAY_SIZE = SET_STRIDE * 48;

    volatile char *array = (volatile char *)malloc(ARRAY_SIZE);
    if (!array) return 8;

    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        array[i] = (char)i;
    }

    int ways_to_test[] = {2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 32};
    int num_tests = sizeof(ways_to_test) / sizeof(ways_to_test[0]);
    double times[16];

    for (int t = 0; t < num_tests; t++) {
        int num_addrs = ways_to_test[t];
        volatile char sum = 0;

        for (int w = 0; w < num_addrs; w++) {
            sum += array[w * SET_STRIDE];
        }

        uint64_t total_time = 0;
        for (int iter = 0; iter < ITERATIONS; iter++) {
            memory_barrier();
            uint64_t start = get_time();
            memory_barrier();

            for (int a = 0; a < ACCESSES_PER_ITER; a++) {
                for (int w = 0; w < num_addrs; w++) {
                    sum += array[w * SET_STRIDE];
                }
            }

            memory_barrier();
            uint64_t end = get_time();
            total_time += (end - start);
        }

        times[t] = (double)total_time / (ITERATIONS * ACCESSES_PER_ITER * num_addrs);
        (void)sum;
    }

    int detected = 8;
    double max_ratio = 1.0;
    for (int t = 1; t < num_tests; t++) {
        double ratio = times[t] / times[t-1];
        if (ratio > max_ratio && ratio > 1.3) {
            max_ratio = ratio;
            detected = ways_to_test[t-1];
        }
    }

    free((void *)array);
    return detected;
}

int main(void) {
    int line_size = probe_cache_line_size();

    size_t l1_size, l2_size, l3_size;
    probe_cache_sizes(&l1_size, &l2_size, &l3_size);

    int associativity = probe_associativity();

    printf("Cache Line Size: %d bytes\n", line_size);
    printf("L1 Data Cache:   %zu KB\n", l1_size / 1024);
    if (l2_size > 0) {
        if (l2_size >= 1024*1024)
            printf("L2 Cache:        %zu MB\n", l2_size / (1024*1024));
        else
            printf("L2 Cache:        %zu KB\n", l2_size / 1024);
    }
    if (l3_size > 0) {
        printf("L3 Cache:        %zu MB\n", l3_size / (1024*1024));
    }
    printf("L1 Associativity: %d-way\n", associativity);

    return 0;
}
