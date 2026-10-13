#include "inference/llama_backend.h"

#include "inference/schema_grammar.h"

#include <ggml-backend.h>
#include <ggml.h>
#include <llama.h>
#include <mtmd-helper.h>
#include <mtmd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <regex>

namespace helix::inference {

namespace {

namespace fs = std::filesystem;

void quietLog(ggml_log_level level, const char* text, void*) {
  if (level >= GGML_LOG_LEVEL_WARN) std::fputs(text, stderr);
}

void initBackendOnce() {
  static const bool initialised = [] {
    llama_log_set(quietLog, nullptr);
    llama_backend_init();
    return true;
  }();
  (void)initialised;
}

struct SamplerDeleter { void operator()(llama_sampler* s) const { llama_sampler_free(s); } };

// Keeps bytes that end in the middle of a UTF-8 character until the rest arrives, so only whole characters are sent.
class Utf8Splitter {
public:
  std::string push(const std::string& bytes) {
    pending_ += bytes;
    std::size_t end = pending_.size();
    // Walk back over an incomplete trailing sequence.
    std::size_t back = 0;
    while (back < 4 && back < pending_.size()) {
      const auto byte = static_cast<unsigned char>(pending_[pending_.size() - 1 - back]);
      if ((byte & 0xC0) == 0x80) { ++back; continue; }  // continuation byte
      const std::size_t need = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
      if (need > back + 1) end = pending_.size() - 1 - back;  // the lead byte's character is not complete yet
      break;
    }
    std::string ready = pending_.substr(0, end);
    pending_.erase(0, end);
    return ready;
  }
  std::string flush() { std::string rest; rest.swap(pending_); return rest; }

private:
  std::string pending_;
};

}  // namespace

LlamaBackend::LlamaBackend(Config config) : config_(std::move(config)) { initBackendOnce(); }

LlamaBackend::~LlamaBackend() { unload(); }

namespace {

// "name-00001-of-00003.gguf" -> {"name", 1, 3}. Only files in that exact shape are shards.
struct Shard { std::string base; int index = 0; int total = 0; };
std::optional<Shard> parseShard(const std::string& file) {
  static const std::regex pattern(R"(^(.+)-(\d{5})-of-(\d{5})\.gguf$)");
  std::smatch match;
  if (!std::regex_match(file, match, pattern)) return std::nullopt;
  return Shard{match[1], std::stoi(match[2]), std::stoi(match[3])};
}

}  // namespace

std::vector<ModelInfo> LlamaBackend::models() {
  std::error_code ec;
  std::string loaded;
  { std::lock_guard lock(mutex_); loaded = loaded_; }
  struct File { std::string name; std::uint64_t bytes; };
  std::vector<File> files;
  for (const auto& entry : fs::directory_iterator(config_.modelDir, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".gguf") continue;
    if (entry.path().filename().string().rfind("mmproj", 0) == 0) continue;  // a projector is part of a model, not a model
    files.push_back({entry.path().filename().string(), static_cast<std::uint64_t>(entry.file_size(ec))});
  }
  std::vector<ModelInfo> out;
  for (const auto& file : files) {
    const auto shard = parseShard(file.name);
    const bool vision = !projectorFor(file.name).empty();
    if (!shard) { out.push_back({file.name, file.bytes, file.name == loaded, vision}); continue; }
    if (shard->index != 1) continue;  // a model split into shards is listed once, by its first shard (the engine finds the rest)
    std::uint64_t total = 0;
    int found = 0;
    for (const auto& other : files) {
      const auto candidate = parseShard(other.name);
      if (candidate && candidate->base == shard->base && candidate->total == shard->total) { total += other.bytes; ++found; }
    }
    if (found == shard->total) out.push_back({file.name, total, file.name == loaded, vision});  // an incomplete set is not offered
  }
  std::ranges::sort(out, {}, &ModelInfo::id);
  return out;
}

// "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf" -> "Qwen2.5-VL-7B-Instruct"; a sharded "name-00001-of-00002.gguf" -> "name".
static std::string baseOf(std::string file) {
  if (file.size() > 5 && file.compare(file.size() - 5, 5, ".gguf") == 0) file.resize(file.size() - 5);
  static const std::regex shard(R"(-\d{5}-of-\d{5}$)");
  file = std::regex_replace(file, shard, "");
  static const std::regex quant(R"(-(?:[iI]?[qQ]\d[A-Za-z0-9_]*|[fF](?:16|32)|[bB][fF]16)$)");
  return std::regex_replace(file, quant, "");
}

std::filesystem::path LlamaBackend::projectorFor(const std::string& model) const {
  std::error_code ec;
  const std::string base = baseOf(model);
  if (base.empty()) return {};
  std::vector<fs::path> candidates;
  for (const auto& entry : fs::directory_iterator(config_.modelDir, ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file(ec) && entry.path().extension() == ".gguf" && name.rfind("mmproj-" + base, 0) == 0) candidates.push_back(entry.path());
  }
  std::ranges::sort(candidates);
  return candidates.empty() ? fs::path{} : candidates.front();
}

bool LlamaBackend::ensureVision(std::string& error) {
  if (vision_) return true;
  const fs::path projector = projectorFor(loaded_);
  if (projector.empty()) {
    error = "this model cannot read pictures: no vision projector is installed for " + loaded_;
    return false;
  }
  mtmd_context_params params = mtmd_context_params_default();
  params.use_gpu = config_.gpuLayers != 0;
  params.print_timings = false;
  params.warmup = false;
  vision_ = mtmd_init_from_file(projector.c_str(), model_, params);
  if (!vision_) { error = "the vision projector could not be loaded: " + projector.filename().string(); return false; }
  if (!mtmd_support_vision(vision_)) { freeVision(); error = "the projector does not support pictures"; return false; }
  return true;
}

void LlamaBackend::freeVision() {
  if (vision_) mtmd_free(vision_);
  vision_ = nullptr;
}

bool LlamaBackend::load(const std::string& requested, std::string& error) {
  const auto available = models();
  std::string name = requested;
  if (name.empty()) {
    if (available.empty()) { error = "no models are installed in " + config_.modelDir.string(); return false; }
    name = available.front().id;
  }
  { std::lock_guard lock(mutex_); if (model_ && loaded_ == name) return true; }
  // Only a model the backend itself lists can be loaded, so a client cannot point the loader at an arbitrary path or at
  // a stray shard.
  if (std::ranges::none_of(available, [&](const ModelInfo& model) { return model.id == name; })) {
    error = "no such model: " + name;
    return false;
  }
  const fs::path path = config_.modelDir / name;

  unload();
  llama_model_params params = llama_model_default_params();
  params.n_gpu_layers = config_.gpuLayers;
  struct Report { const ProgressFn* fn; const std::string* model; float last; } report{&progress_, &name, -1.0F};
  if (progress_) {
    progress_(name, 0.0);
    params.progress_callback_user_data = &report;
    params.progress_callback = [](float fraction, void* data) -> bool {
      auto* r = static_cast<Report*>(data);
      if (fraction - r->last >= 0.02F || fraction >= 1.0F) { r->last = fraction; (*r->fn)(*r->model, fraction); }
      return true;
    };
  }
  llama_model* loaded = llama_model_load_from_file(path.c_str(), params);
  if (!loaded) { error = "the model could not be loaded: " + name; return false; }
  std::lock_guard lock(mutex_);
  model_ = loaded;
  loaded_ = name;
  return true;
}

void LlamaBackend::unload() {
  std::lock_guard lock(mutex_);
  freeVision();  // before the model it was built for
  pictureInMemory_ = false;
  if (context_) llama_free(context_);  // before the model it belongs to
  context_ = nullptr;
  contextTokens_ = 0;
  kvTokens_.clear();
  if (model_) llama_model_free(model_);
  model_ = nullptr;
  loaded_.clear();
}

void LlamaBackend::invalidateCache() {
  if (context_) llama_memory_clear(llama_get_memory(context_), true);
  kvTokens_.clear();
}

bool LlamaBackend::ensureContext(std::uint32_t contextTokens) {
  if (context_ && contextTokens_ == contextTokens) return true;
  if (context_) llama_free(context_);
  context_ = nullptr;
  kvTokens_.clear();
  llama_context_params params = llama_context_default_params();
  params.n_ctx = contextTokens;
  params.n_batch = 512;
  params.no_perf = true;
  context_ = llama_init_from_model(model_, params);
  contextTokens_ = context_ ? contextTokens : 0;
  return context_ != nullptr;
}

bool LlamaBackend::isLoaded() const {
  std::lock_guard lock(mutex_);
  return model_ != nullptr;
}

std::string LlamaBackend::formatPrompt(const nlohmann::json& messages, const std::string& imageMarker) const {
  std::vector<std::string> roles, contents;
  for (const auto& message : messages) {
    roles.push_back(message.at("role").get<std::string>());
    contents.push_back(flattenContent(message.at("content"), imageMarker));
  }
  std::vector<llama_chat_message> chat;
  for (std::size_t i = 0; i < roles.size(); ++i) chat.push_back({roles[i].c_str(), contents[i].c_str()});

  const char* tmpl = llama_model_chat_template(model_, nullptr);
  if (tmpl) {
    std::string out(4096, '\0');
    int32_t length = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, out.data(), static_cast<int32_t>(out.size()));
    if (length > static_cast<int32_t>(out.size())) {
      out.assign(static_cast<std::size_t>(length), '\0');
      length = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, out.data(), static_cast<int32_t>(out.size()));
    }
    if (length > 0) { out.resize(static_cast<std::size_t>(length)); return out; }
  }
  std::string plain;  // a model without a template: a plain transcript
  for (std::size_t i = 0; i < roles.size(); ++i) plain += roles[i] + ": " + contents[i] + "\n";
  return plain + "assistant: ";
}

