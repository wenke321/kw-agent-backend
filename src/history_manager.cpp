#include "history_manager.h"

#include <string.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "helpers/log.h"
#include "sqlite3.h"

namespace fs = std::filesystem;

static const char* kCreateTablesSql = R"sql(
CREATE TABLE IF NOT EXISTS workspaces (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    name VARCHAR(50),
    path VARCHAR(50)
);
CREATE TABLE IF NOT EXISTS sessions (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         VARCHAR(50),
    workspace_id INTEGER NOT NULL,
    FOREIGN KEY(workspace_id) REFERENCES workspaces(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS turns (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    full_ctx_pre TEXT,
    user_content TEXT,
    full_ctx_end TEXT,
    session_id   INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS steps (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    turn_id           INTEGER NOT NULL,
    reasoning_content TEXT,
    assistant_out     TEXT,
    tool_call_json    TEXT CHECK(json_valid(tool_call_json)),
    FOREIGN KEY(turn_id) REFERENCES turns(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS session_errors (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    ctx               TEXT    NOT NULL,
    case_num          INTEGER NOT NULL,
    session_id        INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_sessions_workspace_id_id
ON sessions(workspace_id, id);
)sql";

struct stmt_guard {
  sqlite3_stmt* stmt = nullptr;
  ~stmt_guard() {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
  }
};

static int exec_sql(sqlite3* _db, const char* _sql) {
  char* errmsg = nullptr;
  int rc = sqlite3_exec(_db, _sql, NULL, NULL, &errmsg);

  if (rc != SQLITE_OK) {
    LOG_ERROR("sqlite3 SQL error: rc={0}, {1}\n", rc,
              errmsg ? errmsg : sqlite3_errmsg(_db));
    sqlite3_free(errmsg);
    return rc;
  }

  sqlite3_free(errmsg);
  return SQLITE_OK;
}

static int do_transaction(sqlite3* _db, const char* _sql) {
  int rc = exec_sql(_db, "BEGIN IMMEDIATE TRANSACTION;");
  if (rc != SQLITE_OK) return rc;

  rc = exec_sql(_db, _sql);
  if (rc != SQLITE_OK) {
    int rb = exec_sql(_db, "ROLLBACK;");
    if (rb != SQLITE_OK) {
      LOG_ERROR("sqlite3 ROLLBACK failed: {}", sqlite3_errmsg(_db));
    }
    return rc;
  }

  rc = exec_sql(_db, "COMMIT;");
  if (rc != SQLITE_OK) {
    if (!sqlite3_get_autocommit(_db)) {
      int rb = exec_sql(_db, "ROLLBACK;");
      if (rb != SQLITE_OK) {
        LOG_ERROR("sqlite3 ROLLBACK failed: {}", sqlite3_errmsg(_db));
      }
    }
    return rc;
  }

  return SQLITE_OK;
}

// 返回 true 表示完整性检查通过
static bool integrity_check(sqlite3* db) {
  sqlite3_stmt* stmt = nullptr;
  int rc =
      sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare integrity_check failed: {}", sqlite3_errmsg(db));
    return false;
  }

  std::string errs;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(stmt, 0);
    if (text) {
      if (!errs.empty()) errs += "\n";
      errs += reinterpret_cast<const char*>(text);
    }
  }

  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 integrity_check step failed: {}", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    return false;
  }

  sqlite3_finalize(stmt);

  if (errs == "ok") {
    return true;
  }

  LOG_ERROR("sqlite3 integrity_check failed: {}", errs);
  return false;
}

kw::history_manager::history_manager(const char* _prefix, const char* _db_name)
    : db(nullptr) {
  // create or open sqlite db
  if (!_db_name) {
    throw std::invalid_argument("sqlite3 prefix or db_name is null");
  }

  // 原逻辑是 a += _prefix; a += _db_name; 没有加分隔符。
  // 如果 prefix 是目录，建议用 fs::path(prefix) / db_name。
  std::string a = std::string(_prefix) + _db_name;
  fs::path db_path = a;

  bool need_create = !fs::exists(db_path);

  if (!need_create) {
    // 以读写方式打开，避免完整性检查通过后 db 仍是只读
    int rc = sqlite3_open_v2(db_path.string().c_str(), &db,
                             SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
      LOG_ERROR("sqlite3 open db failed: {}, will rename and recreate",
                sqlite3_errmsg(db));
      if (db) {
        sqlite3_close(db);
        db = nullptr;
      }
      need_create = true;
    } else {
      if (integrity_check(db)) {
        LOG_INFO("sqlite3 db integrity check ok");

        // 外键必须每个连接设置，且不能在事务内
        if (exec_sql(db, "PRAGMA foreign_keys = ON;") != SQLITE_OK) {
          LOG_ERROR("sqlite3 enable foreign_keys failed: {}",
                    sqlite3_errmsg(db));
          sqlite3_close(db);
          db = nullptr;
          throw std::runtime_error("sqlite3 enable foreign_keys failed");
        }
        return;
      }

      LOG_ERROR("sqlite3 db integrity check failed, will rename and recreate");
      sqlite3_close(db);
      db = nullptr;
      need_create = true;
    }
  }

  if (need_create && fs::exists(db_path)) {
    fs::path old_path = db_path;
    old_path += ".old";
    std::error_code ec;
    fs::rename(db_path, old_path, ec);
    if (ec) {
      LOG_ERROR("sqlite3 rename db failed: {}", ec.message());
      throw std::runtime_error("rename db failed");
    }
  }

  // 创建或打开新库
  int rc = sqlite3_open_v2(db_path.string().c_str(), &db,
                           SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("sqlite3 create/open db failed: {}", sqlite3_errmsg(db));
    if (db) {
      sqlite3_close(db);
      db = nullptr;
    }
    throw std::runtime_error("sqlite3 create/open db failed");
  }

  // 事务外设置外键
  if (exec_sql(db, "PRAGMA foreign_keys = ON;") != SQLITE_OK) {
    LOG_ERROR("sqlite3 enable foreign_keys failed: {}", sqlite3_errmsg(db));
    sqlite3_close(db);
    db = nullptr;
    throw std::runtime_error("enable foreign_keys failed");
  }

  // 建表
  if (do_transaction(db, kCreateTablesSql) != SQLITE_OK) {
    LOG_ERROR("sqlite3 create tables failed: {}", sqlite3_errmsg(db));
    sqlite3_close(db);
    db = nullptr;
    throw std::runtime_error("sqlite3 create tables failed");
  }
}

kw::history_manager::~history_manager() { sqlite3_close_v2(db); }

// return -1 if err
long long kw::history_manager::create_workspace(const std::string& _name,
                                                const std::string& _path) {
  const char* sql = "INSERT INTO workspaces (name, path) VALUES (?, ?);";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_text(sg.stmt, 1, _name.c_str(), -1, SQLITE_TRANSIENT) !=
          SQLITE_OK ||
      sqlite3_bind_text(sg.stmt, 2, _path.c_str(), -1, SQLITE_TRANSIENT) !=
          SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 insert failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  return sqlite3_last_insert_rowid(db);
}

bool kw::history_manager::update_workspace_name(long long _w_id,
                                                const std::string& _name) {
  const char* sql = "UPDATE workspaces SET name = ? WHERE id=?;";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_text(sg.stmt, 1, _name.c_str(), -1, SQLITE_TRANSIENT) !=
          SQLITE_OK ||
      sqlite3_bind_int64(sg.stmt, 2, static_cast<sqlite3_int64>(_w_id)) !=
          SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 update_workspace_name failed,errmsg:{}",
              sqlite3_errmsg(db));
    return false;
  }

  return true;
}

