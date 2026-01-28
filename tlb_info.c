/*
 * TLB and Page Size Detection Program
 * Detects page size and TLB size using timing-based probing
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

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

/*
 * Detect page size via stride access
 * When stride >= page_size, each access touches a new page
 * causing potential TLB activity changes
 */
size_t probe_page_size(void) {
    const size_t ARRAY_SIZE = 128 * 1024 * 1024;  /* 128MB */
    const int ITERATIONS = 3;

    volatile char *array = (volatile char *)malloc(ARRAY_SIZE);
    if (!array) return 4096;

    /* Touch all pages to ensure they're mapped */
    for (size_t i = 0; i < ARRAY_SIZE; i += 4096) {
        array[i] = (char)i;
    }

    size_t strides[] = {512, 1024, 2048, 4096, 8192, 16384};
    int num_strides = sizeof(strides) / sizeof(strides[0]);
    double times[8];

    for (int s = 0; s < num_strides; s++) {
        size_t stride = strides[s];
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

        times[s] = (double)total_time / (ITERATIONS * num_accesses);
        (void)sum;
    }

    /* Find where normalized time levels off (similar to cache line detection) */
    size_t detected = 4096;
    for (int s = 2; s < num_strides; s++) {
        double ratio = times[s] / times[s-1];
        if (ratio < 0.6) {  /* Time per access drops when stride >= page */
            detected = strides[s-1];
            break;
        }
    }

    free((void *)array);
    return detected;
}

/*
 * Detect TLB size via page-stride pointer chase
 * Access N pages in random order; when N > TLB entries, TLB misses occur
 */
int probe_tlb_size(size_t page_size) {
    const int ITERATIONS = 5;
    const size_t MAX_PAGES = 4096;
    const size_t ARRAY_SIZE = MAX_PAGES * page_size;

    char *array = (char *)malloc(ARRAY_SIZE);
    if (!array) return 64;

    /* Initialize all pages */
    for (size_t i = 0; i < ARRAY_SIZE; i += page_size) {
        array[i] = 0;
    }

    int pages_to_test[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    int num_tests = sizeof(pages_to_test) / sizeof(pages_to_test[0]);
    double times[12];

    srand(54321);

    for (int t = 0; t < num_tests; t++) {
        int num_pages = pages_to_test[t];
        if ((size_t)num_pages > MAX_PAGES) break;

        /* Create random order of pages */
        size_t *order = (size_t *)malloc(num_pages * sizeof(size_t));
        for (int i = 0; i < num_pages; i++) order[i] = i;

        /* Fisher-Yates shuffle */
        for (int i = num_pages - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            size_t tmp = order[i]; order[i] = order[j]; order[j] = tmp;
        }

        /* Create pointer chase: page[i] -> page[i+1] in shuffled order */
        for (int i = 0; i < num_pages - 1; i++) {
            *(size_t *)(array + order[i] * page_size) = order[i + 1] * page_size;
        }
        *(size_t *)(array + order[num_pages - 1] * page_size) = order[0] * page_size;

        /* Warm up */
        size_t idx = order[0] * page_size;
        for (int i = 0; i < num_pages * 4; i++) {
            idx = *(size_t *)(array + idx);
        }

        /* Timed pointer chase */
        uint64_t total_time = 0;
        size_t accesses = (size_t)num_pages * 200;

        for (int iter = 0; iter < ITERATIONS; iter++) {
            idx = order[0] * page_size;
            memory_barrier();
            uint64_t start = get_time();
            memory_barrier();

            for (size_t a = 0; a < accesses; a++) {
                idx = *(size_t *)(array + idx);
            }

            memory_barrier();
            uint64_t end = get_time();
            total_time += (end - start);
        }

        volatile size_t dummy = idx; (void)dummy;
        times[t] = (double)total_time / (ITERATIONS * accesses);
        free(order);
    }

    /* Find TLB size: where latency increases significantly */
    int detected = 64;
    for (int t = 1; t < num_tests; t++) {
        double ratio = times[t] / times[t-1];
        if (ratio > 1.25) {
            detected = pages_to_test[t-1];
            break;
        }
    }

    free(array);
    return detected;
}

int main(void) {
    size_t page_size = probe_page_size();

    /* Run multiple trials and take most common result */
    int results[10];
    for (int i = 0; i < 10; i++) {
        results[i] = probe_tlb_size(page_size);
    }

    /* Find most frequent result */
    int tlb_size = results[0];
    int max_count = 1;
    for (int i = 0; i < 10; i++) {
        int count = 0;
        for (int j = 0; j < 10; j++) {
            if (results[j] == results[i]) count++;
        }
        if (count > max_count) {
            max_count = count;
            tlb_size = results[i];
        }
    }

    printf("Page Size: %zu bytes (%zu KB)\n", page_size, page_size / 1024);
    printf("TLB Size:  %d entries\n", tlb_size);

    return 0;
}
