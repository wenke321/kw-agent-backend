#include "agent_loop.h"

#include "helpers/builtins.h"
#include "helpers/log.h"
#include "json.hpp"

kw::agent_loop::agent_loop() : history(), cur_session(), running(false) {
  workspaces_sessions = nlohmann::json::array();

  if (!load_workspaces_sessions()) {
    LOG_DEBUG("load_workspaces_sessions failed\n");
  }

  cur_session.step_success_cb =
      [&](const std::string& _reasoning_content,
          const std::string& _assistant_out,
          const std::string& _tool_call_json) -> bool {
    long long ret = history.create_step(cur_turn_id, _reasoning_content,
                                        _assistant_out, _tool_call_json);
    if (ret > -1) {
      LOG_INFO("create_step success\n");
      add_step_to_qml(ret, _reasoning_content, _assistant_out, _tool_call_json);

      if (cur_ctx_size > my_max_ctx) {
        {
          LOG_INFO("模型上下文已使用67.5%,将强制要求压缩\n");
        }
        if (cur_session.compress_ctx()) {
          LOG_INFO("上下文压缩成功\n");
        } else {
          LOG_ERROR("上下文压缩失败\n");
        }
      }
      return true;
    } else {
      LOG_ERROR("create_step failed\n");
      return false;
    }
  };

  cur_session.step_fail_cb = [&](const std::string& _ctx) {
    if (history.create_session_error(cur_session_id, 1, _ctx)) {
      LOG_INFO("备份错误成功，下次对话时处理\n");
    } else {
      LOG_WARN("备份错误失败，将丢弃这一轮对话\n");
      if (history.delete_turn(cur_turn_id)) {
        LOG_WARN("丢弃对话成功\n");
      } else {
        LOG_WARN("丢弃对话失败，用户自己看看要怎么办\n");
      }
    }
  };

  cur_session.ctx_size_cb = [&](int cur_size) { cur_ctx_size = cur_size; };
  cur_session.compress_error_cb = [&](const std::string& _ctx) {
    if (history.create_session_error(cur_session_id, 0, _ctx)) {
      LOG_INFO("备份错误成功，下次对话时处理\n");
    } else {
      LOG_WARN("备份错误失败，将丢弃这一轮对话\n");
      if (history.delete_turn(cur_turn_id)) {
        LOG_WARN("丢弃对话成功\n");
      } else {
        LOG_WARN("丢弃对话失败，用户自己看看要怎么办\n");
      }
    }
  };

  cur_session.turn_end_cb = [&]() -> bool {
    start = true;
    if (history.store_turn(
            cur_turn_id,
            cur_session.cur_ctx.dump(
                2, ' ', false, nlohmann::json::error_handler_t::ignore))) {
      LOG_INFO("此轮对话成功结束\n");
      return true;
    } else {
      LOG_ERROR("此轮对话已完成，但存储失败\n");
      return false;
    }
  };

  cur_session.reset_turn_cb = [&] {
    if (history.delete_turn(cur_turn_id)) {
      LOG_WARN("丢弃对话成功\n");
    } else {
      LOG_WARN("丢弃对话失败，用户自己看看要怎么办\n");
    }
  };
  // model_parameters["stream_options"] = false;
  // model_parameters["temperature"] = 0;
  // model_parameters["top_p"] = 1;
  //  model_parameters["tools"] = m_tools.get_tools_json();
  //  model_parameters["tool_choice"] = "none";
  // model_parameters["logprobs"] = false;
  // model_parameters["top_logprobs"] = 0;
}

kw::agent_loop::~agent_loop() {}

void kw::agent_loop::change_provider(const std::string& _provider,
                                     const std::string& _chat_url,
                                     const std::string& _key,
                                     int _max_context) {
  int old = _load_acquire(_addressof(change_provider_mutex));

  if (old == 1) {
    LOG_ERROR("should not happen\n");
  } else {
    while (!_CAS_weak_release(_addressof(change_provider_mutex), &old, 1)) {
      //
      old = 0;
    }
  }

  cur_session.change_provider(_provider, _chat_url, _key);
  half_max_ctx = _max_context * 4 / 10;
  my_max_ctx = _max_context * 7 / 10 - 50000;

  old = 1;
  while (!_CAS_weak_release(_addressof(change_provider_mutex), &old, 0)) {
    //
    old = 1;
  }

  LOG_INFO("当前provider:{},chat_url:{},half_max_ctx:{},my_max_ctx:{}\n",
           _provider, _chat_url, half_max_ctx, my_max_ctx);
}