//
bool kw::history_manager::delete_workspace(long long _w_id) {
  const char* sql = "DELETE FROM workspaces WHERE id=?;";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_w_id)) !=
      SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 delete_workspace failed,errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  return true;
}

long long kw::history_manager::create_session(long long _w_id,
                                              const std::string& _name) {
  const char* sql = "INSERT INTO sessions (workspace_id,name) VALUES(?,?);";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_w_id)) !=
          SQLITE_OK ||
      sqlite3_bind_text(sg.stmt, 2, _name.c_str(), -1, SQLITE_TRANSIENT) !=
          SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 create_session failed,errmsg:{},workspace_id:{}",
              sqlite3_errmsg(db), _w_id);
    return -1;
  }

  return sqlite3_last_insert_rowid(db);
}

bool kw::history_manager::update_session_name(long long _s_id,
                                              const std::string& _name) {
  const char* sql = "UPDATE sessions SET name=? WHERE id=?;";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_text(sg.stmt, 1, _name.c_str(), -1, SQLITE_TRANSIENT) !=
          SQLITE_OK ||
      sqlite3_bind_int64(sg.stmt, 2, static_cast<sqlite3_int64>(_s_id)) !=
          SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 update_session_name failed,errmsg:{}",
              sqlite3_errmsg(db));
    return false;
  }

  return true;
}

