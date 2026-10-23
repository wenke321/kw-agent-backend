#include "session.h"
#include "helpers/function.h"
#include "helpers/log.h"
#include <string>
#include <unistd.h>


void kw::session::set_model_paras(kw::api_parameters_tag _p,
                                  const std::string _v) {
  model_parameters[api_parameters[_p]] = _v;
}

void kw::session::balance_infos_cb(const https_response &_resp) {
  switch (provider) {
  case deepseek: {
    if (_resp.status_code != 200) {
      deal_with_errors(_resp.status_code);
      return;
    }
    nlohmann::json j = _resp.body_json();
    is_available = j["is_available"];
    if (!is_available) {
      LOG_INFO("your balance empty,please charge");
      return;
    }

    // cal
    std::string s = j["balance_infos"]["total_balance"].get<std::string>();
    int size = s.size() - 1, i = size, tmp = 0;
    while (s[i] != '.') {
      tmp += (s[i] - '0') * ::pow(10, size - i);
      --i;
    }
    total_balance += (double)tmp / (double)::pow(10, size - i);
    tmp = 0;
    size = --i;
    while (i != 0) {
      tmp += (s[i] - '0') * ::pow(10, size - i);
      --i;
    }
    total_balance += tmp;
  } break;
  default:
    break;
  }

  LOG_INFO("your total balance:{}\n", total_balance);
}

void kw::session::deal_with_errors(int _status_code) {
  switch (_status_code) {
  case 400: {
    LOG_ERROR("Invalid request body format");
  } break;
  case 401: {
    LOG_ERROR("Authentication fails due to the wrong API key");
  } break;
  case 402: {
    LOG_ERROR("You have run out of balance");
  } break;
  case 422: {
    LOG_ERROR("Your request contains invalid parameters");
  } break;
  case 429: {
    LOG_ERROR("You are sending requests too quickly");
  } break;
  case 501: {
    LOG_ERROR("Deepseek server encounters an issue");
  } break;
  case 503: {
    LOG_ERROR("The server is overloaded due to high traffic");
  } break;
  default:{
    LOG_ERROR("unknown err\n");
  }
    break;
  }
}