void kw::agent_loop::update_moedl_effort_permission_level(
    const std::string& _model, const std::string& _effort,
    int _permission_level) {
  cur_session.update_moedl_effort_permission_level(_model, _effort,
                                                   _permission_level);
}

void kw::agent_loop::set_cur_user_content(const std::string& _user_content) {
  if (!dont_set_user_content) {
    cur_session.cur_user_content = _user_content;
  } else {
    dont_set_user_content = false;
  }
}

void kw::agent_loop::set_before_turn(const std::string& _model,
                                     const std::string& _effort,
                                     int _permission_level,
                                     const std::string& _user_content) {
  int old = _load_acquire(_addressof(set_before_turn_mutex));

  if (old == 1) {
    LOG_ERROR("should not happen\n");
  } else {
    while (!_CAS_weak_release(_addressof(set_before_turn_mutex), &old, 1)) {
      //
      old = 0;
    }
  }

  cur_session.update_moedl_effort_permission_level(_model, _effort,
                                                   _permission_level);
  {
    LOG_INFO("此轮对话的model: {0},effort: {1},permission_level: {2}", _model,
             _effort, cur_session.permission);
  }
  if (!dont_set_user_content) {
    cur_session.cur_user_content = _user_content;
  } else {
    dont_set_user_content = false;
  }

  old = 1;
  while (!_CAS_weak_release(_addressof(set_before_turn_mutex), &old, 0)) {
    //
    old = 1;
  }
}

void kw::agent_loop::loop(int* _continue) {
  {
    LOG_INFO("turn start\n");
  }

  {
    LOG_INFO("检查上一轮对话的错误\n");
  }
  SessionInfo pre;
  //::memset((void*)&pre, 0, sizeof(pre));
  if (history.get_latest_session_err(cur_session_id, pre)) {
    if (pre.found) {
      if (!history.delete_session_error(cur_session_id)) {
        LOG_ERROR("已拿到latest err，但删除失败\n");
      }
      {
        LOG_INFO("上一轮因正常错误而中断，先处理\n");
      }
      cur_turn_id = history.get_latest_turn_id(cur_session_id);
      if (cur_turn_id == -1) {
        LOG_ERROR("此session根本没有turn，sqlite3出错或代码逻辑错误\n");
        start = true;
      } else {
        start = false;
        cur_ctx_size = pre.ctx.size();
        cur_session.first_turn = false;
        cur_session.first_step = false;
        {
          LOG_DEBUG("上一轮最后一步的上下文: {}\n", pre.ctx);
        }
        cur_session.cur_ctx = nlohmann::json::parse(pre.ctx);
      }
    } else {
      LOG_INFO("上一轮对话无错误\n");
      // start=true;
    }
  } else {
    LOG_ERROR("无法判断上一轮对话是否因服务端繁忙而中断，开启新一轮\n");
    start = true;
  }

  if (start) {
    {
      LOG_INFO("新一轮对话\n");
    }
    cur_turn_id = history.create_turn(
        cur_session_id, cur_session.cur_user_content,
        cur_session.cur_ctx.dump(2, ' ', false,
                                 nlohmann::json::error_handler_t::ignore));
    if (cur_turn_id < 0) {
      LOG_ERROR("create_turn failed,see sqlite3 errs\n");
      *_continue = 0;
      return;
    } else {
      {
        LOG_INFO("新turn创建成功，id:{}\n", cur_turn_id);
      }
      start = false;
      // signal to qml
      add_turn_to_qml(cur_turn_id, cur_session.cur_user_content);
    }
  } else {
  }

  *_continue = cur_session.start_step(true);
}

