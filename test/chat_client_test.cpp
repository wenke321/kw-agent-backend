#include "agent_loop.h"
#include "helpers/function.h"
#include "spdlog/common.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <processenv.h>
#include <sched.h>
#include <sec_api/string_s.h>
#include <string.h>
#include <string>

#include <sys/types.h>

#include <unistd.h>

/*
{
  "discription":"整个文件的功能说明"，
  "symbols":{
    "class A":{
      "description":"类A的功能说明"，
      "implement_file":"类A的实现所在文件名"
      "line":{start_line,end_line}
    },
    "struct B":{
      "description":"结构体B的功能说明"，
      "implement_file":"结构体B的实现所在文件名"
      "line":{start_line,end_line}
    },
    "enum":{...}
  }
}

{
  "discription":"整个文件的功能说明"，
  "symbols":{
    "A(int a,int b)":{
      "description":"函数的功能说明"，
      "line":{start_line,end_line}
    }
    "void A::init(std::string s)":{
      "description":"函数的功能说明"，
      "line":{start_line,end_line}
    }
  }
}

{
  "src":{
   "type":"directory",
   "discription":"...",
   "a.h":{
    "type":"header file",
    "discription":"..."
   }
   "a.cpp":{
    "type":"source file",
    "discription":"..."
   }
  }
}

 */

kw::agent_loop loop();

class json_builder {
public:
  // 新增重载：支持逗号分隔的路径字符串
  // 调用示例：builder.set("directory", "src,type");
  //            builder.set("main source", "src,a.cpp,discription");
  //   template <typename T>
  //   void set(const T &value, const std::string &path, char delimiter = ',') {
  //     auto keys = split(path, delimiter);
  //     if (keys.empty())
  //       return;
  //     std::string pointer;
  //     for (const auto &k : keys) {
  //       pointer += "/" + k;
  //     }
  //     j_[nlohmann::json::json_pointer(pointer)] = value;
  //   }

  // 逗号分隔路径调用：set(value, "src,a.cpp,type")
  template <typename T>
  void set(const T &value, const std::string &path, char delimiter = ',') {
    auto keys = split(path, delimiter);
    set_impl(value, keys);
  }

  nlohmann::json &get() { return j_; }
  const nlohmann::json &get() const { return j_; }
  std::string dump() const { return j_.dump(); }

  void clear() { j_.clear(); }

private:
  nlohmann::json j_;

  // 核心实现：手动遍历键，逐级确保 object
  void set_impl(const nlohmann::json &value,
                const std::vector<std::string> &keys) {
    if (keys.empty())
      return;
    nlohmann::json *node = &j_;
    for (size_t i = 0; i < keys.size() - 1; ++i) {
      const auto &k = keys[i];
      // 如果键不存在或当前值不是对象，则创建或覆盖为对象
      if (!node->is_object() || node->find(k) == node->end()) {
        (*node)[k] = nlohmann::json::object();
      } else if (!(*node)[k].is_object()) {
        // 防止之前被设置成非对象类型，强制覆盖为空对象
        (*node)[k] = nlohmann::json::object();
      }
      node = &(*node)[k];
    }
    // 设置最终键的值
    (*node)[keys.back()] = value;
  }

  // 原始拼接辅助函数
  static std::string concat_keys(const std::string &k) { return "/" + k; }
  template <typename... Rest>
  static std::string concat_keys(const std::string &k, const Rest &...rest) {
    return "/" + k + concat_keys(rest...);
  }

  // 字符串分割
  static std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delim)) {
      // 可选：去除首尾空格
      token.erase(0, token.find_first_not_of(" \t"));
      token.erase(token.find_last_not_of(" \t") + 1);
      if (!token.empty())
        tokens.push_back(token);
    }
    return tokens;
  }
};

json_builder g_j;