kw::session::session()
    :provider(api_provider::deepseek),client(std::make_unique<https_client>()) {

  switch (provider) {
  case deepseek:
    model_parameters["thinking"]["type"] = "enabled";
    model_parameters["max_tokens"] = 256000;
    model_parameters["stream"] = false;
    break;
  default:
    break;
  }

  //注册工具

  //file insert
  m_tools.register_tool("file_insert_line", "将data插入到line的下一行。line == 0	插入到文件开头;"
                         "如果要插入到末尾,将line设成100000000(使用时基本不可能出现的文件行数)或更大,不要超过int。返回\"success\"或错误信息",
                  {{"properties",
                    {{"file_name", {{"type", "string"}, {"description", "file path"}}},
                     {"line",
                      {{"type", "integer"}, {"description", "insert after \"line\""}}},
                     {"data", {{"type", "string"}, {"description", "data to insert"}}}}},
                   {"required", {"file_name", "line", "data"}}},
                  [](const nlohmann::json &_args)->std::string {
                    auto data = _args["data"].get<std::string>();
                    size_t len = data.size();
                    return kw::file_insert_line(
                        _args["file_name"].get<std::string>().c_str(),
                        _args["line"].get<int>(), data.data(), len);
                  });
  m_tools.register_tool("file_read_line", "读取文件行数为[start,end]范围的内容,带行号。出错时返回错误信息",
                         {{"properties",
                           {{"file_name", {{"type", "string"}, {"description", "文件路径，UTF-8 编码"}}},
                            {"start", {{"type", "integer"}, {"description", "起始行号（1-based）"}}},
                            {"end", {{"type", "integer"}, {"description", "<= 0 视为\"使用默认\"；否则必须 >= start"}}}}},
                          {"required", {"file_name", "start", "end"}}},
                         [](const nlohmann::json &_args)->std::string {
                           std::string out;
                           if(!kw::file_read_line(_args["file_name"].get<std::string>().c_str(),
                                                  _args["start"].get<int>(),
                                                  _args["end"].get<int>(),out)){
                             LOG_WARN("file_read_line failed\n");
                           }
                           return out;
                         });
  m_tools.register_tool(
      "file_delete_line", "删除文件行数为[start,end]范围的内容。"
      "如果要删除到文件末尾就将end设成100000000(使用时基本不可能出现的文件行数)或更大,不要超过int。返回\"success\"或错误信息",
      {{"properties",
        {{"file_name", {{"type", "string"}, {"description", "UTF-8 文件路径"}}},
         {"start", {{"type", "integer"}, {"description", "起始行号，1-based"}}},
         {"end", {{"type", "integer"}, {"description", "结束行号"}}}}},
       {"required", {"file_name", "start", "end"}}},
      [](const nlohmann::json &_args) -> std::string {
        return kw::file_delete_line(
            _args["file_name"].get<std::string>().c_str(),
            _args["start"].get<int>(), _args["end"].get<int>());
      });

  m_tools.register_tool("exec_cmd", "运行一个命令,比如ls,mkdir等,根据平台选择命令。返回stdout或stdout+stderr或超时",
                         {{"properties",
                           {{"cmd",
                             {{"type", "string"},
                              {"description", "要运行的命令"}}},
                            {"timeout",
                            {{"type", "integer"},
                             {"description", "超时时间，单位为ms"}}}}},
                          {"required", {"cmd","timeout"}}},
                         [](const nlohmann::json &_args) -> std::string {
                           auto result =
                               kw::exec_cmd(_args["cmd"].get<std::string>(),_args["timeout"].get<int>());
                          if(result.timed_out){
                             return "超时";
                          }
                          if (result.exit_code == -1)
                             return "fail,stdout: "+std::move(result.std_out)+
                                     ";stderr:"+std::move(result.std_err);
                           return "success,stdout=" + result.std_out;
                         });

  m_tools.register_tool("ripgrep","内部调用ripgrep命令，按正则表达式搜索文件内容，返回带行号、按文件分组的匹配结果，默认返回前250条匹配，若超出，则把完整内容写入临时文件，并在返回文本中告知路径",
                        {{"properties",
                          {{"pattern",
                            {{"type", "string"},
                             {"description", "ripgrep正则表达式"}}},
                           {"path",
                            {{"type", "string"},
                             {"description", "相对路径或绝对路径，若为空则在当前工作目录（.）下搜索"}}},
                           {"include",
                            {{"type", "string"},
                             {"description", "文件类型，如\"*.ts\", \"*.{js,jsx}\"，不支持否定（如 !*.min.js）"}}}}},
                         {"required", {"pattern","path","include"}}},
                        [](const nlohmann::json &_args) -> std::string {
                          GrepResult r=kw::grep(_args["pattern"].get<std::string>(),
                                                  _args["path"].get<std::string>(),
                                                  _args["include"].get<std::string>(),
                                                  250);
                          if(r.ok) return r.content;
                          return r.error;
                        });

  // m_tools.register_tool("compress_ctx", "调用后，user将发送压缩上下文的详细要求，照要求做即可。调用此工具时，确保已经完成了阶段性的工作，不要调用其他工具",
  //                       {{"properties",{}},{"required",{}}},
  //                       [&](const nlohmann::json &_args) -> std::string {
  //                         bool s=compress_ctx();
  //                         if(s){
  //                           return cur_ctx[1]["content"].dump(2,' ',false,nlohmann::json::error_handler_t::ignore);
  //                         }
  //                         return "compress ctx fail";
  //                       });
}

kw::session::~session(){}

void kw::session::change_provider(const std::string& _provider,const std::string& _chat_url,const std::string& _key){
  chat_url=_chat_url;
  key=_key;
  if(_provider=="deepseek"){
    provider=api_provider::deepseek;
  }
}

void kw::session::notice_compress_ctx(){
  cur_ctx.push_back({{"role","user"},{"content","尽快完成当前阶段的工作，产出阶段性成果，再过几轮对话就该调用工具进行上下文压缩了"}});
  LOG_INFO("已提醒对方压缩上下文\n");
}