void kw::agent_loop::loop() {
  int old = _load_acquire(_addressof(set_before_turn_mutex));
  if (old == 1) {
    LOG_ERROR("should not happen\n");
  }

  running = true;
  while (running) {
    {
      LOG_INFO("turn start\n");
    }

    {
      LOG_INFO("检查上一轮对话的错误\n");
    }
    SessionInfo pre;
    ::memset((void*)&pre, 0, sizeof(pre));
    if (history.get_latest_session_err(cur_session_id, pre)) {
      if (pre.found) {
        if (!history.delete_session_error(cur_session_id)) {
          LOG_ERROR("已拿到latest err，但删除失败\n");
        }
        {
          LOG_INFO("上一轮因正常错误而中断，先处理\n");
        }
        cur_turn_id = history.get_latest_turn_id(cur_session_id);
        if (cur_turn_id == -1) {
          LOG_ERROR("此session根本没有turn，sqlite3出错或代码逻辑错误\n");
          start = true;
        } else {
          start = false;
          cur_ctx_size = pre.ctx.size();
          cur_session.first_turn = false;
          cur_session.first_step = false;
          {
            LOG_DEBUG("上一轮最后一步的上下文: {}\n", pre.ctx);
          }
          cur_session.cur_ctx = nlohmann::json::parse(pre.ctx);
        }
      } else {
        LOG_INFO("上一轮对话无错误\n");
        // start=true;
      }
    } else {
      LOG_ERROR("无法判断上一轮对话是否因服务端繁忙而中断，开启新一轮\n");
      start = true;
    }

    if (start) {
      {
        LOG_INFO("新一轮对话\n");
      }
      cur_turn_id =
          history.create_turn(cur_session_id, cur_session.cur_user_content,
                              cur_session.cur_ctx.dump());
      if (cur_turn_id < 0) {
        LOG_ERROR("create_turn failed,see sqlite3 errs\n");
        running = false;
        return;
      } else {
        {
          LOG_INFO("新turn创建成功，id:{}\n", cur_turn_id);
        }
        start = false;
        // signal to qml
        add_turn_to_qml(cur_turn_id, cur_session.cur_user_content);
      }
    } else {
    }

    running = cur_session.start_step(true);
  }
}

// for init
bool kw::agent_loop::load_workspaces_sessions() {
  std::string jsonstr = history.get_workspaces_sessions_jsonstr();
  if (!nlohmann::json::accept(jsonstr)) {
    LOG_ERROR("should not happen\n");
    return false;
  }
  workspaces_sessions = nlohmann::json::parse(jsonstr);
  return true;
}

std::string kw::agent_loop::get_workspaces_sessions_jsonstr() {
  {
    LOG_INFO("get_workspaces_sessions_jsonstr\n");
  }
  std::string ret = history.get_workspaces_sessions_jsonstr();

  if (ret.empty()) {
    LOG_WARN("no workspace now\n");
    return ret;
  }
  if (!nlohmann::json::accept(ret)) {
    LOG_ERROR(
        "get_workspaces_sessions_jsonstr return not a json,see sqlite3 errs\n");
    return "";
  }
  return ret;
}

// when clicked a specific session
std::string kw::agent_loop::change_session(long long _s_id,
                                           const std::string& _path) {
  {
    LOG_INFO("change_session\n");
  }

  dont_set_user_content = false;

  cur_session_id = _s_id;
  cur_session.cur_workspace_path = _path;
  kw::turn t;
  //::memset((void*)&t, 0, sizeof(kw::turn));
  bool rc = history.get_latest_turn(cur_session_id, t);
  if (!rc) {
    LOG_ERROR("see sqlite3 errs\n");
  }
  cur_session.first_turn = t.full_ctx_pre.empty();
  cur_session.first_step = start =
      !t.full_ctx_end.empty() || t.full_ctx_pre.empty();
  cur_turn_id = t.id;

  if (t.id > -1 && !cur_session.first_step) {
    int step_cnt = static_cast<int>(history.count_steps(t.id));
    if (step_cnt < 0) {
      LOG_ERROR("see sqlite3 errs\n");
    } else {
      cur_session.first_step = true;
      dont_set_user_content = true;
      cur_session.cur_user_content = t.user_content;
      // cur_ctx_size=t.full_ctx_pre.size()+t.user_content.size();
      if (nlohmann::json::accept(t.full_ctx_pre)) {
        cur_session.cur_ctx = nlohmann::json::parse(t.full_ctx_pre);
      }
    }
  }

  if (start) {
    if (!t.full_ctx_end.empty()) {
      if (!nlohmann::json::accept(t.full_ctx_end)) {
        LOG_ERROR("上轮对话上下文损坏，将使用上轮对话的初始上下文\n");
        if (!nlohmann::json::accept(t.full_ctx_pre)) {
          LOG_ERROR("上轮对话初始上下文损坏，将丢弃上轮对话\n");
          if (history.delete_turn(t.id)) {
            LOG_INFO("丢弃上轮对话成功\n");
          }
        } else {
          cur_session.cur_ctx = nlohmann::json::parse(t.full_ctx_pre);
          cur_ctx_size =
              cur_session.cur_ctx
                  .dump(2, ' ', false, nlohmann::json::error_handler_t::ignore)
                  .size();
        }
      } else {
        cur_session.cur_ctx = nlohmann::json::parse(t.full_ctx_end);
        cur_ctx_size =
            cur_session.cur_ctx
                .dump(2, ' ', false, nlohmann::json::error_handler_t::ignore)
                .size();
      }
    } else {
      cur_ctx_size = 0;
      cur_session.cur_ctx.clear();
    }
  } else {
    if (!t.full_ctx_pre.empty() && t.full_ctx_end.empty()) {
      LOG_WARN("last turn unfinish\n");
    }
  }

  {
    LOG_INFO("get_turns_steps_jsonstr\n");
  }
  std::string ret = history.get_turns_steps_jsonstr(_s_id);
  if (ret.empty()) {
    LOG_ERROR("get_turns_steps_jsonstr failed,see sqlite3 errs\n");
  }
  return ret;
}

