#ifndef SESSION_H
#define SESSION_H

#include "https/https_client.h"
#include "llm_tools.h"
#include <cstdint>
#include <memory>

//system_prompt
//user_content
//reasoning_content
//assistant_out
//tool_calls
//tool_calls_result
//reasoning_content
//...

//system_prompt
//compressed_context
//user_content
//...

namespace kw {

static const char* sys_prompt="You are a helpful software engineer assistant.\n"
                              "";
static const char* compress_prompt="You are now acting as a compaction engine for this AI coding assistant. Condense the conversation ABOVE into a structured checkpoint that lets another model resume the work with no loss of essential context.\n"
    "\n"
    "Output EXACTLY the Markdown structure below: keep every section, in order. Use terse bullets, not prose paragraphs. Write \"(none)\" for an empty section — never drop a section.\n"
    "\n"
    "## Primary Request and Intent\n"
    "- [the user's original and evolving goals; quote verbatim where the exact wording matters]\n"
    "\n"
    "## Key Technical Concepts\n"
    "- [technologies, frameworks, patterns, and conventions in play]\n"
    "\n"
    "## Files and Code\n"
    "- [exact path: why it matters, key changes or snippets]\n"
    "\n"
    "## Errors and Fixes\n"
    "- [error: how it was resolved, plus any related user feedback]\n"
    "\n"
    "## Pending Jobs\n"
    "- [explicitly requested work not yet completed]\n"
    "\n"
    "## Current Work\n"
    "- [precisely what was in progress at this checkpoint]\n"
    "\n"
    "## Next Step\n"
    "- [the single next action, directly in line with the most recent request, or \"(none)\"]\n"
    "\n"
    "## Critical Context\n"
    "- [decisions and their rationale, constraints, user preferences, open questions, data needed to continue]\n"
    "\n"
    "Rules:\n"
    "- Write concise English engineering prose. Preserve exact file paths, commands, error strings, identifiers, numeric values, function signatures, and syntax fragments.\n"
    "- Capture user feedback and explicit instructions faithfully, especially corrections.\n"
    "- Do NOT mention this summarization request or that the context was compacted.\n"
    "- Output only the checkpoint text: do not call any tool or take any other action.\n";

enum api_provider : uint8_t { deepseek };

enum api_reasoning_effort : uint8_t { low, high, max };

// usage: kw::api_parameters_tag::thinking
enum api_parameters_tag : uint8_t {
  thinking,
  reasoning_effort,
  max_tokens,
  response_format,
  stop,
  stream,
  stream_options,
  temperature,
  top_p,
  tools,
  tool_choice,
  logprobs,
  top_logprobs
};

static const char *api_parameters[] = {"thinking",       "reasoning_effort",
                                       "max_tokens",     "response_format",
                                       "stop",           "stream",
                                       "stream_options", "temperature",
                                       "top_p",          "tools",
                                       "tool_choice",    "logprobs",
                                       "top_logprobs"};

class session {
public:
  session();
  ~session();

  void change_provider(const std::string& _provider,const std::string& _chat_url,
                       const std::string& _key);

  void update_moedl_effort_permission_level(const std::string& _model,const std::string& _effort,int _permission_level);

  void reset(std::string &_model, std::string &_key, std::string &_base_url);
  int get_blance();
  void balance_infos_cb(const https_response &_resp);
  void create_tasks_cb(const https_response &_resp);
  void deal_with_errors(int _status_code);

  // use kw::thinking,kw::reasoning_effort...
  void set_model_paras(kw::api_parameters_tag _p, const std::string _v);

  void register_tool(const std::string &_name, const std::string &_description,
                     const nlohmann::json &_parameters, tool_handler _handler);

  void input(char *_buf, uint32_t _size);

  bool start_turn(const std::string &usr_content, bool use_tools);

  bool start_step(bool use_tools);

  nlohmann::json call_tools(const nlohmann::json &_tool_calls);

  //return whether have tool_call
  bool handle_response(const https_response &_resp);

  //compress current(latest turn) context
  bool compress_ctx();
  void notice_compress_ctx();

  std::function<bool(const std::string& _reasoning_content,
                     const std::string& _assistant_out,
                     const std::string& _tool_call_json)> step_success_cb;
  std::function<void(const std::string& _ctx)> step_fail_cb;

  std::function<void(int cur_size)> ctx_size_cb;
  std::function<void(const std::string& _ctx)> compress_error_cb;

  std::function<bool()> turn_end_cb;

  std::function<void()> reset_turn_cb;

  bool first_turn=true;
  bool first_step=true;
  bool ctx_compressed=false;
  std::string key;
  nlohmann::json cur_ctx=nlohmann::json::array();
  std::string cur_workspace_path;
  std::string cur_user_content;
  int retry_clock;
  std::string permission;

private:
  api_provider provider;
 std::string model;
  std::string reasoning_effort;
  std::string chat_url;
  nlohmann::json model_parameters;
  bool is_available;
  double total_balance;
  std::string my_sys_prompt;
  nlohmann::json cur_chat_body=nlohmann::json::object();
  llm_tools m_tools;
  std::unique_ptr<https_client> client;
};
} // namespace kw

#endif