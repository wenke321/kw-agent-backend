#include "json.hpp"
#include "helpers/log.h"
#include "helpers/function.h"
#include <iostream>

nlohmann::json g_j;
// 递归拼接 "/key1/key2/..."
static std::string concat_keys(const std::string &k) { return "/" + k; }

template <typename... Rest>
static std::string concat_keys(const std::string &k, const Rest &...rest) {
  return "/" + k + concat_keys(rest...);
}

// 设置值：set(value, key1, key2, ..., keyN)
// 所有键会被组合成 JSON Pointer，自动创建不存在的中间对象
template <typename T, typename... Keys>
void set(const T &value, const std::string &key, const Keys &...keys) {
  std::string pointer = "/" + key;
  if constexpr (sizeof...(keys) > 0) {
    pointer += concat_keys(keys...);
  }
  // 通过 JSON Pointer 赋值，自动创建路径上所有对象
  g_j[nlohmann::json::json_pointer(pointer)] = value;
}

// ---------- 辅助函数：写文件 ----------
void write_file(const std::string &name, const std::string &content) {
  std::ofstream f(name, std::ios::binary);
  f.write(content.data(), content.size());
  f.close();
}

// ---------- 辅助函数：删除文件 ----------
void remove_file(const std::string &name) { std::remove(name.c_str()); }

int main() {

  // set("directory", "src", "type");

  // // 等价于 j["src"]["a.cpp"]["type"] = "file";
  // set("file", "src", "a.cpp", "type");

  // // 等价于 j["src"]["a.cpp"]["description"] = "main source";
  // set("main source", "src", "a.cpp", "description");

  // // 也可设置数值、布尔等任意类型
  // set(42, "count");
  // set(true, "enabled");

  // // 导出字符串
  // std::cout << g_j.dump() << std::endl;

  std::string json_s="[{provider: deepseek,chat_url: https://api.deepseek.com/chat/completions,models:[{name: deepseek-flash,efforts: [{ name: none},{ name: low},{ name: high},{ name: max}],defaultEffort: high},{name: deepseek-v4-pro,efforts: [{ name: none},{ name: low},{ name: high},{ name: max}],defaultEffort: high}]}]";

  kw::store_jsonstr("test_store_jsonstr",json_s,"C:/Users/kevin/Desktop");

  std::string load_json;
  std::string err;
  if(kw::load_jsonstr("C:/Users/kevin/Desktop/test_store_jsonstr.json",load_json,err)){
    std::cout<<load_json<<"\n";
  }
  else{
    std::cout<<err<<"\n";
  }

  return 0;
}