std::string kw::history_manager::get_workspaces_sessions_jsonstr() {
  if (!db) {
    LOG_ERROR("db is null in {0}:{1}", __FILE__, __LINE__);
    return "[]";
  }

  const char* sql = R"SQL(
SELECT json_group_array(
         json_object(
           'id', w.id,
           'name', w.name,
           'path',w.path,
           'sessions', (
             SELECT json_group_array(
                      json_object('id', s.id, 'name', s.name)
                    )
             FROM (
               SELECT id, name
               FROM sessions
               WHERE workspace_id = w.id
               ORDER BY id
             ) AS s
           )
         )
       )
FROM (
  SELECT id, name,path
  FROM workspaces
  ORDER BY id
) AS w;
)SQL";

  stmt_guard sg;
  int rc = sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("sqlite3_prepare_v2 failed in {0}:{1}, err: {2}", __FILE__,
              __LINE__, sqlite3_errmsg(db));
    return "[]";
  }

  rc = sqlite3_step(sg.stmt);

  if (rc == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(sg.stmt, 0);
    if (!text) {
      LOG_INFO("sqlite3 workspaces is NULL\n");
      return "[]";
    }

    int n = sqlite3_column_bytes(sg.stmt, 0);
    if (n < 0) {
      LOG_INFO("sqlite3 err,should not happen,last errmsg:{}\n",
               sqlite3_errmsg(db));
      return "[]";
    }

    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(n));
  }

  if (rc == SQLITE_DONE) {
    LOG_INFO("sqlite3 err,should not happen,last errmsg:{}\n",
             sqlite3_errmsg(db));
    return "[]";
  }

  LOG_ERROR("sqlite3_step failed in {0}:{1}, err: {2}", __FILE__, __LINE__,
            sqlite3_errmsg(db));
  return "[]";
}

// delete session and sub turns and sub steps
bool kw::history_manager::delete_session(long long _s_id) {
  const char* sql = "DELETE FROM sessions WHERE id=?;";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_s_id)) !=
      SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 delete_session failed,errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  return true;
}

long long kw::history_manager::create_turn(long long _s_id,
                                           const std::string& _user_content,
                                           const std::string& _full_ctx_pre) {
  const char* sql =
      "INSERT INTO turns (full_ctx_pre,user_content,session_id) VALUES(?,?,?);";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_text(sg.stmt, 1, _full_ctx_pre.c_str(), -1,
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(sg.stmt, 2, _user_content.c_str(), -1,
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int64(sg.stmt, 3, static_cast<sqlite3_int64>(_s_id)) !=
          SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}", sqlite3_errmsg(db));
    return -1;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 create_turn failed,errmsg:{},session_id:{}",
              sqlite3_errmsg(db), _s_id);
    return -1;
  }

  return sqlite3_last_insert_rowid(db);
}