void init() {
  loop.set_response_cb([](const https_response &_resp) {
    nlohmann::json j = _resp.body_json();
    if (j.find("error") != j.end()) {
      LOG_ERROR("error={}", j.dump());
    } else
      LOG_INFO("code={0},msg={1}\n", _resp.status_code,
               _resp.body_json()["choices"][0]["message"]["content"].dump());
  });

  //   loop.register_tool(
  //       "open", "open and possibly create a file,same as in glibc",
  //       {{"properties",
  //         {{"path", {{"type", "string"}, {"description", "file path"}}},
  //          {"flags", {{"type", "integer"}, {"description", "flags as in
  //          glibc"}}},
  //          {"mode",
  //           {{"type", "integer"}, {"description", "mode_t as in glibc"}}}}},
  //        {"required", {"path", "flags", "mode"}}},
  //       [](const nlohmann::json &_args) -> nlohmann::json {
  //         int fd = ::open(_args["path"].get<std::string>().data(),
  //                         _args["flags"].get<int>(),
  //                         _args["mode"].get<int>());
  //         if (fd == -1) {
  //           int err = errno;
  //           char buf[256];
  //           ::strerror_s(buf, sizeof(buf), err);
  //           return {"fail", buf};
  //         }
  //         return {"fd", fd};
  //       });

  loop.register_tool(
      "file_insert_line", "insert data in file after a line",
      {{"properties",
        {{"file_name", {{"type", "string"}, {"description", "file path"}}},
         {"line",
          {{"type", "integer"}, {"description", "insert after \"line\""}}},
         {"data", {{"type", "string"}, {"description", "data to insert"}}}}},
       {"required", {"file_name", "line", "data"}}},
      [](const nlohmann::json &_args) -> nlohmann::json {
        auto data = _args["data"].get<std::string>();
        size_t len = data.size();
        return kw::file_insert_line(
            _args["file_name"].get<std::string>().c_str(),
            _args["line"].get<int>(), data.data(), len);
      });

  loop.register_tool(
      "file_read_line", "read part of a file",
      {{"properties",
        {{"file_name", {{"type", "string"}, {"description", "file path"}}},
         {"start", {{"type", "integer"}, {"description", "start line"}}},
         {"end", {{"type", "integer"}, {"description", "end line"}}}}},
       {"required", {"file_name", "start", "end"}}},
      [](const nlohmann::json &_args) -> nlohmann::json {
        return kw::file_read_line(_args["file_name"].get<std::string>().c_str(),
                                  _args["start"].get<int>(),
                                  _args["end"].get<int>());
      });

  loop.register_tool(
      "file_delete_line", "delete part of a file",
      {{"properties",
        {{"file_name", {{"type", "string"}, {"description", "file path"}}},
         {"start", {{"type", "integer"}, {"description", "start line"}}},
         {"end", {{"type", "integer"}, {"description", "end line"}}}}},
       {"required", {"file_name", "start", "end"}}},
      [](const nlohmann::json &_args) -> nlohmann::json {
        return kw::file_delete_line(
            _args["file_name"].get<std::string>().c_str(),
            _args["start"].get<int>(), _args["end"].get<int>());
      });

  loop.register_tool(
      "file_read_entire", "read all of a file",
      {{"properties",
        {{"file_name", {{"type", "string"}, {"description", "file path"}}}}},
       {"required", {"file_name"}}},
      [](const nlohmann::json &_args) -> nlohmann::json {
        return kw::file_read_entire(
            _args["file_name"].get<std::string>().c_str());
      });

  //   loop.register_tool(
  //       "change_task",
  //       "改变任务描述，或新建任务(新建任务只能顺序添加,比如在1.1之后添加1."
  //       "2，在1之后添加2)",
  //       {{"properties",
  //         {{"id",
  //           {{"type", "string"}, {"description",
  //           "任务编号，如1，1.1，2.1.3等"}}},
  //          {"discription",
  //           {{"type", "string"}, {"description", "任务的具体描述"}}}}},
  //        {"required", {"id", "discription"}}},
  //       [](const nlohmann::json &_args) -> nlohmann::json {
  //         loop.tasks.change_task(_args["id"].get<std::string>(),
  //                                  _args["discription"].get<std::string>());
  //         return "finish";
  //       });

  //   loop.register_tool(
  //       "delete_task", "删除任务，可删除任意任务节点，比如1，1.2等",
  //       {{"properties",
  //         {{"id",
  //           {{"type", "string"},
  //            {"description", "任务编号，如1，1.1，2.1.3等"}}}}},
  //        {"required", {"id"}}},
  //       [](const nlohmann::json &_args) -> nlohmann::json {
  //         loop.tasks.delete_node(_args["id"].get<std::string>());
  //         return "finish";
  //       });

  //   loop.register_tool("set_fin", "将任务设置为finish",
  //                        {{"properties",
  //                          {{"id",
  //                            {{"type", "string"},
  //                             {"description",
  //                             "任务编号，如1，1.1，2.1.3等"}}}}},
  //                         {"required", {"id"}}},
  //                        [](const nlohmann::json &_args) -> nlohmann::json {
  //                          loop.tasks.set_fin(_args["id"].get<std::string>());
  //                          return "finish";
  //                        });

  //   loop.register_tool("set_unfin", "将任务设置为unfinish",
  //                        {{"properties",
  //                          {{"id",
  //                            {{"type", "string"},
  //                             {"description",
  //                             "任务编号，如1，1.1，2.1.3等"}}}}},
  //                         {"required", {"id"}}},
  //                        [](const nlohmann::json &_args) -> nlohmann::json {
  //                          loop.tasks.set_unfin(_args["id"].get<std::string>());
  //                          return "finish";
  //                        });

  //   loop.register_tool(
  //       "set_main_task", "设置完整，详细的主任务",
  //       {{"properties",
  //         {{"discription",
  //           {{"type", "string"}, {"description", "任务的具体描述"}}}}},
  //        {"required", {"id", "discription"}}},
  //       [](const nlohmann::json &_args) -> nlohmann::json {
  //         loop.tasks.set_main_task(_args["discription"].get<std::string>());
  //         return "finish";
  //       });

  //   loop.register_tool("get_tasks_json", "获取完整任务描述的json格式", {},
  //                        [](const nlohmann::json &_args) -> nlohmann::json {
  //                          return loop.tasks.tasks_json();
  //                        });

  //   loop.register_tool(
  //       "load_from_json", "将json格式的任务树加载到运行时",
  //       {{"properties",
  //         {{"json_str",
  //           {{"type", "string"}, {"description",
  //           "json格式的任务树字符串"}}}}},
  //        {"required", {"json_str"}}},
  //       [](const nlohmann::json &_args) -> nlohmann::json {
  //         return loop.tasks.load_from_json(
  //             _args["json_str"].get<std::string>());
  //       });

  loop.register_tool(
      "build_global_json", "构建一个全局json",
      {{"properties",
        {{"key", {{"type", "string"}, {"description", "值"}}},
         {"path",
          {{"type", "string"},
           {"description", "路径,以逗号分隔,比如传入src,a.cpp,type,"
                           "结果类似j[src][a.cpp][type]=key"}}}}},
       {"required", {"key", "path"}}},
      [](const nlohmann::json &_args) -> nlohmann::json {
        g_j.set(_args["key"].get<std::string>(),
                _args["path"].get<std::string>().c_str());
        return "finish";
      });

  loop.register_tool(
      "dump_global_json", "将构建完的全局json转为字符串", {},
      [](const nlohmann::json &_args) -> nlohmann::json { return g_j.dump(); });

  loop.register_tool(
      "file_insert_line_json",
      "此函数内部会调用dump_global_json,并将返回的全局json插入文件",
      {{"properties",
        {{"file_name", {{"type", "string"}, {"description", "file path"}}},
         {"line",
          {{"type", "integer"}, {"description", "insert after \"line\""}}}}},
       {"required", {"file_name", "line"}}},
      [](const nlohmann::json &_args) -> nlohmann::json {
        auto data = g_j.dump();
        size_t len = data.size();
        return kw::file_insert_line(
            _args["file_name"].get<std::string>().c_str(),
            _args["line"].get<int>(), data.data(), len);
      });

  loop.register_tool("clear_global_json", "将全局json清空", {},
                     [](const nlohmann::json &_args) -> nlohmann::json {
                       g_j.clear();
                       return "finish";
                     });

  loop.register_tool("exec_cmd", "execute a command,like \"git init\"",
                     {{"properties",
                       {{"cmd",
                         {{"type", "string"},
                          {"description", "a command,like \"git init\""}}}}},
                      {"required", {"cmd"}}},
                     [](const nlohmann::json &_args) -> nlohmann::json {
                       auto result =
                           kw::exec_cmd(_args["cmd"].get<std::string>());
                       if (result.exit_code == -1)
                         return {"fail",
                                 {"stdout", std::move(result.std_out)},
                                 {"stderr", std::move(result.std_err)}};
                       return "success,output=" + result.std_out;
                     });
}

