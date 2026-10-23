#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "history_manager.h"
#include "session.h"

#define signal(...) std::function<void(__VA_ARGS__)>

namespace kw {

class agent_loop {
 public:

  signal(long long _id,const std::string& _user_content) add_turn_to_qml;
  signal(long long _id,const std::string& _reasoning_content,const std::string& _assistant_out,const std::string& _tool_call_json) add_step_to_qml;

  agent_loop();
  ~agent_loop();

  // void create_tasks(const std::string sys_content, std::string usr_content);

  void chat_completion(const std::string& sys_content = "",
                       std::string usr_content = "", bool use_tools = false);

  void handle_response(const https_response& _resp);
  void loop(int *_continue);
  void loop();

  void change_provider(const std::string& _provider,const std::string& _chat_url,const std::string& _key,int _max_context);

  void set_cur_user_content(const std::string& _user_content);

  void update_moedl_effort_permission_level(const std::string& _model,const std::string& _effort,int _permission_level);

  void set_before_turn(const std::string& _model,const std::string& _effort,int _permission_level,const std::string& _user_content);

  // for init
  bool load_workspaces_sessions();
  // when clicked a different session
  std::string load_session_history(long long _id);
  std::string get_workspaces_sessions_jsonstr();

  // when clicked a specific session
  std::string change_session(long long _id,const std::string& _path);
  //void change_workspace(long long _id);

  // when clicked add_workspace,add_session,
  long long add_workspace(const std::string& _folder_path,const std::string& _name);
  long long add_session(long long _w_idx,long long _w_id,const std::string& _s_name);
  long long add_turn(long long _s_id, std::string& _full_ctx_pre,
                     std::string& _user_content);
  long long add_step(long long _t_id, std::string& _reasoning_content,
                     std::string& _tool_call_json);

  //
  bool change_session_name(long long _w_idx,long long _s_idx,long long _s_id,const std::string& _new_name);
  bool change_workspace_name(long long _w_idx,long long _w_id,const std::string& _new_name);


  //  cascade delete
  bool delete_workspace(long long _w_idx,long long _w_id);
  bool delete_session(long long _w_idx,long long _s_idx,long long _s_id);
  bool delete_turn(long long _t_id);


  bool finish_turn(long long _s_id, std::string& _full_ctx_end,
                          std::string& _assistant_out);

  std::string get_turns_steps_jsonstr(long long _s_id);
  std::string  get_latest_turn_full_ctx_end(long long _s_id);

  bool checkout_session(long long _cur_s_id,long long _new_s_id);

  long long cur_session_id;
  int half_max_ctx;
  int my_max_ctx;
  int cur_ctx_size=0;
  bool dont_set_user_content=false;
  bool start;
  long long cur_turn_id;

 private:
  //{
  //    "name":{
  //                "type":"directory/file",
  //                "discription":"",
  //                "name":{...}
  //           },
  //    "name":{...}
  //}

  int change_provider_mutex;
  int set_before_turn_mutex;

  kw::history_manager history;
  kw::session cur_session;
  nlohmann::json workspaces_sessions;
  std::string cur_user_content;
  bool running;
};
}  // namespace kw

#endif