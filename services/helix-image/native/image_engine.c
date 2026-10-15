/* One local diffusion request. Called from the Rust helix-image worker. */

#include "stable-diffusion.h"

#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef void (*ProgressFn)(double fraction, const char* stage);

static ProgressFn g_progress = NULL;

static void on_log(enum sd_log_level_t level, const char* text, void* data) {
  (void)data;
  if (level >= SD_LOG_WARN && text)
    fprintf(stderr, "image engine: %.2000s", text);
}

static void on_sd_progress(int step, int steps, float time, void* data) {
  (void)time;
  (void)data;
  if (!g_progress)
    return;
  const double fraction = steps > 0 ? (double)step / (double)steps : 0.0;
  g_progress(fraction, "Generating image");
}

static void harden(void) {
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return;
  const struct rlimit no_core = {0, 0};
  setrlimit(RLIMIT_CORE, &no_core);
#if defined(__x86_64__)
  const int denied[] = {SYS_socket,      SYS_connect,      SYS_bind,  SYS_listen,  SYS_accept, SYS_execve,
                        SYS_execveat,    SYS_ptrace,       SYS_mount, SYS_umount2, SYS_reboot, SYS_kexec_load,
                        SYS_init_module, SYS_finit_module, SYS_setns, SYS_unshare};
  struct sock_filter program[4 + (sizeof denied / sizeof denied[0]) * 2 + 1];
  size_t count = 0;
  program[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
  program[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
  program[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
  program[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
  for (size_t i = 0; i < sizeof denied / sizeof denied[0]; ++i) {
    program[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)denied[i], 0, 1);
    program[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
  }
  program[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  struct sock_fprog filter = {.len = (unsigned short)count, .filter = program};
  prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter);
#endif
}

static int clamp_threads(void) {
  long processors = sysconf(_SC_NPROCESSORS_ONLN);
  if (processors < 0)
    processors = 4;
  int threads = (int)processors / 2;
  if (threads < 2)
    threads = 2;
  if (threads > 6)
    threads = 6;
  return threads;
}

int helix_image_generate(
    const char* model_path, const char* prompt, uint32_t width, uint32_t height, uint32_t steps, uint64_t seed,
    ProgressFn progress, uint8_t** rgb_out, size_t* rgb_len) {
  if (!model_path || !prompt || !rgb_out || !rgb_len || width == 0 || height == 0)
    return -1;
  *rgb_out = NULL;
  *rgb_len = 0;
  g_progress = progress;
  harden();

  sd_set_log_callback(on_log, NULL);
  sd_set_progress_callback(on_sd_progress, NULL);
  if (progress)
    progress(0.0, "Loading image model");

  sd_ctx_params_t context_params;
  sd_ctx_params_init(&context_params);
  context_params.model_path = model_path;
  context_params.n_threads = clamp_threads();
  context_params.enable_mmap = true;
  context_params.rng_type = CPU_RNG;
  context_params.max_vram = "8";
  sd_ctx_t* context = new_sd_ctx(&context_params);
  if (!context)
    return -2;

  sd_img_gen_params_t params;
  sd_img_gen_params_init(&params);
  params.prompt = prompt;
  params.negative_prompt = "";
  params.width = (int)width;
  params.height = (int)height;
  params.seed = (int64_t)seed;
  params.batch_count = 1;
  params.sample_params.sample_steps = (int)steps;
  params.sample_params.guidance.txt_cfg = 1.0f;
  params.sample_params.sample_method = EULER_SAMPLE_METHOD;
  params.sample_params.scheduler = DISCRETE_SCHEDULER;

  sd_image_t* generated = NULL;
  int count = 0;
  const bool ok = generate_image(context, &params, &generated, &count) && count == 1 && generated
      && generated[0].channel == 3 && generated[0].width == width && generated[0].height == height;
  if (!ok) {
    if (generated)
      free_sd_images(generated, count);
    free_sd_ctx(context);
    return -3;
  }
  const size_t size = (size_t)width * height * 3;
  uint8_t* copy = malloc(size);
  if (!copy) {
    free_sd_images(generated, count);
    free_sd_ctx(context);
    return -4;
  }
  memcpy(copy, generated[0].data, size);
  free_sd_images(generated, count);
  free_sd_ctx(context);
  if (progress)
    progress(1.0, "Saving image");
  *rgb_out = copy;
  *rgb_len = size;
  return 0;
}