bool kw::session::compress_ctx(){
  {
    LOG_INFO("开始压缩上下文\n");
  }

  nlohmann::json tool_result=cur_ctx[cur_ctx.size()-1];
  nlohmann::json message=cur_ctx[cur_ctx.size()-2];

  cur_ctx.push_back({{"role","user"},{"content",compress_prompt}});
  cur_chat_body["messages"]=cur_ctx;
  https_response _resp=client->post(chat_url,
                                     {{"Content-Type", "application/json"},
                                      {"Accept", "application/json"},
                                      {"Authorization", "Bearer " + key}},
                                     cur_chat_body.dump(2,' ',false,nlohmann::json::error_handler_t::ignore));
  nlohmann::json j=_resp.body_json();

  if(_resp.status_code!=200){
    LOG_ERROR("bad response,see below\n");
    deal_with_errors(_resp.status_code);
    if(_resp.status_code !=400 && _resp.status_code !=422){
      LOG_INFO("出错，再次开始对话时重试\n");
      cur_ctx.erase(cur_ctx.size()-1);
      compress_error_cb(cur_ctx.dump(2,' ',false,nlohmann::json::error_handler_t::ignore));
    }else{
      cur_ctx.erase(cur_ctx.size()-1);
    }
    if (j.find("error") != j.end()) {
      LOG_ERROR("error: {}\n", j.dump(2,' ',false,nlohmann::json::error_handler_t::ignore));
    }

    return false;
  }

  cur_ctx.clear();
  cur_ctx.push_back({{"role","system"},{"content",my_sys_prompt}});
  std::string new_ctx=j["choices"][0]["message"]["content"].get<std::string>();
  cur_ctx.push_back(j["choices"][0]["message"]);
  // {
  //   LOG_DEBUG("压缩返回的上下文: \n{}",new_ctx);
  // }

  ctx_size_cb(j["usage"]["prompt_tokens"].get<int>());
  ctx_compressed=true;

  return true;
}

void kw::session::update_moedl_effort_permission_level(const std::string& _model,const std::string& _effort,int _permission_level){
  model_parameters["model"]=_model;
  model_parameters["reasoning_effort"]=_effort;
  //全局修改,工作区修改,工作区只读:0,1,2
  permission=(_permission_level==0?"全局修改":(_permission_level==1?"工作区修改":"工作区只读"));
  cur_chat_body.update(model_parameters);
}

void kw::session::register_tool(const std::string &_name,
                                const std::string &_description,
                                const nlohmann::json &_parameters,
                                tool_handler _handler) {
  m_tools.register_tool(_name, _description, _parameters, _handler);
}

// sys_content is only once
bool kw::session::start_step(bool use_tools) {
  {
    LOG_INFO("start step\n");
  }

  if(first_turn){
    my_sys_prompt+="\n当前工作目录为"+cur_workspace_path;
#if defined(_WIN32)
    my_sys_prompt+=",当前平台为windows";
#elif defined(__linux__)
    my_sys_prompt+=",当前平台为linux";
#endif
    my_sys_prompt+=",你的权限为"+permission+"\n";
    cur_ctx.push_back({{"role","system"},{"content",my_sys_prompt}});
  }

  if(first_step){
    if (!cur_user_content.empty()) {
      {LOG_DEBUG("cur_user_content: {}\n",cur_user_content);}
      cur_ctx.push_back({{"role", "user"},{"content", cur_user_content}});
    }

    // parameters
    cur_chat_body.update(model_parameters);

  }

  // tools
  if (use_tools) {
    cur_chat_body["tool_choice"] = "auto";
    cur_chat_body["tools"] = m_tools.get_tools_json();
  }

  // messages
  cur_chat_body["messages"] = cur_ctx;

  // handle
  return handle_response(client->post(chat_url,
                               {{"Content-Type", "application/json"},
                                {"Accept", "application/json"},
                                {"Authorization", "Bearer " + key}},
                               cur_chat_body.dump(2,' ',false,nlohmann::json::error_handler_t::ignore)));
}

