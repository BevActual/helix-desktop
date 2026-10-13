// One local diffusion request per process. JSON in; bounded JSONL progress and base64 PNG out.
#include "inference/image_generator.h"
#include "stable-diffusion.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace helix::inference;
namespace {
  void say(const nlohmann::json& frame) {
    const auto line = frame.dump() + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
  }
  [[noreturn]] void fail(const std::string& reason) {
    say({{"ok", false}, {"error", reason}});
    std::exit(1);
  }
  void harden() {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
      fail("could not restrict image worker privileges");
    const rlimit noCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
#if defined(__x86_64__)
    const int denied[] = {SYS_socket,      SYS_connect,      SYS_bind,  SYS_listen,  SYS_accept, SYS_execve,
                          SYS_execveat,    SYS_ptrace,       SYS_mount, SYS_umount2, SYS_reboot, SYS_kexec_load,
                          SYS_init_module, SYS_finit_module, SYS_setns, SYS_unshare};
    std::vector<sock_filter> program;
    program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    program.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
    program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
    for (int number : denied) {
      program.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(number), 0, 1));
      program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
    }
    program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    sock_fprog filter{static_cast<unsigned short>(program.size()), program.data()};
    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter) != 0)
      fail("could not install image worker syscall filter");
#endif
  }
} // namespace

int main(int argc, char** argv) try {
  if (argc != 3 || std::string(argv[1]) != "--model")
    fail("an installed model is required");
  harden();
  std::string input;
  for (char c; std::cin.get(c) && c != '\n';) {
    if (input.size() >= 16384)
      fail("image request is too large");
    input += c;
  }
  const auto json = nlohmann::json::parse(input);
  ChatRequest request;
  request.imagePrompt = json.at("prompt").get<std::string>();
  request.imageWidth = json.at("width").get<std::uint32_t>();
  request.imageHeight = json.at("height").get<std::uint32_t>();
  request.imageSteps = json.at("steps").get<std::uint32_t>();
  request.imageSeed = json.at("seed").get<std::uint64_t>();
  if (auto valid = validateImageRequest(request); !valid)
    fail(valid.error());
  sd_set_log_callback(
      [](sd_log_level_t level, const char* text, void*) {
        if (level >= SD_LOG_WARN)
          std::fprintf(stderr, "image engine: %.2000s", text);
      },
      nullptr
  );
  sd_set_progress_callback(
      [](int step, int steps, float, void*) {
        say({{"progress", steps > 0 ? static_cast<double>(step) / steps : 0}, {"stage", "Generating image"}});
      },
      nullptr
  );
  say({{"progress", 0.0}, {"stage", "Loading image model"}});
  sd_ctx_params_t contextParams;
  sd_ctx_params_init(&contextParams);
  contextParams.model_path = argv[2];
  contextParams.n_threads = std::clamp(static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN)) / 2, 2, 6);
  contextParams.enable_mmap = true;
  contextParams.rng_type = CPU_RNG;
  contextParams.max_vram = "8";
  sd_ctx_t* context = new_sd_ctx(&contextParams);
  if (!context)
    fail("the image model could not be loaded");
  sd_img_gen_params_t params;
  sd_img_gen_params_init(&params);
  params.prompt = request.imagePrompt.c_str();
  params.negative_prompt = "";
  params.width = request.imageWidth;
  params.height = request.imageHeight;
  params.seed = request.imageSeed;
  params.batch_count = 1;
  params.sample_params.sample_steps = request.imageSteps;
  params.sample_params.guidance.txt_cfg = 1.0;
  params.sample_params.sample_method = EULER_SAMPLE_METHOD;
  params.sample_params.scheduler = DISCRETE_SCHEDULER;
  sd_image_t* generated = nullptr;
  int count = 0;
  if (!generate_image(context, &params, &generated, &count)
      || count != 1
      || !generated
      || generated[0].channel != 3
      || generated[0].width != request.imageWidth
      || generated[0].height != request.imageHeight) {
    if (generated)
      free_sd_images(generated, count);
    free_sd_ctx(context);
    fail("image generation failed");
  }
  const auto size = static_cast<std::size_t>(request.imageWidth) * request.imageHeight * 3;
  std::vector<std::uint8_t> pixels(generated[0].data, generated[0].data + size);
  free_sd_images(generated, count);
  free_sd_ctx(context);
  say({{"progress", 1.0}, {"stage", "Saving image"}});
  const auto png = encodePng(pixels, request.imageWidth, request.imageHeight);
  if (png.empty())
    fail("could not encode the image");
  say({{"ok", true}, {"png", png}});
  return 0;
} catch (const std::exception&) {
  fail("invalid image request or engine failure");
}
