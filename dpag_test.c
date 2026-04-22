#include "types.h"
#include "stat.h"
#include "user.h"

#define PGSIZE 4096
#define N      10

int
main(void)
{
  int before, after_sbrk, after_touch, i;
  char *buf;
  int ok = 1;

  printf(1, "=== demand paging test ===\n");

  before = frees();
  printf(1, "initial free: %d\n", before);

  printf(1, "[1] sbrk(%d pages)\n", N);
  buf = sbrk(N * PGSIZE);
  after_sbrk = frees();
  printf(1, "    free: %d -> %d (consumed %d)\n",
         before, after_sbrk, before - after_sbrk);

  printf(1, "[2] touch each of the %d pages\n", N);
  for(i = 0; i < N; i++)
    buf[i * PGSIZE] = 1;
  after_touch = frees();
  printf(1, "    free: %d -> %d (consumed %d)\n",
         after_sbrk, after_touch, after_sbrk - after_touch);

  if(before - after_sbrk > 1) ok = 0;
  if(before - after_touch < N) ok = 0;

  printf(1, "=== %s ===\n", ok ? "PASS" : "FAIL");
  exit();
}
