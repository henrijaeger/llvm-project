#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

static atomic_int release_workers;
volatile int go_b;

__attribute__((noinline)) int step_target(int x) {
  int y = x + 1; // in step_target
  return y;
}

__attribute__((noinline)) void worker_hit(void) {}
__attribute__((noinline)) void stop_a(void) {}
__attribute__((noinline)) void stop_b(void) {}

static void *worker(void *arg) {
  // Spin so that both workers reach their first breakpoint together.
  while (!atomic_load(&release_workers))
    ;
  for (int i = 0; i < 20; ++i)
    worker_hit();
  return NULL;
}

static void *thread_a(void *arg) {
  stop_a();
  return NULL;
}

static void *thread_b(void *arg) {
  // Released by the debugger.
  while (!go_b)
    ;
  stop_b();
  return NULL;
}

int main(int argc, char **argv) {
  int v = step_target(1); // break here
  v = step_target(v);

  pthread_t workers[2];
  for (int i = 0; i < 2; ++i)
    pthread_create(&workers[i], NULL, worker, NULL);
  atomic_store(&release_workers, 1);
  for (int i = 0; i < 2; ++i)
    pthread_join(workers[i], NULL);

  // Only the test that releases thread B passes an argument.
  if (argc > 1) {
    pthread_t a, b;
    pthread_create(&b, NULL, thread_b, NULL);
    pthread_create(&a, NULL, thread_a, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
  }
  return v;
}
