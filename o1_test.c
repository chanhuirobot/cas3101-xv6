#include "types.h"
#include "stat.h"
#include "user.h"

void busy_loop(int rounds)
{
  volatile int i;
  int r;
  for (r = 0; r < rounds; r++) {
    for (i = 0; i < 20000000; i++) {
    }
  }
}

void o1_test()
{
  int i;
  int levels[4] = {-5, -2, 1, 4};

  for (i = 0; i < 4; i++) {
    if (fork() == 0) {
      int start, end;
      nice(levels[i]);
      start = uptime();
      busy_loop(5);
      end = uptime();
      printf(1, "[child] nice=%d runtime=%d ticks\n", levels[i], end - start);
      exit();
    }
  }

  for (i = 0; i < 4; i++)
    wait();
}

int main(int argc, char **argv)
{
  printf(1, "====Testing O(1) Scheduler====\n");
  o1_test();
  printf(1, "====Finished Testing====\n");
  exit();
}