bool kw::history_manager::store_turn(long long _t_id,
                                     const std::string& _full_ctx_end) {
  if (_t_id <= 0) {
    LOG_ERROR("sqlite3 store_turn invalid t_id:{}", _t_id);
    return false;
  }

  const char* sql = "UPDATE turns SET full_ctx_end=? WHERE id=?;";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed, errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  if (sqlite3_bind_text(sg.stmt, 1, _full_ctx_end.c_str(), -1,
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int64(sg.stmt, 2, static_cast<sqlite3_int64>(_t_id)) !=
          SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed, errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 store_turn failed, errmsg:{}, t_id:{}",
              sqlite3_errmsg(db), _t_id);
    return false;
  }

  if (sqlite3_changes(db) != 1) {
    LOG_ERROR("sqlite3 store_turn affected {} rows, t_id:{}",
              sqlite3_changes(db), _t_id);
    return false;
  }

  return true;
}

long long kw::history_manager::get_latest_turn_id(long long _s_id) {
  const char* sql =
      "SELECT id FROM turns WHERE session_id=? ORDER BY id DESC LIMIT 1";

  stmt_guard sg;

  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed: {}", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_s_id)) !=
      SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed: {}", sqlite3_errmsg(db));
    return -1;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc == SQLITE_ROW) {
    return sqlite3_column_int64(sg.stmt, 0);
  }

  if (rc == SQLITE_DONE) {
    LOG_INFO("sqlite3 current session has no turn\n");
  } else {
    LOG_ERROR("sqlite3 unknown err,should not happen\n");
  }

  return -1;
}

bool kw::history_manager::get_latest_turn(long long _s_id, turn& _out) {
  const char* sql =
      "SELECT id,full_ctx_pre,user_content,full_ctx_end FROM turns WHERE session_id=? ORDER BY id DESC LIMIT 1;";

  stmt_guard sg;

  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed: {}", sqlite3_errmsg(db));
    return false;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_s_id)) !=
      SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed: {}", sqlite3_errmsg(db));
    return false;
  }

  int rc = sqlite3_step(sg.stmt), n;

  if (rc == SQLITE_ROW) {
    // id
    _out.id = sqlite3_column_int64(sg.stmt, 0);

    // full_ctx_pre
    const unsigned char* text = sqlite3_column_text(sg.stmt, 1);
    if (text == nullptr) {
      LOG_WARN("sqlite3 full_ctx_pre is null\n");
    } else {
      n = sqlite3_column_bytes(sg.stmt, 1);
      if (n < 0) {
        LOG_ERROR("sqlite3 err,should not happen,last errmsg:{}\n",
                  sqlite3_errmsg(db));
        return false;
      }
      _out.full_ctx_pre = {reinterpret_cast<const char*>(text),
                           static_cast<size_t>(n)};
    }

    // user_content
    text = sqlite3_column_text(sg.stmt, 2);
    if (text == nullptr) {
      LOG_WARN("sqlite3 user_content is null\n");
    } else {
      n = sqlite3_column_bytes(sg.stmt, 2);
      if (n < 0) {
        LOG_ERROR("sqlite3 err,should not happen,last errmsg:{}\n",
                  sqlite3_errmsg(db));
        return false;
      }
      _out.user_content = {reinterpret_cast<const char*>(text),
                           static_cast<size_t>(n)};
    }

    // full_ctx_end
    text = sqlite3_column_text(sg.stmt, 3);
    if (text == nullptr) {
      LOG_WARN("sqlite3 full_ctx_end is null\n");
      return true;
    } else {
      n = sqlite3_column_bytes(sg.stmt, 3);
      if (n < 0) {
        LOG_ERROR("sqlite3 err,should not happen,last errmsg:{}\n",
                  sqlite3_errmsg(db));
        return false;
      }
      _out.full_ctx_end = {reinterpret_cast<const char*>(text),
                           static_cast<size_t>(n)};
      return true;
    }
  } else {
    _out.id = -1;
    if (rc == SQLITE_DONE) {
      LOG_WARN("sqlite3 cur session has no turn in db now,id={}\n", _s_id);
      return true;
    }
    LOG_ERROR("sqlite3 SELECT failed\n");
    return false;
  }

  LOG_ERROR("sqlite3 err,should not happen,last errmsg:{}\n",
            sqlite3_errmsg(db));
  return false;
}

