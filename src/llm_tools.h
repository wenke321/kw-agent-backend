#ifndef LLM_TOOLS_H
#define LLM_TOOLS_H

#include "json.hpp"
#include <functional>
#include <string>
#include <unordered_map>
namespace kw {

typedef std::function<std::string(const nlohmann::json &)> tool_handler;

class llm_tools {
public:
  llm_tools();

  nlohmann::json get_tools_json();

  void register_tool(const std::string &_name, const std::string &_description,
                     const nlohmann::json &_parameters, tool_handler _handler);
  void register_tool(const nlohmann::json &_definitions, tool_handler _handler);

  void unregister(const std::string &name);

  std::string invoke(const std::string &_name, const nlohmann::json &_args);

  template <class return_type, class... Args>
  void regist(std::string &_name, return_type (*func)(Args...));

private:
  std::unordered_map<std::string, tool_handler> tools;
  nlohmann::json definitions;
  int tool_cnt;
};

} // namespace kw

#endif