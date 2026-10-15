/* Thin C wrapper around whisper.cpp for the Rust helix-transcribe worker.
 * Keeps large whisper_full_params / context structs out of Rust FFI. */

#include "whisper.h"

#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

void helix_transcribe_harden(void) {
  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  const struct rlimit no_core = {0, 0};
  setrlimit(RLIMIT_CORE, &no_core);
#if defined(__x86_64__)
  const int denied[] = {SYS_socket,      SYS_connect,      SYS_bind,  SYS_listen,  SYS_accept, SYS_execve,
                        SYS_execveat,    SYS_ptrace,       SYS_mount, SYS_umount2, SYS_reboot, SYS_kexec_load,
                        SYS_init_module, SYS_finit_module, SYS_setns, SYS_unshare};
  struct sock_filter program[4 + (sizeof(denied) / sizeof(denied[0])) * 2 + 1];
  size_t n = 0;
  program[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
  program[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
  program[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
  program[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
  for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
    program[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)denied[i], 0, 1);
    program[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
  }
  program[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  struct sock_fprog filter = {.len = (unsigned short)n, .filter = program};
  prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter);
#endif
}

static void quiet_log(enum ggml_log_level level, const char *text, void *user) {
  (void)level;
  (void)text;
  (void)user;
}

/* Returns 0 on success. Writes UTF-8 text into `text_out` and language code into `lang_out`. */
int helix_whisper_transcribe(
    const char *model_path,
    const char *language,
    int threads,
    const float *samples,
    int n_samples,
    char *text_out,
    size_t text_cap,
    char *lang_out,
    size_t lang_cap) {
  if (!model_path || !language || !samples || n_samples <= 0 || !text_out || text_cap == 0
      || !lang_out || lang_cap == 0) {
    return -1;
  }
  text_out[0] = '\0';
  lang_out[0] = '\0';

  whisper_log_set(quiet_log, NULL);
  struct whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = false;
  struct whisper_context *ctx = whisper_init_from_file_with_params(model_path, cparams);
  if (!ctx) {
    return -2;
  }

  struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.print_progress = false;
  params.print_realtime = false;
  params.print_timestamps = false;
  params.print_special = false;
  params.translate = false;
  params.no_context = true;
  params.n_threads = threads;
  params.language = language;
  params.detect_language = false;
  params.suppress_blank = true;
  params.no_speech_thold = 0.6f;

  if (whisper_full(ctx, params, samples, n_samples) != 0) {
    whisper_free(ctx);
    return -3;
  }

  size_t used = 0;
  const int n = whisper_full_n_segments(ctx);
  for (int i = 0; i < n; ++i) {
    const char *seg = whisper_full_get_segment_text(ctx, i);
    if (!seg) {
      continue;
    }
    const size_t len = strlen(seg);
    if (used + len + 1 > text_cap) {
      whisper_free(ctx);
      return -4;
    }
    memcpy(text_out + used, seg, len);
    used += len;
    text_out[used] = '\0';
  }

  const int lang_id = whisper_full_lang_id(ctx);
  const char *spoken = lang_id >= 0 ? whisper_lang_str(lang_id) : NULL;
  if (spoken) {
    strncpy(lang_out, spoken, lang_cap - 1);
    lang_out[lang_cap - 1] = '\0';
  } else if (strcmp(language, "auto") != 0) {
    strncpy(lang_out, language, lang_cap - 1);
    lang_out[lang_cap - 1] = '\0';
  }

  whisper_free(ctx);
  return 0;
}
