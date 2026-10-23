#ifndef TASK_TREE_H
#define TASK_TREE_H

#include "log.h"
#include "json.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace kw {

struct task_node {
  bool finish = false;
  std::string task;   // 任务描述
  std::string err;    // 错误信息
  bool valid = false; // 节点是否有效（用于删除标记）
  std::vector<task_node> child;

  task_node() = default;

  // 标记删除一个子节点（索引基于1）
  void erase_child(size_t idx_1based) {
    // idx_1based 应在 [1, child.size()] 内
    if (idx_1based > 0 && idx_1based <= child.size()) {
      child[idx_1based - 1].valid = false;
      child[idx_1based - 1].task.clear();
      child[idx_1based - 1].err.clear();
      child[idx_1based - 1].finish = false;
      // 不真正移除元素，保持后续索引不变
    } else {
      // 记录错误，不做其他操作
      LOG_ERROR("erase_child out of range: " + std::to_string(idx_1based));
    }
  }
};

class task_tree {
public:
  void set_main_task(const std::string &_disc) { main.task = _disc; }
  void delete_main_task() { main.task.clear(); }

  // ---------- 修改器 (写入) ----------
  void change_task(const std::string &id, const std::string &description) {
    task_node &node = navigate(id, true); // 允许创建路径
    node.task = description;
    node.valid = true; // 确保节点有效
    node.finish = false;
  }

  void set_err(const std::string &id, const std::string &err_msg) {
    task_node &node = navigate(id, true);
    node.err = err_msg;
    node.valid = true; // 即使只设错误，也标记有效
    node.finish = false;
  }

  void set_fin(const std::string &id) {
    navigate(id, false).finish = true; // 只读导航，节点必须已存在
  }

  void set_unfin(const std::string &id) { navigate(id, false).finish = false; }

  void delete_node(const std::string &id) {
    // 找到父节点路径和要删除的子节点索引
    if (id.empty())
      return; // 不能删除根节点

    // 解析出父路径和最后一个数字
    size_t last_dot = id.rfind('.');
    std::string parent_id;
    int child_idx;
    if (last_dot == std::string::npos) {
      parent_id = ""; // 父节点为 main
      child_idx = std::stoi(id);
    } else {
      parent_id = id.substr(0, last_dot);
      child_idx = std::stoi(id.substr(last_dot + 1));
    }

    task_node &parent = parent_id.empty() ? main : navigate(parent_id, false);
    parent.erase_child(static_cast<size_t>(child_idx));
  }

  // ---------- 访问器 (读取) ----------
  std::string get_task(const std::string &id) const {
    try {
      return const_navigate(id).task;
    } catch (const std::out_of_range &) {
      return {};
    }
  }

  std::string get_err(const std::string &id) const {
    try {
      return const_navigate(id).err;
    } catch (const std::out_of_range &) {
      return {};
    }
  }

  bool get_fin(const std::string &id) const {
    try {
      return const_navigate(id).finish;
    } catch (const std::out_of_range &) {
      return false;
    }
  }

  std::string tasks_json() const {
    nlohmann::json j;
    j["main task"]["discription"] = main.task;
    flatten(main, "main", j);
    return j.dump();
  }

  // 从 tasks_json() 生成的 JSON 字符串重建任务树，覆盖当前内容
  std::string load_from_json(const std::string &json_str) {
    // 1. 清空当前树（重置根节点）
    main = task_node{};

    // 2. 解析 JSON
    nlohmann::json j = nlohmann::json::parse(json_str);
    if (!j.is_object()) {
      return "JSON for task tree must be an object";
    }

    // 3. 遍历所有键值对
    for (const auto &[key, val] : j.items()) {
      // 跳过根节点键（当前序列化不输出 "main"，但以防万一）
      if (key == "main task") {
        continue;
      }

      // 提取字段，缺失时提供默认值
      std::string description = val.value("discription", "");
      std::string status = val.value("status", "unfinish");
      std::string error = val.value("error", "");

      // 4. 先创建/更新节点描述（会自动创建路径）
      change_task(key, description);

      // 5. 设置错误信息（如有）
      if (!error.empty()) {
        // set_err 内部也会标记节点有效，并可能重置 finish，所以放在 finish
        // 设置之前
        set_err(key, error);
      }

      // 6. 设置完成状态
      if (status == "finish") {
        set_fin(key);
      } else {
        set_unfin(key);
      }
    }
    return "finish";
  }

private:
  // 非 const 导航：若 allow_create 为
  // true，路径中不存在的节点会自动创建并标记有效
  task_node &navigate(const std::string &id, bool allow_create) {
    task_node *node = &main;
    const char *p = id.c_str();

    while (*p) {
      if (*p == '.') {
        ++p;
        continue;
      }

      char *end;
      long val = std::strtol(p, &end, 10);
      if (p == end || val <= 0)
        throw std::invalid_argument("Invalid task id: " + id);

      size_t idx = static_cast<size_t>(val);
      if (allow_create && node->child.size() < idx) {
        node->child.resize(idx); // 扩容到刚好容纳 idx（索引 idx-1）
        // 新创建的节点 valid 默认为 false，稍后由调用者设为 true
      }
      if (node->child.size() < idx)
        throw std::out_of_range("Node not found: " + id);

      node = &node->child[idx - 1];
      p = end;
    }
    return *node;
  }

  // const 导航：只读，节点必须存在且有效
  const task_node &const_navigate(const std::string &id) const {
    const task_node *node = &main;
    const char *p = id.c_str();

    while (*p) {
      if (*p == '.') {
        ++p;
        continue;
      }

      char *end;
      long val = std::strtol(p, &end, 10);
      if (p == end || val <= 0)
        throw std::invalid_argument("Invalid task id: " + id);

      size_t idx = static_cast<size_t>(val);
      if (node->child.size() < idx || !node->child[idx - 1].valid)
        throw std::out_of_range("Node not found: " + id);

      node = &node->child[idx - 1];
      p = end;
    }
    return *node;
  }

  // 递归展平 JSON，跳过无效节点
  static void flatten(const task_node &node, const std::string &path,
                      nlohmann::json &j) {
    // 输出当前节点内容（根节点 "main" 特殊处理）
    if (path != "main") {
      j[path]["discription"] = node.task;
      j[path]["status"] = node.finish ? "finish" : "unfinish";
      j[path]["error"] = node.err;
    }

    // 遍历子节点，仅处理有效节点
    int index = 1;
    for (const auto &child : node.child) {
      if (child.valid) {
        std::string child_path;
        if (path == "main")
          child_path = std::to_string(index);
        else
          child_path = path + "." + std::to_string(index);
        flatten(child, child_path, j);
      }
      ++index; // 保持索引计数，即使跳过无效节点
    }
  }

  task_node main;
};
}; // namespace kw

#endif