bool kw::history_manager::get_latest_turn_full_ctx_end(long long _s_id,
                                                       std::string& _out) {
  if (db == nullptr) {
    LOG_ERROR("db is null\n");
    return false;
  }

  const char* sql =
      "SELECT full_ctx_end FROM turns "
      "WHERE session_id = ? "
      "ORDER BY id DESC LIMIT 1;";

  stmt_guard sg;

  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed: {}", sqlite3_errmsg(db));
    return false;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_s_id)) !=
      SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed: {}", sqlite3_errmsg(db));
    return false;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(sg.stmt, 0);
    // full_ctx_end 可能为 NULL
    if (text == nullptr) {
      LOG_WARN("sqlite3 latest full_ctx_end is NULL\n");
      return true;
    }
    int n = sqlite3_column_bytes(sg.stmt, 0);
    if (n < 0) {
      LOG_ERROR("sqlite3 err,should not happen,last errmsg:{}\n",
                sqlite3_errmsg(db));
      return false;
    }
    _out = {reinterpret_cast<const char*>(text), static_cast<size_t>(n)};
    return true;
  }

  if (rc == SQLITE_DONE) {
    LOG_INFO("sqlite3 current session has no turn\n");
    return true;
  }

  LOG_ERROR("sqlite3 unknown err\n");
  return false;
}

// delete a turn and sub steps
bool kw::history_manager::delete_turn(long long _turn_id) {
  std::string sql =
      "DELETE FROM turns WHERE id=" + std::to_string(_turn_id) + ";";

  if (do_transaction(db, sql.data()) != SQLITE_OK) {
    LOG_ERROR("sqlite3 delete a turn fail\n");
    return false;
  }
  return true;
}

long long kw::history_manager::create_step(
    long long _t_id, const std::string& _reasoning_content,
    const std::string& _assistant_out, const std::string& _tool_call_json) {
  const char* sql =
      "INSERT INTO steps (turn_id,reasoning_content,assistant_out,tool_call_json) VALUES(?,?,?,?)";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}\n", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_t_id)) !=
          SQLITE_OK ||
      sqlite3_bind_text(sg.stmt, 2, _reasoning_content.c_str(), -1,
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(sg.stmt, 3, _assistant_out.c_str(), -1,
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(sg.stmt, 4, _tool_call_json.c_str(), -1,
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}\n", sqlite3_errmsg(db));
    return -1;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 create_step failed,errmsg:{},turn_id:{}",
              sqlite3_errmsg(db), _t_id);
    return -1;
  }

  return sqlite3_last_insert_rowid(db);
}

// 返回: >=0 表示数量; -1 表示出错(调用方需判负)
long long kw::history_manager::count_steps(long long _t_id) {
  if (_t_id <= 0) {
    LOG_ERROR("count_steps invalid t_id:{}", _t_id);
    return -1;
  }

  const char* sql = "SELECT COUNT(*) FROM steps WHERE turn_id=?;";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed: {}", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_t_id)) !=
      SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed: {}", sqlite3_errmsg(db));
    return -1;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
    LOG_WARN("count_steps busy, t_id:{}", _t_id);
    return -1;  // 或按你的重试策略处理
  }
  if (rc != SQLITE_ROW) {
    LOG_ERROR("sqlite3 count_steps failed, errmsg:{}, t_id:{}",
              sqlite3_errmsg(db), _t_id);
    return -1;
  }

  return sqlite3_column_int64(sg.stmt, 0);
}