int main() {

  SetCurrentDirectory("D:\\kevin\\dev\\vscodeproject\\downloader");

  spdlog::set_level(spdlog::level::info);
  init();

  //   client.chat_completion("测试tools调用,win10环境下",
  //                          "给你一个任务：用c/"
  //                          "c++"
  //                          "写一个多线程下载器，要非常完善且专业。现在详细分析任"
  //                          "务，进行任务分级，并把任务树的完整json格式写入tasks."
  //                          "md。注意网络库使用libcurl，日志使用spdlog。",
  //                          true);

  //   client.chat_completion("win10环境下",
  //                          "给你一个任务：用c/"
  //                          "c++"
  //                          "写一个多线程下载器，要非常完善且专业。任务分析已完成"
  //                          "，任务内容已写入tasks.md。现在读取tasks.md,"
  //                          "先完成任务1,将需求分析与总体设计写入design.md。",
  //                          true);

  //   client.chat_completion(
  //       "win10环境下",
  //       "给你一个任务：用c/"
  //       "c++写一个多线程下载器，要非常完善且专业。任务内容在tasks.md;"
  //       "完整设计在design.md。现在完成任务2,并创建project-structure.md,"
  //       "将项目结构设计的json格式写入project-"
  //       "structure.md,比如像这样:{\"src\":{\"type\" : "
  //       "\"directory\",\"discription\" : \"...\",\"a.h\": {\"type\" :\"header
  //       " "file\", \"discription\" : \"...\"} \"a.cpp\": {\"type\" :\"source
  //       " "file\", \"discription\" : "
  //       "\"...\"}}},libcurl和spdlog已放在third_party文件夹下,头文件在third_party/"
  //       "curl/include,third_party/spdlog/include,库文件在third_party/curl/"
  //       "lib,third_party/spdlog/lib,在cmake中加入搜索路径后可以这样使用:#include
  //       "
  //       "<spdlog/spdlog.h>,#include "
  //       "<curl/curl.h>,lib里面就是libcurl-x64.dll,spdlog.dll等",
  //       true);

  client.chat_completion(
      "win10环境下",
      "用c/"
      "c++"
      "写一个多线程下载器，要非常完善且专业。使用git进行版本控制,"
      "每完成一个任务要commit,"
      "一定不能出现修改文件后无法回退的情况。任务内容在t"
      "asks.md;需求分析与总体设计在design.md;"
      "文件结构及各个文件的作用在project-structure.md;现在读取tasks.md,"
      "完成任务3",
      true);
}