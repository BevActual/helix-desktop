// helix-transcribe: one transcription per process. Reads `u32 count` then `count` signed 16-bit samples (16 kHz mono)
// on stdin and writes one JSON line on stdout. It runs the CPU path of whisper.cpp only, so it never competes with the
// chat model for the GPU.
//
// Hardening: no new privileges, no core dumps, and a syscall filter that refuses creating sockets, running programs,
// tracing and mounting. The service's own sandbox (no network, no home) applies underneath.
#include "whisper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr std::uint32_t kMaxSamples = 16000 * 600;

  void say(const nlohmann::json& value) {
    const std::string line = value.dump() + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
  }

  [[noreturn]] void fail(const std::string& why) {
    say({{"ok", false}, {"error", why}});
    std::exit(1);
  }

  void harden() {
    ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    const rlimit noCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
#if defined(__x86_64__)
    constexpr std::uint32_t arch = AUDIT_ARCH_X86_64;
    const int denied[] = {SYS_socket,      SYS_connect,      SYS_bind,  SYS_listen,  SYS_accept, SYS_execve,
                          SYS_execveat,    SYS_ptrace,       SYS_mount, SYS_umount2, SYS_reboot, SYS_kexec_load,
                          SYS_init_module, SYS_finit_module, SYS_setns, SYS_unshare};
    std::vector<sock_filter> program;
    program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    program.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0));
    program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
    for (const int number : denied) {
      program.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(number), 0, 1));
      program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
    }
    program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    sock_fprog fprog{static_cast<unsigned short>(program.size()), program.data()};
    ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog);
#endif
  }

  bool readAll(void* into, std::size_t bytes) {
    auto* out = static_cast<std::uint8_t*>(into);
    while (bytes > 0) {
      const ssize_t n = ::read(0, out, bytes);
      if (n <= 0)
        return false;
      out += n;
      bytes -= static_cast<std::size_t>(n);
    }
    return true;
  }

} // namespace

int main(int argc, char** argv) {
  std::string model, language = "auto";
  int threads = std::clamp(static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)) / 2, 2, 8);
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    if (key == "--model")
      model = argv[i + 1];
    else if (key == "--language")
      language = argv[i + 1];
    else if (key == "--threads")
      threads = std::clamp(std::atoi(argv[i + 1]), 1, 16);
  }
  if (model.empty())
    fail("no model given");
  // Two letters (or three), or "auto": nothing else reaches the engine.
  const bool plain = language == "auto"
      || (language.size() >= 2 && language.size() <= 3 && std::all_of(language.begin(), language.end(), [](char c) {
                       return c >= 'a' && c <= 'z';
                     }));
  if (!plain)
    fail("bad language");

  harden();

  std::uint32_t count = 0;
  if (!readAll(&count, 4) || count == 0 || count > kMaxSamples)
    fail("bad audio");
  std::vector<std::int16_t> pcm(count);
  if (!readAll(pcm.data(), static_cast<std::size_t>(count) * 2))
    fail("short audio");

  std::vector<float> samples(count);
  float peak = 0.0F;
  for (std::uint32_t i = 0; i < count; ++i) {
    samples[i] = static_cast<float>(pcm[i]) / 32768.0F;
    peak = std::max(peak, std::fabs(samples[i]));
  }
  if (peak < 0.004F
      || count < 3200) { // silence, or under a fifth of a second: nothing to hear (the engine would invent words)
    say({{"ok", true}, {"text", ""}, {"language", language == "auto" ? "" : language}});
    return 0;
  }

  whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
  whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = false;
  whisper_context* ctx = whisper_init_from_file_with_params(model.c_str(), cparams);
  if (!ctx)
    fail("the speech model could not be loaded");

  whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.print_progress = false;
  params.print_realtime = false;
  params.print_timestamps = false;
  params.print_special = false;
  params.translate = false;
  params.no_context = true;
  params.n_threads = threads;
  params.language = language.c_str();
  params.detect_language = false;
  params.suppress_blank = true;
  params.no_speech_thold = 0.6F;
  if (whisper_full(ctx, params, samples.data(), static_cast<int>(samples.size())) != 0) {
    whisper_free(ctx);
    fail("the transcription failed");
  }
  std::string text;
  for (int i = 0, n = whisper_full_n_segments(ctx); i < n; ++i)
    text += whisper_full_get_segment_text(ctx, i);
  const int langId = whisper_full_lang_id(ctx);
  const std::string spoken = langId >= 0 ? whisper_lang_str(langId) : (language == "auto" ? "" : language);
  whisper_free(ctx);

  // Tidy: trim, and drop the engine's markers for "nothing was said".
  const auto trim = [](std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
  };
  trim(text);
  for (const char* marker : {"[BLANK_AUDIO]", "(silence)", "[silence]", "[ Silence ]"}) {
    for (auto at = text.find(marker); at != std::string::npos; at = text.find(marker))
      text.erase(at, std::strlen(marker));
  }
  trim(text);
  say({{"ok", true}, {"text", text}, {"language", spoken}});
  return 0;
}
