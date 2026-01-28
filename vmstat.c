#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("=== Processor Cache Info (CPU 0) ===\n");

    // 循环遍历 index 0 到 3 (通常对应 L1d, L1i, L2, L3)
    // 只要文件存在，我们就读取它
    for (int i = 0; i < 4; i++) {
        char path[128];
        char data[64];
        FILE *f;

        // --- 步骤 1: 检查这一层 Cache 是否存在 ---
        sprintf(path, "/sys/devices/system/cpu/cpu0/cache/index%d/size", i);
        f = fopen(path, "r");
        if (f == NULL) break; // 如果打不开，说明没有更多 Cache 了，退出循环
        fscanf(f, "%s", data);
        fclose(f);
        
        printf("\n[Cache Level Index %d]:\n", i);
        printf("  Size:          %s\n", data);

        // --- 步骤 2: 读取类型 (Type: Data vs Instruction) ---
        sprintf(path, "/sys/devices/system/cpu/cpu0/cache/index%d/type", i);
        f = fopen(path, "r");
        if (f) {
            fscanf(f, "%s", data);
            fclose(f);
            printf("  Type:          %s\n", data);
        }

        // --- 步骤 3: 读取关联度 (Associativity) ---
        sprintf(path, "/sys/devices/system/cpu/cpu0/cache/index%d/ways_of_associativity", i);
        f = fopen(path, "r");
        if (f) {
            fscanf(f, "%s", data); // 直接当字符串读出来打印，不需要转成 int
            fclose(f);
            printf("  Associativity: %s-way\n", data);
        }

        // --- 步骤 4: 读取 Cache Line 大小 ---
        sprintf(path, "/sys/devices/system/cpu/cpu0/cache/index%d/coherency_line_size", i);
        f = fopen(path, "r");
        if (f) {
            fscanf(f, "%s", data);
            fclose(f);
            printf("  Line Size:     %s bytes\n", data);
        }
    }

    return 0;
}