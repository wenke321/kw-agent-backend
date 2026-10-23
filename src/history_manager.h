#ifndef HISTORY_MANAGER_H
#define HISTORY_MANAGER_H

#include <string>
#include "sqlite3.h"


// "PRAGMA foreign_keys = ON;\
//     CREATE TABLE workspaces ( \
//                 id   INTEGER PRIMARY KEY AUTOINCREMENT,\
//                 name VARCHAR(50),\
//                 path VARCHAR(50));\
//     CREATE TABLE sessions(\
//                           id           INTEGER PRIMARY KEY AUTOINCREMENT,\
//                           name         VARCHAR(50),\
//                           workspace_id INTEGER NOT NULL,\
//                           FOREIGN KEY(workspace_id) REFERENCES workspaces(id) ON DELETE CASCADE);\
//     CREATE TABLE turns(\
//                        id         INTEGER PRIMARY KEY AUTOINCREMENT,\
//                        full_ctx_pre TEXT,\
//                        user_content TEXT,\
//                        full_ctx_end TEXT,\
//                        session_id INTEGER NOT NULL,\
//                        FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE);\
//     CREATE TABLE steps(\
//                        id                INTEGER PRIMARY KEY AUTOINCREMENT,\
//                        turn_id           INTEGER NOT NULL,\
//                        reasoning_content TEXT,\
//                        assistant_out TEXT,\
//                        tool_call_json    TEXT check(json_valid(tool_call_json)),\
//                        FOREIGN KEY(turn_id) REFERENCES turns(id) ON DELETE CASCADE);\
//     CREATE INDEX idx_sessions_workspace_id_id ON sessions(workspace_id, id);";

namespace kw {

//0:compress_err,1:step_error
struct SessionInfo {
  bool     found = false;   // 是否查到记录
  int      case_num = 0;    // case 字段
  std::string ctx;          // ctx 字段
};

struct turn {
  long long   id;
  std::string full_ctx_pre;
  std::string user_content;
  std::string full_ctx_end;
};

class history_manager {
 public:
  // default "kw-agent"
  history_manager(const char* _prefix = "",
                  const char* _db_name = "kw-agent.db");
  ~history_manager();

  std::string get_workspaces_sessions_jsonstr();

  //return -1 if err
  long long create_workspace(const std::string& _name,const std::string& _path);

  bool update_workspace_name(long long _w_id,const std::string& _name);

  bool delete_workspace(long long _w_id);


  //return -1 if err
  long long create_session(long long _w_id, const std::string& _name);

  bool update_session_name(long long _s_id,const std::string& _name);

  // delete session and sub turns and sub steps
  bool delete_session(long long _session_id);

  long long create_turn(long long _session_id, const std::string& _full_ctx_pre,
                          const std::string& _user_content);

  bool store_turn(long long _t_id, const std::string& _full_ctx_end);

  long long get_latest_turn_id(long long _s_id);

  bool get_latest_turn(long long _s_id,turn& _t);

  //
  bool  get_latest_turn_full_ctx_end(long long _s_id,std::string& _out);

  // delete a turn and sub steps
  bool delete_turn(long long _turn_id);

  std::string get_turns_steps_jsonstr(long long _session_id);

  long long create_step(long long _turn_id, const std::string& _reasoning_content,
                          const std::string& _assistant_out,const std::string& _tool_call_json);

  long long count_steps(long long _t_id);

  bool get_latest_session_err(long long _s_id, SessionInfo& out);
  bool create_session_error(long long _s_id,int case_num,const std::string& _ctx);
  bool delete_session_error(long long _s_id);
  bool checkout_session(long long _cur_s_id,long long _new_s_id);

 private:
  sqlite3* db;
};
}  // namespace kw

#endif