std::string kw::history_manager::get_turns_steps_jsonstr(long long _s_id) {
  std::string sql =
      "SELECT json_group_array(\
                 json_object(\
                   'id',t.id,\
                   'user_content',t.user_content,\
                   'steps',(SELECT json_group_array(\
                                        json_object(\
                                          'id',s.id,\
                                          'reasoning_content',s.reasoning_content,\
                                          'assistant_out',s.assistant_out,\
                                          'tool_call_json',s.tool_call_json)) \
                            FROM (SELECT id,reasoning_content,assistant_out,tool_call_json \
                                  FROM steps \
                                  WHERE turn_id=t.id \
                                  ORDER BY id) AS s))) \
        FROM (SELECT id,user_content FROM turns WHERE session_id=? ORDER BY id) AS t;";

  stmt_guard sg;

  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &sg.stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("sqlite3_prepare_v2 failed in {0}:{1},err: {2}", __FILE__,
              __LINE__, sqlite3_errmsg(db));
    return "";
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_s_id)) !=
      SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}\n", sqlite3_errmsg(db));
    return "";
  }

  int rc = sqlite3_step(sg.stmt);

  if (rc == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(sg.stmt, 0);

    if (!text) {
      LOG_INFO("sqlite3 err,should not happen,last errmsg:{}\n",
               sqlite3_errmsg(db));
      return "";
    }

    int n = sqlite3_column_bytes(sg.stmt, 0);
    if (n < 0) {
      LOG_ERROR("sqlite3 err,should not happen,last errmsg:{}\n",
                sqlite3_errmsg(db));
      return "";
    }

    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<size_t>(n));
  }

  if (rc == SQLITE_DONE) {
    // something wrong
    LOG_INFO("sqlite3 SELECT return nothing\n");
    return "[]";
  }

  LOG_ERROR("sqlite3 err,should not happen,last errmsg:{}\n",
            sqlite3_errmsg(db));
  return "";
}

bool kw::history_manager::get_latest_session_err(long long _s_id,
                                                 SessionInfo& out) {
  const char* sql =
      "SELECT case_num, ctx "
      "FROM session_errors "
      "WHERE session_id = ? "
      "ORDER BY id "
      "LIMIT 1;";

  stmt_guard sg;
  int rc = sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}\n", sqlite3_errmsg(db));
    return false;
  }

  // 绑定参数
  rc = sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_s_id));
  if (rc != SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}\n", sqlite3_errmsg(db));
    return false;
  }

  // 只取第一行
  rc = sqlite3_step(sg.stmt);
  if (rc == SQLITE_ROW) {
    out.found = true;
    out.case_num = sqlite3_column_int(sg.stmt, 0);

    const unsigned char* text = sqlite3_column_text(sg.stmt, 1);
    if (text) {
      out.ctx.assign(reinterpret_cast<const char*>(text));
    } else {
      out.ctx.clear();  // ctx 为 NULL
    }
    return true;
  }
  if (rc != SQLITE_DONE) {
    LOG_ERROR("sqlite3 get_latest_session_err failed,errmsg:{}",
              sqlite3_errmsg(db));
    return false;
  }

  return true;
}