Outcome LlamaBackend::chat(const ChatRequest& request, const std::function<void(const std::string&)>& onDelta,
                           const std::function<bool()>& cancelled) {
  Outcome outcome;
  std::string error;
  if (!load(request.model, error)) { outcome.error = error; return outcome; }
  llama_model* model = model_;
  const llama_vocab* vocab = llama_model_get_vocab(model);

  // The grammar first: a request that cannot be constrained is refused, never run unconstrained.
  std::string grammar;
  if (!request.jsonSchema.empty()) {
    try { grammar = grammarFromToolSchema(nlohmann::json::parse(request.jsonSchema), GrammarOptions{.reasonFirst = request.reasonFirst}); }
    catch (const std::exception& problem) { outcome.error = std::string("the output schema is not supported: ") + problem.what(); return outcome; }
  }

  // Pictures: the memory of a previous picture turn cannot be reused (it holds embeddings, not tokens).
  if (pictureInMemory_) { invalidateCache(); pictureInMemory_ = false; }
  const bool withPictures = !request.images.empty();
  if (withPictures && !ensureVision(error)) { outcome.error = error; return outcome; }
  const std::string prompt = formatPrompt(request.messages, withPictures ? std::string(mtmd_default_marker()) : std::string());
  std::vector<llama_token> tokens;  // stays empty with pictures: the multimodal library decodes those requests itself
  int32_t count = 0;
  if (!withPictures) {
    tokens.resize(prompt.size() + 16);
    count = llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                           static_cast<int32_t>(tokens.size()), false, true);
    if (count < 0) {
      tokens.resize(static_cast<std::size_t>(-count));
      count = llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                             static_cast<int32_t>(tokens.size()), false, true);
    }
    if (count <= 0) { outcome.error = "the prompt could not be tokenized"; return outcome; }
    tokens.resize(static_cast<std::size_t>(count));
  }

  const std::uint32_t trained = static_cast<std::uint32_t>(std::max(llama_model_n_ctx_train(model), 512));
  const std::uint32_t contextTokens = std::min(config_.contextTokens, trained);
  if (!withPictures && static_cast<std::uint32_t>(count) + request.maxTokens > contextTokens) {
    outcome.error = "the request does not fit in the model's context window";
    return outcome;
  }

  if (!ensureContext(contextTokens)) { outcome.error = "could not create a context for the model"; return outcome; }
  llama_context* context = context_;

  std::unique_ptr<llama_sampler, SamplerDeleter> sampler(llama_sampler_chain_init(llama_sampler_chain_default_params()));
  if (!grammar.empty()) {
    llama_sampler* constraint = llama_sampler_init_grammar(vocab, grammar.c_str(), "root");
    if (!constraint) { outcome.error = "the output grammar was rejected by the engine"; return outcome; }
    llama_sampler_chain_add(sampler.get(), constraint);
  }
  if (request.temperature <= 0.0) {
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());
  } else {
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_temp(static_cast<float>(request.temperature)));
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_dist(std::random_device{}()));
  }

  if (withPictures) {
    // Text and pictures through the multimodal library: it splits the prompt at the markers, encodes each picture and decodes
    // the lot, leaving the memory ready for sampling. Nothing is reused from earlier requests.
    invalidateCache();
    std::vector<mtmd_bitmap*> bitmaps;
    for (const auto& image : request.images) bitmaps.push_back(mtmd_bitmap_init(image.width, image.height, image.rgb.data()));
    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    const mtmd_input_text text{prompt.c_str(), prompt.size(), /*add_special=*/true, /*parse_special=*/true};
    std::vector<const mtmd_bitmap*> view(bitmaps.begin(), bitmaps.end());
    const int32_t split = mtmd_tokenize(vision_, chunks, &text, view.data(), view.size());
    for (auto* bitmap : bitmaps) mtmd_bitmap_free(bitmap);
    if (split != 0) {
      mtmd_input_chunks_free(chunks);
      outcome.error = split == 1 ? "the number of pictures does not match the prompt" : "the pictures could not be prepared for the model";
      return outcome;
    }
    const std::size_t promptTokens = mtmd_helper_get_n_tokens(chunks);
    if (promptTokens + request.maxTokens > contextTokens) {
      mtmd_input_chunks_free(chunks);
      outcome.error = "the pictures and text do not fit in the model's context window";
      return outcome;
    }
    if (cancelled()) { mtmd_input_chunks_free(chunks); outcome.cancelled = true; outcome.error = "cancelled"; return outcome; }
    llama_pos newPast = 0;
    const int32_t evaluated = mtmd_helper_eval_chunks(vision_, context, chunks, 0, 0, 512, /*logits_last=*/true, &newPast);
    mtmd_input_chunks_free(chunks);
    pictureInMemory_ = true;
    if (evaluated != 0) {
      invalidateCache();
      pictureInMemory_ = false;
      outcome.error = "the engine failed while reading the pictures";
      return outcome;
    }
    outcome.promptTokens = static_cast<std::uint32_t>(promptTokens);
  }

  // Reuse the memory of the previous request for the part of the prompt that has not changed. At least the last prompt
  // token is always decoded afresh, because sampling needs the model's output for it.
  std::size_t shared = 0;
  while (shared < kvTokens_.size() && shared + 1 < tokens.size() && kvTokens_[shared] == tokens[shared]) ++shared;
  if (shared < kvTokens_.size()) {
    if (!llama_memory_seq_rm(llama_get_memory(context), 0, static_cast<llama_pos>(shared), -1)) {
      invalidateCache();  // this model cannot drop part of its memory: start again from nothing
      shared = 0;
    }
    kvTokens_.resize(shared);
  }
  if (!withPictures) outcome.promptTokens = static_cast<std::uint32_t>(count);
  outcome.cachedPromptTokens = static_cast<std::uint32_t>(shared);

  constexpr std::size_t kChunk = 512;  // the context's batch size
  for (std::size_t at = shared; at < tokens.size(); at += kChunk) {
    if (cancelled()) { outcome.cancelled = true; outcome.error = "cancelled"; return outcome; }
    const auto chunk = static_cast<int32_t>(std::min(kChunk, tokens.size() - at));
    if (llama_decode(context, llama_batch_get_one(tokens.data() + at, chunk)) != 0) {
      invalidateCache();
      outcome.error = "the engine failed while reading the prompt";
      return outcome;
    }
    kvTokens_.insert(kvTokens_.end(), tokens.begin() + static_cast<std::ptrdiff_t>(at), tokens.begin() + static_cast<std::ptrdiff_t>(at) + chunk);
  }

  Utf8Splitter utf8;
  llama_token token = 0;  // outlives the loop: each batch points at it
  char piece[256];
  for (std::uint32_t generated = 0; generated < request.maxTokens; ++generated) {
    if (cancelled()) { outcome.cancelled = true; outcome.error = "cancelled"; break; }
    token = llama_sampler_sample(sampler.get(), context, -1);
    if (llama_vocab_is_eog(vocab, token)) { outcome.ok = true; break; }
    const int32_t length = llama_token_to_piece(vocab, token, piece, sizeof piece, 0, false);
    if (length > 0) {
      const std::string text = utf8.push(std::string(piece, static_cast<std::size_t>(length)));
      if (!text.empty()) { outcome.text += text; onDelta(text); }
    }
    ++outcome.completionTokens;
    if (generated + 1 == request.maxTokens) { outcome.truncated = true; break; }  // out of tokens before the model finished
    if (llama_decode(context, llama_batch_get_one(&token, 1)) != 0) {
      invalidateCache();
      outcome.error = "the engine failed while generating";
      break;
    }
    kvTokens_.push_back(token);  // now part of the memory; the last sampled token never is
  }
  (void)utf8.flush();  // an incomplete character at the very end is dropped rather than sent as invalid UTF-8
  if (!outcome.cancelled && outcome.error.empty()) outcome.ok = true;
  // Free text may simply be cut short, but a constrained reply that stops half-way is not a valid reply at all.
  if (outcome.truncated && !grammar.empty()) { outcome.ok = false; outcome.error = "the reply was cut off before it was complete; allow more tokens"; }
  return outcome;
}

nlohmann::json LlamaBackend::snapshot() const {
  // What the engine can compute on, and how much memory is free right now: the budget the service has to respect.
  nlohmann::json devices = nlohmann::json::array();
  for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
    ggml_backend_dev_t device = ggml_backend_dev_get(i);
    std::size_t free = 0, total = 0;
    ggml_backend_dev_memory(device, &free, &total);
    const auto type = ggml_backend_dev_type(device);
    devices.push_back({{"name", ggml_backend_dev_name(device)}, {"description", ggml_backend_dev_description(device)},
                       {"gpu", type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU},
                       {"memory_free", free}, {"memory_total", total}});
  }
  std::lock_guard lock(mutex_);
  return {{"backend", "llama.cpp"}, {"loaded", loaded_}, {"gpuLayers", config_.gpuLayers},
          {"contextTokens", config_.contextTokens}, {"devices", std::move(devices)}};
}

}  // namespace helix::inference