bool kw::session::handle_response(const https_response &_resp) {

  {
    LOG_INFO("handle response\n");
  }

  nlohmann::json j=_resp.body_json();

  if(_resp.status_code!=200){
    LOG_ERROR("bad response,see below\n");
    deal_with_errors(_resp.status_code);
    if(_resp.status_code==429){
      retry_clock=1000;
      while(1){
        if(retry_clock==11000){
          LOG_INFO("已重试10次，停止重试\n");
          step_fail_cb(cur_ctx);
          break;
        }
        {
          LOG_WARN("服务器繁忙，{}ms后将重试\n",retry_clock);
        }
        https_response resp=client->post(chat_url,
                                           {{"Content-Type", "application/json"},
                                            {"Accept", "application/json"},
                                            {"Authorization", "Bearer " + key}},
                                           cur_chat_body.dump(2,' ',false,nlohmann::json::error_handler_t::ignore));
        if(resp.status_code==200){
          LOG_INFO("重试成功\n");
          j=resp.body_json();
          goto goon;
        }
        if(resp.status_code!=503){
          LOG_ERROR("bad response,see below\n");
          deal_with_errors(_resp.status_code);
          break;
        }
        sleep(retry_clock);
        retry_clock+=1000;
      }
    }
    if(_resp.status_code !=400 && _resp.status_code !=422){
      LOG_INFO("出错，再次开始对话时重试\n");
      step_fail_cb(cur_ctx.dump(2,' ',false,nlohmann::json::error_handler_t::ignore));
    }else{
      LOG_ERROR("发生不可重试的错误，将丢弃这一轮对话\n");
      //cur_ctx rollback
      reset_turn_cb();
    }
    if (j.find("error") != j.end()) {
      LOG_ERROR("error from api: {}\n", j.dump(2,' ',false,nlohmann::json::error_handler_t::ignore));
    }

    return false;
  }

goon:
  std::string r=j["choices"][0]["message"]["reasoning_content"].get<std::string>(),
              a=j["choices"][0]["message"]["content"].get<std::string>();

  nlohmann::json t=nlohmann::json::array();

  ctx_size_cb(j["usage"]["prompt_tokens"].get<int>());

  {
    LOG_INFO("reasoning_content:{}\n",r.c_str());
  }

  cur_ctx.push_back(j["choices"][0]["message"]);

  if (!j["choices"][0]["message"]["tool_calls"].empty()) {
    {
      LOG_INFO("do tool_calls\n");
    }
    auto tool_calls = j["choices"][0]["message"]["tool_calls"];
    for (auto it : tool_calls) {
      // {
      //   LOG_INFO("tool_call:\n");
      //   LOG_INFO("func:{0},args:{1}\n",
      //            it["function"]["name"].get<std::string>(),
      //            it["function"]["arguments"].get<std::string>());
      // }
      nlohmann::json at=nlohmann::json::object({{"name",it["function"]["name"]},
                                                {"args",it["function"]["arguments"]},
                                                {"result",m_tools.invoke(it["function"]["name"],
                                                                          it["function"]["arguments"])}});
      t.push_back(at);

      nlohmann::json tool_result = {
          {"role", "tool"},
          {"tool_call_id", it["id"]},
          {"content", at["result"]}};

      cur_ctx.push_back(tool_result);
    }

    LOG_INFO("tool_calls over\n");
    if(step_success_cb(r,a,t.dump(2,' ',false,nlohmann::json::error_handler_t::ignore))){
      LOG_INFO("handle response success\n");
      first_step=false;
      return true;
    }else{
      LOG_INFO("backend err,handle response failed\n");
      return false;
    }
  }else{
    first_step=true;
    if(step_success_cb(r,a,t.dump(2,' ',false,nlohmann::json::error_handler_t::ignore))){
      if(turn_end_cb()){
        LOG_INFO("handle response success\n");
        first_turn=false;
      }else{
        LOG_ERROR("finish turn failed\n");
      }
    }else{
      LOG_INFO("store step failed\n");
      step_fail_cb(cur_ctx.dump(2,' ',false,nlohmann::json::error_handler_t::ignore));
    }
    return false;
  }
}















