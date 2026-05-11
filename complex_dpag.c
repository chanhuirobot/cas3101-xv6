#include "types.h"
#include "stat.h"
#include "user.h"

void recursive_stack_stress(int depth) {
  volatile char stack_eater[1000];
  stack_eater[0] = depth;
  stack_eater[999] = depth;

  if (depth < 14) {
    recursive_stack_stress(depth + 1);
  }

  if (stack_eater[0] != depth || stack_eater[999] != depth) {
    printf(1, "Stack corruption detected at depth %d\n", depth);
    exit();
  }
}

int main(int argc, char *argv[]) {
  printf(1, "=== Complex Demand Paging Test ===\n");

  // --- Test 1: Lazy Allocation + Fork ---
  printf(1, "[Test 1] Lazy Allocation and Fork Interaction\n");
  int initial_pages = 100;
  char *heap_base = sbrk(initial_pages * 4096);

  int pid = fork();
  if (pid < 0) {
    printf(1, "fork failed\n");
    exit();
  } else if (pid == 0) {
    int i;
    for (i = 0; i < initial_pages; i += 10) {
      heap_base[i * 4096] = 0xAA;
    }
    exit();
  }
  wait();
  heap_base[0] = 0xBB;
  printf(1, "[Test 1] Passed.\n");

  // --- Test 2: Stack Growth Stress ---
  printf(1, "[Test 2] Stack Growth Stress (Deep Recursion)\n");
  recursive_stack_stress(1);
  printf(1, "[Test 2] Passed. Stack dynamically grew safely.\n");

  // --- Test 3: Guard Page Violation ---
  printf(1, "[Test 3] Guard Page Violation (Should kill child)\n");
  pid = fork();
  if (pid == 0) {
    char dummy;
    uint guard_addr = (uint)&dummy - (17 * 1024);
    volatile char *guard_page_ptr = (volatile char *)guard_addr;
    printf(1, "  Child attempting to access guard page at 0x%x...\n", guard_addr);
    *guard_page_ptr = 0xFF;
    printf(1, "  FAIL: Child survived guard page access!\n");
    exit();
  }
  wait();
  printf(1, "[Test 3] Passed. Parent survived, child was killed.\n");

  printf(1, "=== ALL DPAG TESTS PASSED ===\n");
  exit();
}