bool kw::history_manager::create_session_error(long long _s_id, int case_num,
                                               const std::string& _ctx) {
  const char* sql =
      "INSERT INTO session_errors (ctx, case_num, session_id) "
      "VALUES (?1, ?2, ?3);";

  stmt_guard sg;
  int rc = sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed,errmsg:{}\n", sqlite3_errmsg(db));
    return false;
  }

  if (sqlite3_bind_text(sg.stmt, 1, _ctx.c_str(), static_cast<int>(_ctx.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int(sg.stmt, 2, case_num) != SQLITE_OK ||
      sqlite3_bind_int64(sg.stmt, 3, static_cast<sqlite3_int64>(_s_id)) !=
          SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed,errmsg:{}\n", sqlite3_errmsg(db));
    return false;
  }

  // 执行插入
  if (sqlite3_step(sg.stmt) != SQLITE_DONE) {
    LOG_ERROR("sqlite3 create_session_error failed,errmsg:{},session_id:{}",
              sqlite3_errmsg(db), _s_id);
    return false;
  }

  return true;
}

bool kw::history_manager::delete_session_error(long long _s_id) {
  const char* sql = "DELETE FROM session_errors WHERE session_id=?;";

  stmt_guard sg;
  if (sqlite3_prepare_v2(db, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("sqlite3 prepare failed: {}", sqlite3_errmsg(db));
    return false;
  }

  if (sqlite3_bind_int64(sg.stmt, 1, static_cast<sqlite3_int64>(_s_id)) !=
      SQLITE_OK) {
    LOG_ERROR("sqlite3 bind failed: {}", sqlite3_errmsg(db));
    return false;
  }

  int rc = sqlite3_step(sg.stmt);
  if (rc == SQLITE_DONE) {
    return true;
  }

  LOG_ERROR("sqlite3 delete_session_error failed,errmsg:{}",
            sqlite3_errmsg(db));
  return false;
}

bool kw::history_manager::checkout_session(long long _cur_s_id,
                                           long long _new_s_id) {
  // 源会话和目标会话不能相同（否则会无限复制自己，可选限制）
  if (_cur_s_id == _new_s_id) {
    // 允许但通常没意义；这里按业务选择返回 false
    return false;
  }

  sqlite3_stmt* st_turns = NULL;
  sqlite3_stmt* st_steps = NULL;
  sqlite3_stmt* st_ins_turn = NULL;
  sqlite3_stmt* st_ins_step = NULL;
  bool ok = false;

  // 1) 校验源会话与目标会话都存在
  {
    sqlite3_stmt* st_check = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM sessions WHERE id IN (?, ?);",
                           -1, &st_check, NULL) != SQLITE_OK) {
      LOG_ERROR("sqlite3 bind failed,errmsg:{}", sqlite3_errmsg(db));
      return false;
    }
    sqlite3_bind_int64(st_check, 1, _cur_s_id);
    sqlite3_bind_int64(st_check, 2, _new_s_id);
    bool both_exist = false;
    if (sqlite3_step(st_check) == SQLITE_ROW) {
      both_exist = (sqlite3_column_int(st_check, 0) == 2);
    }
    sqlite3_finalize(st_check);
    if (!both_exist) {
      LOG_ERROR("sqlite3 checkout_session failed,errmsg:{}",
                sqlite3_errmsg(db));
      return false;
    }
  }

  // 2) 开启事务，保证复制操作的原子性
  if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
    LOG_ERROR("sqlite3 BEGIN IMMEDIATE failed,errmsg:{}", sqlite3_errmsg(db));
    return false;
  }

  // 3) 准备 SQL
  //    查询源会话下所有 turn
  if (sqlite3_prepare_v2(db,
                         "SELECT id, full_ctx_pre, user_content, full_ctx_end "
                         "FROM turns WHERE session_id = ? ORDER BY id;",
                         -1, &st_turns, NULL) != SQLITE_OK) {
    goto cleanup;
  }

  //    按原 turn_id 查询该 turn 下的所有 step
  if (sqlite3_prepare_v2(
          db,
          "SELECT reasoning_content, assistant_out, tool_call_json "
          "FROM steps WHERE turn_id = ? ORDER BY id;",
          -1, &st_steps, NULL) != SQLITE_OK) {
    goto cleanup;
  }

  //    插入新的 turn 到目标 session
  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO turns (full_ctx_pre, user_content, full_ctx_end, session_id) "
          "VALUES (?, ?, ?, ?);",
          -1, &st_ins_turn, NULL) != SQLITE_OK) {
    goto cleanup;
  }

  //    插入新的 step（turn_id 使用刚插入的 turn 的 id）
  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO steps (turn_id, reasoning_content, assistant_out, tool_call_json) "
          "VALUES (?, ?, ?, ?);",
          -1, &st_ins_step, NULL) != SQLITE_OK) {
    goto cleanup;
  }

  // 4) 遍历每个 turn 并复制
  sqlite3_bind_int64(st_turns, 1, _cur_s_id);

  int rc;
  while ((rc = sqlite3_step(st_turns)) == SQLITE_ROW) {
    long long old_turn_id = sqlite3_column_int64(st_turns, 0);

    // 绑定新 turn 字段
    // 注意：列可能是 NULL，用 sqlite3_bind_null 保持 NULL 语义
    // 这里使用 sqlite3_bind_value 更通用，但显式处理 NULL 更清晰。
    {
      // full_ctx_pre
      if (sqlite3_column_type(st_turns, 1) == SQLITE_NULL)
        sqlite3_bind_null(st_ins_turn, 1);
      else
        sqlite3_bind_text(st_ins_turn, 1,
                          (const char*)sqlite3_column_text(st_turns, 1),
                          sqlite3_column_bytes(st_turns, 1), SQLITE_TRANSIENT);

      // user_content
      if (sqlite3_column_type(st_turns, 2) == SQLITE_NULL)
        sqlite3_bind_null(st_ins_turn, 2);
      else
        sqlite3_bind_text(st_ins_turn, 2,
                          (const char*)sqlite3_column_text(st_turns, 2),
                          sqlite3_column_bytes(st_turns, 2), SQLITE_TRANSIENT);

      // full_ctx_end
      if (sqlite3_column_type(st_turns, 3) == SQLITE_NULL)
        sqlite3_bind_null(st_ins_turn, 3);
      else
        sqlite3_bind_text(st_ins_turn, 3,
                          (const char*)sqlite3_column_text(st_turns, 3),
                          sqlite3_column_bytes(st_turns, 3), SQLITE_TRANSIENT);

      // session_id = 目标会话
      sqlite3_bind_int64(st_ins_turn, 4, _new_s_id);
    }

    if (sqlite3_step(st_ins_turn) != SQLITE_DONE) {
      goto cleanup;
    }
    sqlite3_reset(st_ins_turn);
    sqlite3_clear_bindings(st_ins_turn);

    long long new_turn_id = sqlite3_last_insert_rowid(db);

    // 5) 复制该 turn 下的所有 steps
    sqlite3_reset(st_steps);
    sqlite3_clear_bindings(st_steps);
    sqlite3_bind_int64(st_steps, 1, old_turn_id);

    int sres;
    while ((sres = sqlite3_step(st_steps)) == SQLITE_ROW) {
      // reasoning_content
      if (sqlite3_column_type(st_steps, 0) == SQLITE_NULL)
        sqlite3_bind_null(st_ins_step, 2);
      else
        sqlite3_bind_text(st_ins_step, 2,
                          (const char*)sqlite3_column_text(st_steps, 0),
                          sqlite3_column_bytes(st_steps, 0), SQLITE_TRANSIENT);

      // assistant_out
      if (sqlite3_column_type(st_steps, 1) == SQLITE_NULL)
        sqlite3_bind_null(st_ins_step, 3);
      else
        sqlite3_bind_text(st_ins_step, 3,
                          (const char*)sqlite3_column_text(st_steps, 1),
                          sqlite3_column_bytes(st_steps, 1), SQLITE_TRANSIENT);

      // tool_call_json
      if (sqlite3_column_type(st_steps, 2) == SQLITE_NULL)
        sqlite3_bind_null(st_ins_step, 4);
      else
        sqlite3_bind_text(st_ins_step, 4,
                          (const char*)sqlite3_column_text(st_steps, 2),
                          sqlite3_column_bytes(st_steps, 2), SQLITE_TRANSIENT);

      // turn_id = 新 turn 的 id
      sqlite3_bind_int64(st_ins_step, 1, new_turn_id);

      if (sqlite3_step(st_ins_step) != SQLITE_DONE) {
        goto cleanup;
      }
      sqlite3_reset(st_ins_step);
      sqlite3_clear_bindings(st_ins_step);
    }
    if (sres != SQLITE_DONE) {
      goto cleanup;
    }
  }
  if (rc != SQLITE_DONE) {
    goto cleanup;
  }

  ok = true;

cleanup:
  if (st_turns) sqlite3_finalize(st_turns);
  if (st_steps) sqlite3_finalize(st_steps);
  if (st_ins_turn) sqlite3_finalize(st_ins_turn);
  if (st_ins_step) sqlite3_finalize(st_ins_step);

  if (ok) {
    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
      return false;
    }
  } else {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    LOG_ERROR("sqlite3 checkout_session failed,errmsg:{}", sqlite3_errmsg(db));
  }
  return ok;
}
