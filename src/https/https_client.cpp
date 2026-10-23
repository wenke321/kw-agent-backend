#include "https_client.h"
#include "helpers/log.h"


// ---------- 工具函数：去除字符串首尾空白 ----------
std::string trim(const std::string &s) {
  const char *whitespace = " \t\r\n";
  size_t start = s.find_first_not_of(whitespace);
  if (start == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(whitespace);
  return s.substr(start, end - start + 1);
}

https_client::https_client() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl_ = curl_easy_init();
  if (!curl_)
    throw std::runtime_error("Failed to initialize libcurl");

          // 默认配置（可根据需要调整）
  curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 01L); // 示例中跳过证书验证
  curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L); // 启用 TCP keep-alive
  // 注册回调
  curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, header_callback);
}

https_client::~https_client() {
  if (curl_) {
    curl_easy_cleanup(curl_);
  }
  curl_global_cleanup();
}

        // ---------- GET 请求 ----------
https_response https_client::get(const std::string &url, const nlohmann::json &headers) {
  return perform_request(url, headers, nullptr, "", false);
}

        // ---------- POST JSON 请求 ----------
https_response https_client::post(const std::string &url, const nlohmann::json &headers,
                                  const std::string body) {
  auto hdrs = headers;
  if (hdrs.find("Content-Type") == hdrs.end()) {
    hdrs["Content-Type"] = "application/json";
  }
  return perform_request(url, hdrs, &body, "POST", true);
}

        // 统一请求执行函数
https_response https_client::perform_request(
    const std::string &url, const nlohmann::json &headers,
    const std::string *post_body,     // 非空表示 POST/PUT 等带请求体
    const std::string &custom_method, // 如 "GET", "POST", "PUT", "DELETE"
    bool has_body) {
  // 设置 URL
  curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());

          // 构建自定义头部链表
  struct curl_slist *header_slist = nullptr;
  for (const auto& [key, value] : headers.items()) {
    // if (h.size() != 2 || !h[0].is_string() || !h[1].is_string()) {
    //   LOG_ERROR("invalid headers");
    // }
    //auto a = h.get<std::pair<std::string, std::string>>();
    std::string header_str = key + ": " + value.get<std::string>();
    header_slist = curl_slist_append(header_slist, header_str.c_str());
  }
  curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_slist);

          // 设置请求方法
  if (custom_method == "GET") {
    curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
  } else if (custom_method == "POST" || custom_method == "PUT") {
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, custom_method.c_str());
    if (post_body && has_body) {
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, post_body->c_str());
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, post_body->size());
    } else {
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, 0);
    }
  } else {
    // 其他方法（DELETE, PATCH 等）
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, custom_method.c_str());
    if (post_body && has_body) {
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, post_body->c_str());
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, post_body->size());
    }
  }

          // 接收容器
  std::string body;
  nlohmann::ordered_json response_headers;
  curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &response_headers);

          // 执行请求
  CURLcode res = curl_easy_perform(curl_);

          // 清理头部链表
  if (header_slist) {
    curl_slist_free_all(header_slist);
  }

          // 处理结果
  long status_code = 0;
  if (res != CURLE_OK) {
    LOG_ERROR("curl_perform error{}", curl_easy_strerror(res));
  } else {
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status_code);
  }

  return {status_code, std::move(body), std::move(response_headers)};
}