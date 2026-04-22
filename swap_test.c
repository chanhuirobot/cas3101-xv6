#include "types.h"
#include "stat.h"
#include "user.h"

#define PGSIZE 4096
#define N      8000   // larger than free frames (~7700), forces swap
#define STEP   2000

int
main(void)
{
  int before, after_sbrk, i;
  char *buf;
  int ok = 1;

  printf(1, "=== swap test ===\n");

  before = frees();
  printf(1, "initial free: %d\n", before);

  printf(1, "[1] sbrk(%d pages)\n", N);
  buf = sbrk(N * PGSIZE);
  after_sbrk = frees();
  printf(1, "    free: %d -> %d (consumed %d)\n",
         before, after_sbrk, before - after_sbrk);

  printf(1, "[2] write signature to each page\n");
  for(i = 0; i < N; i++){
    buf[i * PGSIZE] = (char)(i & 0xFF);
    if((i + 1) % STEP == 0)
      printf(1, "    after %d pages: free=%d\n", i + 1, frees());
  }

  printf(1, "[3] read back in reverse\n");
  for(i = N - 1; i >= 0; i--){
    if(buf[i * PGSIZE] != (char)(i & 0xFF)){
      printf(1, "    page %d corrupted (got %d, expected %d)\n",
             i, buf[i * PGSIZE], (char)(i & 0xFF));
      ok = 0;
      break;
    }
  }
  if(ok)
    printf(1, "    all %d pages verified\n", N);

  printf(1, "=== %s ===\n", ok ? "PASS" : "FAIL");
  exit();
}
