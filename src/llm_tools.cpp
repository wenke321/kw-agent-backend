#include "llm_tools.h"
#include "helpers/log.h"
#include <string>

kw::llm_tools::llm_tools() : tool_cnt(0) {}

void kw::llm_tools::register_tool(const std::string &_name,
                                  const std::string &_description,
                                  const nlohmann::json &_parameters,
                                  tool_handler _handler) {
  nlohmann::json params = {{"type", "object"}};

  // 2. 如果传入了额外的 properties 和 required，才把它们合并进来
  if (!_parameters.empty()) {
    if (_parameters.contains("properties")) {
      params["properties"] = _parameters["properties"];
    }
    if (_parameters.contains("required")) {
      params["required"] = _parameters["required"];
    }
  } else {
    // 没有参数时也保证 properties 和 required 存在（API 要求）
    params["properties"] = nlohmann::json::object();
  }

  // 3. 组装最终的 tool 对象
  nlohmann::json j = {{"type", "function"},
                      {"function",
                       {{"name", _name},
                        {"description", _description},
                        {"parameters", params}}}};

  register_tool(j, _handler);
}

void kw::llm_tools::register_tool(const nlohmann::json &_definition,
                                  tool_handler _handler) {
  if (_definition["type"].dump() != "\"function\"") {
    LOG_ERROR(" type={},must be function", _definition["type"].dump());
    return;
  }
  tools[_definition["function"]["name"]] = std::move(_handler);
  definitions.push_back(_definition);
  ++tool_cnt;
}

void kw::llm_tools::unregister(const std::string &_name) {
  if (tools.find(_name) != tools.end()) {
    tools.erase(_name);
    for (auto it : definitions) {
      if (it["function"]["name"] == _name) {
        definitions.erase(it);
        return;
      }
    }
    {
      LOG_ERROR(" doesn't in definitions");
    }
  }
  LOG_ERROR(" doesn't in tools");
}

nlohmann::json kw::llm_tools::get_tools_json() { return definitions; }

std::string kw::llm_tools::invoke(const std::string &_name,
                                     const nlohmann::json &_args = {"a"}) {
  auto it = tools.find(_name);
  if (it != tools.end()) {
    return it->second(nlohmann::json::parse(_args.get<std::string>()));
  }
  LOG_ERROR("unregister function");
  return "找不到此工具";
}