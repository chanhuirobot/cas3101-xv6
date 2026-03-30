#include "types.h"
#include "stat.h"
#include "user.h"

void prio_test()
{
  int pid = fork();

  if (pid == 0) {
    int i, sum = 0;
    for (i = 0; i < 500000000; i++) sum += i;
    printf(1, "[child]  done (nice=2) sum=%d\n", sum);
    exit();
  } else {
    int i, sum = 0;
    for (i = 0; i < 250000000; i++) sum += i;
    nice(4);
    for (i = 250000000; i < 500000000; i++) sum += i;
    printf(1, "[parent] done (nice=4) sum=%d\n", sum);
    wait();
  }
}

int main(int argc, char **argv)
{
  printf(1, "====Testing Priority Scheduler====\n");
  prio_test();
  printf(1, "====Finished Testing====\n");
  exit();
}
