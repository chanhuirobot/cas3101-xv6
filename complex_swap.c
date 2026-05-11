#include "types.h"
#include "stat.h"
#include "user.h"

unsigned int seed = 12345;
unsigned int my_rand() {
  seed = seed * 1664525 + 1013904223;
  return seed;
}

int main(int argc, char *argv[]) {
  int i;
  printf(1, "=== Complex Swap Test ===\n");

  int total_pages = 5000;
  printf(1, "[Test 1] Allocating %d pages...\n", total_pages);
  char *base = sbrk(total_pages * 4096);
  if ((int)base == -1) {
    printf(1, "sbrk failed\n");
    exit();
  }

  // --- Test 2: Random Access Write (Thrashing) ---
  printf(1, "[Test 2] Random Access Write (Thrashing stress)\n");
  for (i = 0; i < total_pages; i++) {
    int p_idx = my_rand() % total_pages;
    base[p_idx * 4096] = (char)(p_idx % 256);
  }

  for (i = 0; i < total_pages; i++) {
    base[i * 4096] = (char)(i % 256);
  }

  // --- Test 3: Swap + Fork ---
  printf(1, "[Test 3] Forking while pages are swapped out...\n");
  int pid = fork();

  if (pid < 0) {
    printf(1, "fork failed\n");
    exit();
  } else if (pid == 0) {
    printf(1, "  Child verifying data...\n");
    for (i = 0; i < total_pages; i += 5) {
      if (base[i * 4096] != (char)(i % 256)) {
        printf(1, "  FAIL: Child data corrupted at page %d\n", i);
        exit();
      }
      base[i * 4096] = 0xFF;
    }
    printf(1, "  Child verification & modification done. Exiting.\n");
    exit();
  }

  wait();

  // --- Test 4: Parent Data Integrity ---
  printf(1, "[Test 4] Parent verifying its own isolated data...\n");
  for (i = 0; i < total_pages; i += 5) {
    if (base[i * 4096] != (char)(i % 256)) {
      printf(1, "FAIL: Parent data corrupted at page %d! (Swap copyuvm issue)\n", i);
      exit();
    }
  }
  printf(1, "[Test 4] Passed. Parent and Child swap isolation is perfect.\n");

  printf(1, "=== ALL SWAP TESTS PASSED ===\n");
  exit();
}
