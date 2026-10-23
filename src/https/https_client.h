#ifndef HTTPS_CLIENT_H
#define HTTPS_CLIENT_H

#include "curl/curl.h"
#include "json.hpp"
#include <string>

// ---------- 数据结构与回调类型 ----------
struct https_response {
  long status_code = 0;
  std::string body;
  nlohmann::json headers;
  const nlohmann::json body_json() const { return nlohmann::json::parse(body); }
};

// ---------- 工具函数：去除字符串首尾空白 ----------
std::string trim(const std::string &s);

// ---------- HTTPS 客户端类 ----------
class https_client {
 public:
  https_client();

  ~https_client();

          // 禁止拷贝
  https_client(const https_client &) = delete;
  https_client &operator=(const https_client &) = delete;

          // ---------- GET 请求 ----------
  https_response get(const std::string &url, const nlohmann::json &headers);

          // ---------- POST JSON 请求 ----------
  https_response post(const std::string &url, const nlohmann::json &headers,
                      const std::string body);

          // 可继续添加 post_form、put、delete 等方法...

 private:
  CURL *curl_;

          // 写数据回调
  static size_t write_callback(void *contents, size_t size, size_t nmemb,
                               void *userp) {
    size_t total = size * nmemb;
    static_cast<std::string *>(userp)->append(static_cast<char *>(contents),
                                             total);
    return total;
  }

          // 头部回调
  static size_t header_callback(char *buffer, size_t size, size_t nitems,
                                void *userp) {
    size_t total = size * nitems;
    std::string line(buffer, total);
    // 忽略 HTTP 状态行（不以 ':' 开头的行）
    auto colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      // 去除首尾空白和尾部 "\r\n"
      key = trim(key);
      value = trim(value);
      auto *headers = static_cast<nlohmann::ordered_json *>(userp);
      (*headers)[key] = value; // 如有重复键，保留最后一个
    }
    return total;
  }

          // 统一请求执行函数
  https_response perform_request(
      const std::string &url, const nlohmann::json &headers,
      const std::string *post_body,     // 非空表示 POST/PUT 等带请求体
      const std::string &custom_method, // 如 "GET", "POST", "PUT", "DELETE"
      bool has_body);
};

#endif