#include "inference/llama_backend.h"

#include "inference/schema_grammar.h"

#include <ggml-backend.h>
#include <ggml.h>
#include <llama.h>

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

struct ContextDeleter { void operator()(llama_context* c) const { llama_free(c); } };
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
    files.push_back({entry.path().filename().string(), static_cast<std::uint64_t>(entry.file_size(ec))});
  }
  std::vector<ModelInfo> out;
  for (const auto& file : files) {
    const auto shard = parseShard(file.name);
    if (!shard) { out.push_back({file.name, file.bytes, file.name == loaded}); continue; }
    if (shard->index != 1) continue;  // a model split into shards is listed once, by its first shard (the engine finds the rest)
    std::uint64_t total = 0;
    int found = 0;
    for (const auto& other : files) {
      const auto candidate = parseShard(other.name);
      if (candidate && candidate->base == shard->base && candidate->total == shard->total) { total += other.bytes; ++found; }
    }
    if (found == shard->total) out.push_back({file.name, total, file.name == loaded});  // an incomplete set is not offered
  }
  std::ranges::sort(out, {}, &ModelInfo::id);
  return out;
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
  llama_model* loaded = llama_model_load_from_file(path.c_str(), params);
  if (!loaded) { error = "the model could not be loaded: " + name; return false; }
  std::lock_guard lock(mutex_);
  model_ = loaded;
  loaded_ = name;
  return true;
}

void LlamaBackend::unload() {
  std::lock_guard lock(mutex_);
  if (model_) llama_model_free(model_);
  model_ = nullptr;
  loaded_.clear();
}

bool LlamaBackend::isLoaded() const {
  std::lock_guard lock(mutex_);
  return model_ != nullptr;
}

std::string LlamaBackend::formatPrompt(const nlohmann::json& messages) const {
  std::vector<std::string> roles, contents;
  for (const auto& message : messages) {
    roles.push_back(message.at("role").get<std::string>());
    contents.push_back(message.at("content").get<std::string>());
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
    try { grammar = grammarFromToolSchema(nlohmann::json::parse(request.jsonSchema)); }
    catch (const std::exception& problem) { outcome.error = std::string("the output schema is not supported: ") + problem.what(); return outcome; }
  }

  const std::string prompt = formatPrompt(request.messages);
  std::vector<llama_token> tokens(prompt.size() + 16);
  int32_t count = llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                                 static_cast<int32_t>(tokens.size()), false, true);
  if (count < 0) {
    tokens.resize(static_cast<std::size_t>(-count));
    count = llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                           static_cast<int32_t>(tokens.size()), false, true);
  }
  if (count <= 0) { outcome.error = "the prompt could not be tokenized"; return outcome; }
  tokens.resize(static_cast<std::size_t>(count));

  const std::uint32_t trained = static_cast<std::uint32_t>(std::max(llama_model_n_ctx_train(model), 512));
  const std::uint32_t contextTokens = std::min(config_.contextTokens, trained);
  if (static_cast<std::uint32_t>(count) + request.maxTokens > contextTokens) {
    outcome.error = "the request does not fit in the model's context window";
    return outcome;
  }

  llama_context_params contextParams = llama_context_default_params();
  contextParams.n_ctx = contextTokens;
  contextParams.n_batch = std::max<std::uint32_t>(512, static_cast<std::uint32_t>(count));
  contextParams.no_perf = true;
  std::unique_ptr<llama_context, ContextDeleter> context(llama_init_from_model(model, contextParams));
  if (!context) { outcome.error = "could not create a context for the model"; return outcome; }

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

  outcome.promptTokens = static_cast<std::uint32_t>(count);
  Utf8Splitter utf8;
  llama_batch batch = llama_batch_get_one(tokens.data(), count);
  llama_token token = 0;  // outlives the loop: the next batch points at it
  char piece[256];
  for (std::uint32_t generated = 0; generated < request.maxTokens; ++generated) {
    if (cancelled()) { outcome.cancelled = true; outcome.error = "cancelled"; break; }
    if (llama_decode(context.get(), batch) != 0) { outcome.error = "the engine failed while generating"; break; }
    token = llama_sampler_sample(sampler.get(), context.get(), -1);
    if (llama_vocab_is_eog(vocab, token)) { outcome.ok = true; break; }
    const int32_t length = llama_token_to_piece(vocab, token, piece, sizeof piece, 0, false);
    if (length > 0) {
      const std::string text = utf8.push(std::string(piece, static_cast<std::size_t>(length)));
      if (!text.empty()) { outcome.text += text; onDelta(text); }
    }
    ++outcome.completionTokens;
    batch = llama_batch_get_one(&token, 1);
    if (generated + 1 == request.maxTokens) outcome.truncated = true;  // ran out of tokens before the model finished
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