// when clicked rename button
bool kw::agent_loop::change_session_name(long long _w_idx, long long _s_idx,
                                         long long _s_id,
                                         const std::string& _new_name) {
  {
    LOG_INFO("change_session_name\n");
  }
  bool ret = history.update_session_name(_s_id, _new_name);

  if (ret) {
    // workspaces_sessions[_w_idx]["sessions"][_s_idx]["name"]=_new_name;
  } else {
    LOG_ERROR("change_session_name failed,see sqlite3 errs\n");
  }

  return ret;
}

bool kw::agent_loop::change_workspace_name(long long _w_idx, long long _w_id,
                                           const std::string& _new_name) {
  {
    LOG_INFO("change_workspace_name\n");
  }
  bool ret = history.update_workspace_name(_w_id, _new_name);

  if (ret) {
    // workspaces_sessions[_w_idx]["name"]=_new_name;
  } else {
    LOG_ERROR("change_workspace_name failed,see sqlite3 errs\n");
  }

  return ret;
}

//
long long kw::agent_loop::add_workspace(const std::string& _folder_path,
                                        const std::string& _name) {
  {
    LOG_INFO("add_workspace\n");
  }
  long long w_id = history.create_workspace(_name, _folder_path);

  if (w_id > -1) {
    // workspaces_sessions.push_back({{"id",w_id},{"name",_name},{"path",_folder_path},{"sessions",nlohmann::json::array()}});
    return w_id;
  } else {
    LOG_ERROR("add_workspace failed,see sqlite3 errs\n");
  }

  return -1;
}

bool kw::agent_loop::delete_workspace(long long _w_idx, long long _w_id) {
  {
    LOG_INFO("delete_workspace\n");
  }
  bool ret = history.delete_workspace(_w_id);

  if (ret) {
    // workspaces_sessions.erase(_w_idx);
  } else {
    LOG_ERROR("delete_workspace failed,see sqlite3 errs\n");
  }

  return ret;
}

long long kw::agent_loop::add_session(long long _w_idx, long long _w_id,
                                      const std::string& _session_name) {
  {
    LOG_INFO("add_session\n");
  }
  long long ret = history.create_session(_w_id, _session_name);

  if (ret > -1) {
    // workspaces_sessions[_w_idx]["sessions"].push_back({{"id",ret},{"name",_session_name}});
  } else {
    LOG_ERROR("add_session failed,see sqlite3 errs\n");
  }

  return ret;
}

// delete session and sub turns and sub steps
bool kw::agent_loop::delete_session(long long _w_idx, long long _s_idx,
                                    long long _s_id) {
  {
    LOG_INFO("delete_session\n");
  }
  bool ret = history.delete_session(_s_id);

  if (ret) {
    // workspaces_sessions[_w_idx]["sessions"].erase(_s_idx);
  } else {
    LOG_ERROR("delete_session failed,see sqlite3 errs\n");
  }

  return ret;
}

std::string kw::agent_loop::get_turns_steps_jsonstr(long long _s_id) {
  return history.get_turns_steps_jsonstr(_s_id);
}

bool kw::agent_loop::checkout_session(long long _cur_s_id,
                                      long long _new_s_id) {
  return history.checkout_session(_cur_s_id, _new_s_id);
}
