
#include "helpers/function.h"

#if defined(_WIN32)
#include <iostream>

// ============================================================
// 极简测试框架
// ============================================================
static int g_pass = 0;
static int g_fail = 0;

namespace fs = std::filesystem;

struct TestEntry {
  std::string name;
  void (*fn)();
};
static std::vector<TestEntry>& tests() {
  static std::vector<TestEntry> t;
  return t;
}

#define TEST(name) \
  static void name(); \
  static struct Reg_##name { \
    Reg_##name() { tests().push_back({#name, name}); } \
  } reg_##name; \
  static void name()

#define CHECK(cond) \
  do { \
    if (cond) { \
      ++g_pass; \
    } else { \
      ++g_fail; \
      std::cout << "  [FAIL] " << #cond << "  (line " << __LINE__ << ")\n"; \
    } \
  } while (0)

#define CHECK_EQ(a, b) \
  do { \
    auto _a = (a); \
    auto _b = (b); \
    if (_a == _b) { \
      ++g_pass; \
    } else { \
      ++g_fail; \
      std::cout << "  [FAIL] " << #a << " == " << #b << "  (line " << __LINE__ \
                << ")" \
                << "  got=[" << _a << "] want=[" << _b << "]\n"; \
    } \
  } while (0)

// ============================================================
// 工具：切 CWD
// ============================================================
struct CwdGuard {
  fs::path old;
  CwdGuard() : old(fs::current_path()) {}
  ~CwdGuard() {
    std::error_code ec;
    fs::current_path(old, ec);
  }
};

// 判断一行是否为 "数字:..." 形态
static bool looks_like_match_line(const std::string& line) {
  size_t i = 0;
  while (i < line.size() && std::isdigit((unsigned char)line[i])) ++i;
  return i > 0 && i < line.size() && line[i] == ':';
}

// 从一段文本中数出匹配行数（排除文件标题行）
static int count_match_lines(const std::string& text) {
  int n = 0;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (looks_like_match_line(line)) ++n;
  }
  return n;
}

static void cleanup_temp(const std::string& p) {
  if (p.empty()) return;
  std::error_code ec;
  fs::remove(p, ec);
}

// ============================================================
// 测试夹具
// ============================================================
static fs::path g_fixture;

static void write_file(const fs::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  f.write(s.data(), (std::streamsize)s.size());
}

static void setup_fixture() {
  static bool done = false;
  if (done) return;
  done = true;

  auto base = fs::temp_directory_path() /
              ("kw_greptest_" + std::to_string(::GetTickCount64()));
  fs::create_directories(base);
  fs::create_directories(base / "sub");
  g_fixture = base;

  // a.txt: 3 处 foo + 1 处 bar
  write_file(base / "a.txt",
             "foo one\n"
             "bar\n"
             "foo two\n"
             "baz\n"
             "foo three\n");

  // b.txt: 2 处 foo
  write_file(base / "b.txt",
             "hello foo\n"
             "world\n"
             "foo again\n");

  // c.txt: 无匹配
  write_file(base / "c.txt", "nothing here\nat all\n");

  // sub/d.cpp: 1 处 foo
  write_file(base / "sub" / "d.cpp", "int foo = 1;\nint bar = 2;\n");

  // sub/e.js: 1 处 foo
  write_file(base / "sub" / "e.js", "let x = foo;\n");

  // empty.txt
  write_file(base / "empty.txt", "");

  // crlf.txt: CRLF 行尾，2 处 foo
  write_file(base / "crlf.txt", "foo\r\nbar\r\nfoo\r\n");

  // nonl.txt: 最后一行无换行符，2 处 foo
  write_file(base / "nonl.txt", "foo\nbar\nfoo");

  // big.txt: 300 处 foo（用于截断测试）
  {
    std::string s;
    for (int i = 0; i < 300; ++i) s += "line" + std::to_string(i) + " foo\n";
    write_file(base / "big.txt", s);
  }

  // long.txt: 一条超长匹配行（用于 --max-columns 测试）
  {
    std::string s = "foo ";
    s.append(2000, 'x');
    s += "\n";
    write_file(base / "long.txt", s);
  }

  // utf8.txt: 多字节内容
  write_file(base / "utf8.txt",
             "\xe4\xbd\xa0\xe5\xa5\xbd foo\n"  // 你好 foo
             "no match\n"
             "world foo \xe4\xb8\x96\xe7\x95\x8c\n");  // world foo 世界

  // with space.txt
  write_file(base / "with space.txt", "foo in a spaced name\n");
}

// ============================================================
//  测试用例
// ============================================================

// ---------- 1. 基础功能 ----------

TEST(test_basic_match_single_file) {
  auto r = kw::grep("foo", (g_fixture / "a.txt").string());
  CHECK(r.ok);
  CHECK(!r.truncated);
  CHECK_EQ(r.totalMatches, 3u);
  CHECK(r.fullListPath.empty());
  CHECK(r.content.find("1:foo one") != std::string::npos);
  CHECK(r.content.find("3:foo two") != std::string::npos);
  CHECK(r.content.find("5:foo three") != std::string::npos);
  CHECK(r.content.find("共 3 处匹配") != std::string::npos);
}

TEST(test_no_match) {
  auto r = kw::grep("zzzz_nothing", (g_fixture / "a.txt").string());
  CHECK(r.ok);  // 退出码 1 视为成功
  CHECK_EQ(r.exitCode, 1);
  CHECK_EQ(r.totalMatches, 0u);
  CHECK(!r.truncated);
  CHECK(r.content.find("未找到匹配") != std::string::npos);
}

TEST(test_exit_code_match) {
  auto r = kw::grep("foo", (g_fixture / "a.txt").string());
  CHECK_EQ(r.exitCode, 0);
}

TEST(test_directory_recursive) {
  // 期望：a(3)+b(2)+d.cpp(1)+e.js(1)+crlf(2)+nonl(2)+big(300)+long(1)+utf8(2)+space(1)
  // = 315 若夹具变更，此数字需同步更新。
  CwdGuard g;
  fs::current_path(g_fixture);
  auto r = kw::grep("foo", ".", "", 10000);  // 关掉截断以便校验总数
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 315u);
}

TEST(test_heading_grouping) {
  // 单文件也应有文件标题行
  auto path = (g_fixture / "a.txt").string();
  auto r = kw::grep("foo", path, "", 10000);
  CHECK(r.ok);
  CHECK(r.content.find(path) != std::string::npos);
}

// ---------- 2. include 过滤 ----------

TEST(test_include_glob_cpp) {
  CwdGuard g;
  fs::current_path(g_fixture);
  auto r = kw::grep("foo", ".", "*.cpp", 10000);
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 1u);
  CHECK(r.content.find("d.cpp") != std::string::npos);
  CHECK(r.content.find("a.txt") == std::string::npos);
}

TEST(test_include_glob_braces) {
  // txt(3+2+2+2+300+1+2+1=313) + cpp(1) = 314
  CwdGuard g;
  fs::current_path(g_fixture);
  auto r = kw::grep("foo", ".", "*.{txt,cpp}", 10000);
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 314u);
  CHECK(r.content.find("e.js") == std::string::npos);
}

// ---------- 3. 参数校验与错误路径 ----------

TEST(test_empty_pattern_rejected) {
  auto r = kw::grep("");
  CHECK(!r.ok);
  CHECK(!r.error.empty());
  CHECK(r.content.empty());
}

TEST(test_invalid_regex) {
  auto r = kw::grep("(", (g_fixture / "a.txt").string());
  CHECK(!r.ok);
  CHECK_EQ(r.exitCode, 2);
  CHECK(!r.error.empty());
}

TEST(test_nonexistent_path) {
  auto r = kw::grep("foo", (g_fixture / "no_such_file_xyz").string());
  CHECK(!r.ok);
  CHECK(r.exitCode != 0 && r.exitCode != 1);
  CHECK(r.error.size() > 5);  // 不能再是空的 "rg exited with code 2"
}

// ---------- 4. 截断行为 ----------

TEST(test_truncation_triggered) {
  auto r = kw::grep("foo", (g_fixture / "big.txt").string(), "", 250);
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 300u);
  CHECK(r.truncated);
  CHECK(!r.fullListPath.empty());
  CHECK(fs::exists(r.fullListPath));
  CHECK(fs::file_size(r.fullListPath) > 0);

  // 内联部分应恰好 250 条
  CHECK_EQ(count_match_lines(r.content), 250);

  // 提示语提到保存位置
  CHECK(r.content.find(r.fullListPath) != std::string::npos);

  cleanup_temp(r.fullListPath);
}

TEST(test_saved_file_contains_all_matches) {
  auto r = kw::grep("foo", (g_fixture / "big.txt").string(), "", 250);
  CHECK(r.truncated);

  std::ifstream f(r.fullListPath, std::ios::binary);
  CHECK(f.good());

  int saved = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (looks_like_match_line(line)) ++saved;
  }
  CHECK_EQ(saved, 300);

  cleanup_temp(r.fullListPath);
}

TEST(test_no_truncation_when_under_limit) {
  auto r = kw::grep("foo", (g_fixture / "big.txt").string(), "", 1000);
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 300u);
  CHECK(!r.truncated);
  CHECK(r.fullListPath.empty());
  CHECK_EQ(count_match_lines(r.content), 300);
}

TEST(test_no_truncation_at_exact_boundary) {
  // 恰好 300 条匹配、上限 300：不应截断
  auto r = kw::grep("foo", (g_fixture / "big.txt").string(), "", 300);
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 300u);
  CHECK(!r.truncated);
}

TEST(test_truncation_at_boundary_plus_one) {
  // 上限 299：应截断
  auto r = kw::grep("foo", (g_fixture / "big.txt").string(), "", 299);
  CHECK(r.ok);
  CHECK(r.truncated);
  CHECK_EQ(count_match_lines(r.content), 299);
  cleanup_temp(r.fullListPath);
}

TEST(test_max_inline_zero) {
  auto r = kw::grep("foo", (g_fixture / "a.txt").string(), "", 0);
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 3u);
  CHECK(r.truncated);
  CHECK_EQ(count_match_lines(r.content), 0);
  CHECK(!r.fullListPath.empty());
  cleanup_temp(r.fullListPath);
}

// ---------- 5. 编码 / 特殊字符 ----------

TEST(test_crlf_input) {
  auto r = kw::grep("foo", (g_fixture / "crlf.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 2u);
  CHECK(r.content.find('\r') == std::string::npos);  // 无残留 \r
  CHECK(r.content.find("1:foo") != std::string::npos);
  CHECK(r.content.find("3:foo") != std::string::npos);
}

TEST(test_no_trailing_newline) {
  auto r = kw::grep("foo", (g_fixture / "nonl.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 2u);  // 最后一行虽无 \n 也应命中
}

TEST(test_utf8_pattern) {
  auto r = kw::grep("你好", (g_fixture / "utf8.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 1u);
  CHECK(r.content.find("你好") != std::string::npos);
}

TEST(test_utf8_in_content) {
  auto r = kw::grep("foo", (g_fixture / "utf8.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 2u);
  CHECK(r.content.find("你好") != std::string::npos);
  CHECK(r.content.find("世界") != std::string::npos);
}

// ---------- 6. quoteArg 相关：含空格 / 引号 / 反斜杠 ----------

TEST(test_pattern_with_spaces) {
  auto r = kw::grep("foo one", (g_fixture / "a.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 1u);
}

TEST(test_path_with_spaces) {
  auto r = kw::grep("foo", (g_fixture / "with space.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 1u);
}

TEST(test_pattern_with_backslash_regex) {
  auto r = kw::grep("\\bfoo\\b", (g_fixture / "a.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 3u);
}

TEST(test_pattern_with_quotes) {
  auto r = kw::grep("\"", (g_fixture / "a.txt").string());
  CHECK(r.ok);  // 命令能正常起，无引号错乱
  CHECK_EQ(r.totalMatches, 0u);
}

TEST(test_pattern_alternation) {
  auto r = kw::grep("foo|bar", (g_fixture / "a.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 4u);  // 3×foo + 1×bar
}

// ---------- 7. 长行截断（--max-columns） ----------

TEST(test_long_line_is_previewed_not_dumped) {
  auto r = kw::grep("foo", (g_fixture / "long.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 1u);
  // 不应把 2000+ 字符整行塞进内联输出
  CHECK(r.content.size() < 1500);
  // rg 会打一个类似 "[... omitted end of long line]" 的标记
  CHECK(r.content.find("omitted") != std::string::npos ||
        r.content.find("[...]") != std::string::npos);
}

// ---------- 8. 确定性 / 默认路径 ----------

TEST(test_deterministic_output) {
  CwdGuard g;
  fs::current_path(g_fixture);
  auto r1 = kw::grep("foo", ".", "*.txt", 10000);
  auto r2 = kw::grep("foo", ".", "*.txt", 10000);
  CHECK_EQ(r1.content, r2.content);  // --sort path 保证顺序一致
}

TEST(test_default_path_is_cwd) {
  CwdGuard g;
  fs::current_path(g_fixture);
  auto r = kw::grep("foo", "", "*.txt", 10000);
  CHECK(r.ok);
  CHECK(r.totalMatches > 0u);
}

// ============================================================
// main
// ============================================================
int main() {
  ::SetConsoleOutputCP(CP_UTF8);

  setup_fixture();
  std::cout << "fixture: " << g_fixture.string() << "\n\n";

  int failed = 0;
  for (auto& t : tests()) {
    int before = g_fail;
    std::cout << "[ RUN  ] " << t.name << "\n";
    t.fn();
    if (g_fail == before) {
      std::cout << "[  OK  ] " << t.name << "\n";
    } else {
      std::cout << "[ FAIL ] " << t.name << "\n";
      ++failed;
    }
  }

  std::cout << "\n========== 汇总 ==========\n";
  std::cout << "断言通过 : " << g_pass << "\n";
  std::cout << "断言失败 : " << g_fail << "\n";
  std::cout << "用例失败 : " << failed << " / " << tests().size() << "\n";

  // 清理夹具
  std::error_code ec;
  fs::remove_all(g_fixture, ec);

  return failed == 0 ? 0 : 1;
}
#elif defined(__linux__)
// =============================================================================
//  test_grep_tool.cpp
//  单文件测试（无外部依赖，除系统 ripgrep）。
//
//  编译:  g++ -std=c++20 -O2 -Wall -Wextra -o test_grep_tool test_grep_tool.cpp
//  运行:  ./test_grep_tool          # 只显示汇总与失败信息
//         ./test_grep_tool -v       # 同时打印 GrepTool 内部的
//         LOG_ERROR/LOG_WARN
//
//  要求:  Linux 6.12+，glibc >= 2.29（posix_spawn_file_actions_addchdir_np），
//         ripgrep >= 12
// =============================================================================

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace fs = std::filesystem;

// =============================================================================
//  1) 极简日志 shim（替代 spdlog，只做 "{}" 占位替换）
// =============================================================================
namespace tlog {

static bool enabled = false;

inline void format_impl(std::string&) {}

template <typename T, typename... Rest>
void format_impl(std::string& s, T&& v, Rest&&... rest) {
  auto p = s.find("{}");
  if (p == std::string::npos) return;
  std::ostringstream oss;
  oss << v;
  std::string rep = oss.str();
  s.replace(p, 2, rep);
  format_impl(s, std::forward<Rest>(rest)...);
}

template <typename... Args>
void log_line(const char* lvl, const char* fmt, Args&&... args) {
  if (!enabled) return;
  std::string s(fmt);
  format_impl(s, std::forward<Args>(args)...);
  std::fprintf(stderr, "[%s] %s\n", lvl, s.c_str());
}

}  // namespace tlog

#define LOG_ERROR(...) ::tlog::log_line("ERROR", __VA_ARGS__)
#define LOG_WARN(...) ::tlog::log_line("WARN", __VA_ARGS__)

// =============================================================================
//  2) GrepTool 完整实现（与生产代码同源）
// =============================================================================

#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
#define GREPTOOL_HAS_SPAWN_CHDIR 1
#else
#define GREPTOOL_HAS_SPAWN_CHDIR 0
#endif

struct GrepParams {
  std::string pattern;
  std::optional<std::string> path;
  std::optional<std::string> include;
};

struct GrepResult {
  bool ok = false;
  std::string error;
  std::string output;
  std::size_t total_matches = 0;
  std::size_t returned_matches = 0;
  bool truncated = false;
  std::string saved_path;
  std::string warning;

  static GrepResult failure(std::string msg) {
    GrepResult r;
    r.ok = false;
    r.error = std::move(msg);
    return r;
  }
};

struct GrepOptions {
  std::string rg_path = "rg";
  std::chrono::milliseconds timeout{30'000};
  std::size_t max_spill_bytes = 256ull * 1024 * 1024;
  std::size_t inline_bytes_limit = 8ull * 1024 * 1024;
  bool sort_by_path = false;
};

class GrepTool {
 public:
  explicit GrepTool(std::string workspace, std::size_t inline_limit = 250,
                    std::size_t max_line_bytes = 4096,
                    GrepOptions options = {});
  GrepResult run(const GrepParams& params) const;

 private:
  std::string workspace_;
  std::size_t inline_limit_;
  std::size_t max_line_bytes_;
  GrepOptions options_;
};

// -------- GrepTool 内部 helpers --------

static constexpr std::size_t kErrCap = 64 * 1024;
static constexpr std::size_t kMaxPartialExtra = 4 * 1024;

struct ChildGuard {
  pid_t pid = -1;
  ChildGuard() = default;
  ChildGuard(const ChildGuard&) = delete;
  ChildGuard& operator=(const ChildGuard&) = delete;
  ~ChildGuard() { reap(); }
  void adopt(pid_t p) { pid = p; }
  void release() { pid = -1; }
  void reap() {
    if (pid <= 0) return;
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    pid = -1;
  }
};

static std::string errno_msg(const char* what, int err) {
  std::string s(what);
  s += ": ";
  s += std::generic_category().message(err);
  return s;
}

static bool has_nul(const std::string& s) {
  return s.find('\0') != std::string::npos;
}

static std::string trim_ws(std::string s) {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) ++i;
  s.erase(0, i);
  return s;
}

static bool write_all(int fd, const char* data, std::size_t len) {
  while (len > 0) {
    ssize_t n = ::write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    len -= static_cast<std::size_t>(n);
  }
  return true;
}
static bool write_all(int fd, std::string_view s) {
  return write_all(fd, s.data(), s.size());
}

struct TempFile {
  int fd = -1;
  std::string path;
  std::size_t bytes = 0;
  ~TempFile() {
    if (fd >= 0) ::close(fd);
  }
  bool create() {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string dir = (tmpdir && *tmpdir) ? tmpdir : "/tmp";
    std::string tmpl = dir + "/agent-grep-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    fd = ::mkostemp(buf.data(), O_CLOEXEC);
    if (fd < 0) return false;
    path.assign(buf.data());
    return true;
  }
};

static bool path_inside(const std::string& base, const std::string& p) {
  if (base == "/") return !p.empty() && p.front() == '/';
  if (p == base) return true;
  return p.size() > base.size() && p.compare(0, base.size(), base) == 0 &&
         p[base.size()] == '/';
}

// -------- GrepTool 实现 --------

GrepTool::GrepTool(std::string workspace, std::size_t inline_limit,
                   std::size_t max_line_bytes, GrepOptions options)
    : workspace_(std::move(workspace)),
      inline_limit_(std::max<std::size_t>(1, inline_limit)),
      max_line_bytes_(std::clamp<std::size_t>(max_line_bytes, std::size_t{64},
                                              std::size_t{1} << 20)),
      options_(std::move(options)) {}

GrepResult GrepTool::run(const GrepParams& params) const {
  auto fail_user = [](std::string msg) -> GrepResult {
    LOG_WARN("grep: {}", msg);
    return GrepResult::failure(std::move(msg));
  };
  auto fail_sys = [](std::string msg) -> GrepResult {
    LOG_ERROR("grep: {}", msg);
    return GrepResult::failure(std::move(msg));
  };

  if (workspace_.empty()) return fail_user("workspace must not be empty");
  if (has_nul(workspace_)) return fail_user("workspace contains NUL");
  if (params.pattern.empty()) return fail_user("pattern must not be empty");
  if (has_nul(params.pattern)) return fail_user("pattern contains NUL");
  if (options_.rg_path.empty()) return fail_user("rg_path must not be empty");
  if (has_nul(options_.rg_path)) return fail_user("rg_path contains NUL");
  if (options_.timeout.count() < 0)
    return fail_user("timeout must be non-negative");
  if (options_.inline_bytes_limit == 0)
    return fail_user("inline_bytes_limit must be > 0");

  if (params.include) {
    if (params.include->empty())
      return fail_user("include must not be empty when provided");
    if (has_nul(*params.include)) return fail_user("include contains NUL");
    if (params.include->front() == '!')
      return fail_user("include does not support negation ('!' prefix)");
    if (params.include->find("..") != std::string::npos)
      return fail_user("include must not contain '..'");
  }
  if (params.path && has_nul(*params.path))
    return fail_user("path contains NUL");

  std::error_code ec;
  fs::path ws = fs::weakly_canonical(fs::path(workspace_), ec);
  if (ec) return fail_user("invalid workspace: " + ec.message());
  ec.clear();
  if (!fs::is_directory(ws, ec)) {
    if (ec) return fail_sys("workspace check failed: " + ec.message());
    return fail_user("workspace is not a directory: " + ws.native());
  }

  fs::path target;
  if (params.path && !params.path->empty()) {
    fs::path p(*params.path);
    ec.clear();
    target = fs::weakly_canonical(p.is_absolute() ? p : ws / p, ec);
    if (ec)
      return fail_user("invalid path '" + *params.path + "': " + ec.message());
    if (!path_inside(ws.native(), target.native()))
      return fail_user("path '" + *params.path +
                       "' escapes the session workspace");
    ec.clear();
    if (!fs::exists(target, ec)) {
      if (ec) return fail_sys("path stat failed: " + ec.message());
      return fail_user("path does not exist: " + target.native());
    }
  } else {
    target = ws;
  }

  fs::path rel_target = target.lexically_relative(ws);
  if (rel_target.empty()) rel_target = ".";
  std::string target_arg = rel_target.native();

  std::vector<std::string> argv;
  argv.reserve(20);
  argv.emplace_back(options_.rg_path);
  argv.emplace_back("--line-number");
  argv.emplace_back("--with-filename");
  argv.emplace_back("--no-heading");
  argv.emplace_back("--color=never");
  argv.emplace_back("--binary-files=without-match");
  argv.emplace_back("--max-columns");
  argv.emplace_back(std::to_string(max_line_bytes_));
  argv.emplace_back("--max-columns-preview");

  if (options_.sort_by_path) {
    argv.emplace_back("--sort=path");
  } else {
    argv.emplace_back("-j1");
  }

  if (params.include) {
    argv.emplace_back("-g");
    argv.emplace_back(*params.include);
  }

  argv.emplace_back("--");
  argv.emplace_back(params.pattern);
  argv.emplace_back(target_arg);

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe2(out_pipe, O_CLOEXEC) != 0)
    return fail_sys(errno_msg("pipe2(stdout)", errno));
  if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
    int e = errno;
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return fail_sys(errno_msg("pipe2(stderr)", e));
  }
  if (out_pipe[0] <= 2 || out_pipe[1] <= 2 || err_pipe[0] <= 2 ||
      err_pipe[1] <= 2) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return fail_sys("standard fds 0-2 are not open; cannot spawn child safely");
  }

  auto close_pipes = [&]() {
    if (out_pipe[0] >= 0) {
      ::close(out_pipe[0]);
      out_pipe[0] = -1;
    }
    if (out_pipe[1] >= 0) {
      ::close(out_pipe[1]);
      out_pipe[1] = -1;
    }
    if (err_pipe[0] >= 0) {
      ::close(err_pipe[0]);
      err_pipe[0] = -1;
    }
    if (err_pipe[1] >= 0) {
      ::close(err_pipe[1]);
      err_pipe[1] = -1;
    }
  };

  posix_spawn_file_actions_t fa;
  if (int r = posix_spawn_file_actions_init(&fa); r != 0) {
    close_pipes();
    return fail_sys(errno_msg("posix_spawn_file_actions_init", r));
  }

  auto cleanup_and_fail = [&](const char* op, int err) -> GrepResult {
    posix_spawn_file_actions_destroy(&fa);
    close_pipes();
    return fail_sys(errno_msg(op, err));
  };

  if (int r = posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
      r != 0)
    return cleanup_and_fail("adddup2(stdout)", r);
  if (int r = posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
      r != 0)
    return cleanup_and_fail("adddup2(stderr)", r);
  if (int r = posix_spawn_file_actions_addclose(&fa, out_pipe[0]); r != 0)
    return cleanup_and_fail("addclose(stdout[0])", r);
  if (int r = posix_spawn_file_actions_addclose(&fa, out_pipe[1]); r != 0)
    return cleanup_and_fail("addclose(stdout[1])", r);
  if (int r = posix_spawn_file_actions_addclose(&fa, err_pipe[0]); r != 0)
    return cleanup_and_fail("addclose(stderr[0])", r);
  if (int r = posix_spawn_file_actions_addclose(&fa, err_pipe[1]); r != 0)
    return cleanup_and_fail("addclose(stderr[1])", r);

#if GREPTOOL_HAS_SPAWN_CHDIR
  if (int r = posix_spawn_file_actions_addchdir_np(&fa, ws.c_str()); r != 0)
    return cleanup_and_fail("addchdir_np(workspace)", r);
#else
  posix_spawn_file_actions_destroy(&fa);
  close_pipes();
  return fail_sys(
      "posix_spawn_file_actions_addchdir_np not available (glibc >= 2.29 required)");
#endif

  posix_spawnattr_t attr;
  if (int r = posix_spawnattr_init(&attr); r != 0) {
    posix_spawn_file_actions_destroy(&fa);
    close_pipes();
    return fail_sys(errno_msg("posix_spawnattr_init", r));
  }
  auto attr_fail = [&](const char* op, int err) -> GrepResult {
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close_pipes();
    return fail_sys(errno_msg(op, err));
  };

  if (int r = posix_spawnattr_setflags(
          &attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
      r != 0)
    return attr_fail("posix_spawnattr_setflags", r);
  if (int r = posix_spawnattr_setpgroup(&attr, 0); r != 0)
    return attr_fail("posix_spawnattr_setpgroup", r);

  sigset_t empty_mask;
  ::sigemptyset(&empty_mask);
  if (int r = posix_spawnattr_setsigmask(&attr, &empty_mask); r != 0)
    return attr_fail("posix_spawnattr_setsigmask", r);

  pid_t pid = -1;
  int spawn_rc = ::posix_spawnp(&pid, options_.rg_path.c_str(), &fa, &attr,
                                cargv.data(), environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  ::close(out_pipe[1]);
  out_pipe[1] = -1;
  ::close(err_pipe[1]);
  err_pipe[1] = -1;

  if (spawn_rc != 0) {
    close_pipes();
    return fail_sys(errno_msg("posix_spawnp(rg)", spawn_rc));
  }

  ChildGuard guard;
  guard.adopt(pid);

  const std::size_t limit = inline_limit_;
  const std::size_t max_partial = max_line_bytes_ * 4 + kMaxPartialExtra;

  std::vector<std::string> head;
  head.reserve(std::min<std::size_t>(limit, 4096));

  std::size_t total = 0;
  std::size_t head_bytes = 0;
  bool truncated = false;
  bool io_error = false;
  bool spill_full = false;
  bool partial_truncated = false;
  bool any_line_truncated = false;
  TempFile spill;
  std::string partial;

  auto emit_line = [&](std::string_view line) {
    ++total;
    const bool fits_inline =
        (total <= limit) &&
        (head_bytes + line.size() + 1 <= options_.inline_bytes_limit);
    if (fits_inline) {
      head.emplace_back(line.data(), line.size());
      head_bytes += line.size() + 1;
      return;
    }

    if (!truncated) {
      truncated = true;
      if (!spill.create()) {
        LOG_ERROR("grep: failed to create spill file under $TMPDIR or /tmp");
        io_error = true;
        return;
      }
      for (auto& h : head) {
        if (spill.bytes + h.size() + 1 > options_.max_spill_bytes) {
          spill_full = true;
          break;
        }
        if (!write_all(spill.fd, h) || !write_all(spill.fd, "\n")) {
          LOG_ERROR("grep: write to spill file '{}' failed", spill.path);
          io_error = true;
          break;
        }
        spill.bytes += h.size() + 1;
      }
      if (io_error || spill_full) return;
    }
    if (io_error || spill_full) return;
    if (spill.bytes + line.size() + 1 > options_.max_spill_bytes) {
      spill_full = true;
      return;
    }
    if (!write_all(spill.fd, line) || !write_all(spill.fd, "\n")) {
      LOG_ERROR("grep: write to spill file '{}' failed", spill.path);
      io_error = true;
    } else {
      spill.bytes += line.size() + 1;
    }
  };

  auto feed = [&](const char* p, std::size_t n) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (p[i] != '\n') continue;
      std::size_t seg = i - start;
      std::size_t remaining =
          max_partial > partial.size() ? max_partial - partial.size() : 0;
      if (seg <= remaining) {
        partial.append(p + start, seg);
      } else {
        partial.append(p + start, remaining);
        partial_truncated = true;
        any_line_truncated = true;
      }
      if (partial_truncated) partial += " [...line truncated]";
      emit_line(partial);
      partial.clear();
      partial_truncated = false;
      start = i + 1;
    }
    if (start < n) {
      std::size_t seg = n - start;
      std::size_t remaining =
          max_partial > partial.size() ? max_partial - partial.size() : 0;
      if (seg <= remaining) {
        partial.append(p + start, seg);
      } else {
        partial.append(p + start, remaining);
        partial_truncated = true;
        any_line_truncated = true;
      }
    }
  };

  char buf[65536];
  std::string errbuf;
  bool out_done = false, err_done = false;
  bool timed_out = false;

  auto deadline = options_.timeout.count() > 0
                      ? std::chrono::steady_clock::now() + options_.timeout
                      : std::chrono::steady_clock::time_point::max();

  while (!out_done || !err_done) {
    int timeout_ms = -1;
    if (deadline != std::chrono::steady_clock::time_point::max()) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        timed_out = true;
        break;
      }
      auto rem =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count();
      timeout_ms = static_cast<int>(std::min<long long>(rem, INT_MAX));
    }
    struct pollfd pfds[2];
    pfds[0].fd = out_done ? -1 : out_pipe[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = err_done ? -1 : err_pipe[0];
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    int pr = ::poll(pfds, 2, timeout_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) {
      timed_out = true;
      break;
    }

    if (!out_done &&
        (pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      ssize_t n = ::read(out_pipe[0], buf, sizeof buf);
      if (n > 0)
        feed(buf, static_cast<std::size_t>(n));
      else if (n == 0)
        out_done = true;
      else if (errno != EINTR && errno != EAGAIN)
        out_done = true;
    }
    if (!err_done &&
        (pfds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      ssize_t n = ::read(err_pipe[0], buf, sizeof buf);
      if (n > 0) {
        if (errbuf.size() < kErrCap) {
          std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(n),
                                                   kErrCap - errbuf.size());
          errbuf.append(buf, take);
        }
      } else if (n == 0)
        err_done = true;
      else if (errno != EINTR && errno != EAGAIN)
        err_done = true;
    }
  }

  if (!partial.empty()) {
    if (partial_truncated) partial += " [...line truncated]";
    if (timed_out) partial += " [...incomplete, search timed out]";
    emit_line(partial);
    partial.clear();
  }

  if (timed_out) {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
  }
  if (out_pipe[0] >= 0) {
    ::close(out_pipe[0]);
    out_pipe[0] = -1;
  }
  if (err_pipe[0] >= 0) {
    ::close(err_pipe[0]);
    err_pipe[0] = -1;
  }

  int status = 0;
  pid_t w;
  do {
    w = ::waitpid(pid, &status, 0);
  } while (w < 0 && errno == EINTR);
  if (w < 0) {
    int e = errno;
    if (!spill.path.empty()) ::unlink(spill.path.c_str());
    guard.release();
    return fail_sys(errno_msg("waitpid", e));
  }
  guard.release();

  int exit_code = -1;
  if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);

  if (!timed_out && exit_code != 0 && exit_code != 1) {
    if (!spill.path.empty()) ::unlink(spill.path.c_str());
    std::string msg = "ripgrep failed";
    if (WIFSIGNALED(status)) {
      msg += " (terminated by signal " + std::to_string(WTERMSIG(status)) + ")";
    } else if (!errbuf.empty()) {
      msg += ": ";
      msg += trim_ws(errbuf);
    } else if (exit_code >= 0) {
      msg += " (exit code " + std::to_string(exit_code) + ")";
    }
    return fail_sys(std::move(msg));
  }

  if (io_error && !spill.path.empty()) {
    ::unlink(spill.path.c_str());
    spill.path.clear();
  }

  GrepResult r;
  r.ok = true;
  r.total_matches = total;
  r.returned_matches = head.size();
  r.truncated = truncated || timed_out;

  if (timed_out) {
    LOG_WARN("grep: search timed out after {}ms; results are partial",
             options_.timeout.count());
    r.warning += "search timed out; results are partial. ";
  }
  if (any_line_truncated) {
    LOG_WARN("grep: some lines exceeded {} bytes and were truncated",
             max_line_bytes_);
    r.warning += "some lines exceeded the inline byte cap and were truncated. ";
  }
  if (io_error) {
    r.warning += "spill file write failed; complete list is incomplete. ";
  }
  if (spill_full) {
    LOG_WARN(
        "grep: spill file reached {} bytes limit; saved list is incomplete",
        options_.max_spill_bytes);
    r.warning += "spill file reached size limit; saved list is incomplete. ";
  }
  if (!errbuf.empty()) {
    std::string e = trim_ws(errbuf);
    if (!e.empty()) {
      if (!r.warning.empty()) r.warning += ' ';
      r.warning += "ripgrep stderr: " + e;
    }
  }
  if (r.truncated && !io_error && !spill.path.empty())
    r.saved_path = spill.path;

  std::string out;
  std::size_t reserve = 0;
  for (auto& h : head) reserve += h.size() + 1;
  out.reserve(reserve + 2048);
  for (auto& h : head) {
    out += h;
    out += '\n';
  }

  if (r.truncated) {
    std::string note = "\n[truncated] showing first ";
    note += std::to_string(r.returned_matches);
    note += " of ";
    note += std::to_string(r.total_matches);
    note += timed_out ? " matched lines seen before timeout; "
                      : " matching lines; ";
    if (!r.saved_path.empty()) {
      note += "complete list saved to ";
      note += r.saved_path;
      if (spill_full) note += " (size limit reached, list is incomplete)";
    } else if (timed_out) {
      note += "search timed out before completion";
    } else {
      note += "complete list could not be saved";
    }
    note += '\n';
    out += note;
  }
  r.output = std::move(out);
  return r;
}

// =============================================================================
//  3) 极简测试框架
// =============================================================================
namespace testing {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> v;
  return v;
}

struct Registrar {
  Registrar(const char* n, std::function<void()> f) {
    registry().push_back({n, std::move(f)});
  }
};

#define TEST(name) \
  static void test_##name(); \
  static ::testing::Registrar reg_##name(#name, test_##name); \
  static void test_##name()

}  // namespace testing

// ---- 断言宏 ----

#define ASSERT_TRUE(cond) \
  do { \
    if (!(cond)) { \
      std::ostringstream _os; \
      _os << "ASSERT_TRUE(" #cond ") failed @ " << __FILE__ << ":" \
          << __LINE__; \
      throw std::runtime_error(_os.str()); \
    } \
  } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) \
  do { \
    auto&& _a = (a); \
    auto&& _b = (b); \
    if (!(_a == _b)) { \
      std::ostringstream _os; \
      _os << "ASSERT_EQ(" #a ", " #b ") failed: '" << _a << "' != '" << _b \
          << "' @ " << __FILE__ << ":" << __LINE__; \
      throw std::runtime_error(_os.str()); \
    } \
  } while (0)

#define ASSERT_NE(a, b) \
  do { \
    auto&& _a = (a); \
    auto&& _b = (b); \
    if (_a == _b) { \
      std::ostringstream _os; \
      _os << "ASSERT_NE(" #a ", " #b ") failed: both '" << _a << "' @ " \
          << __FILE__ << ":" << __LINE__; \
      throw std::runtime_error(_os.str()); \
    } \
  } while (0)

#define ASSERT_LT(a, b) \
  do { \
    auto&& _a = (a); \
    auto&& _b = (b); \
    if (!(_a < _b)) { \
      std::ostringstream _os; \
      _os << "ASSERT_LT(" #a ", " #b ") failed: " << _a << " >= " << _b \
          << " @ " << __FILE__ << ":" << __LINE__; \
      throw std::runtime_error(_os.str()); \
    } \
  } while (0)

#define ASSERT_CONTAINS(hay, needle) \
  do { \
    std::string _h = (hay); \
    std::string _n = (needle); \
    if (_h.find(_n) == std::string::npos) { \
      std::ostringstream _os; \
      _os << "ASSERT_CONTAINS failed: '" << _n << "' not found in first "; \
      _os << "500 bytes of: '" << _h.substr(0, 500) << "' @ " << __FILE__ \
          << ":" << __LINE__; \
      throw std::runtime_error(_os.str()); \
    } \
  } while (0)

#define ASSERT_NOT_CONTAINS(hay, needle) \
  do { \
    std::string _h = (hay); \
    std::string _n = (needle); \
    if (_h.find(_n) != std::string::npos) { \
      std::ostringstream _os; \
      _os << "ASSERT_NOT_CONTAINS failed: '" << _n << "' found in output @ " \
          << __FILE__ << ":" << __LINE__; \
      throw std::runtime_error(_os.str()); \
    } \
  } while (0)

// =============================================================================
//  4) 测试辅助
// =============================================================================
namespace tutil {

struct TempDir {
  fs::path path;
  TempDir() {
    std::string tmpl = "/tmp/grep_test_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* p = ::mkdtemp(buf.data());
    if (!p) throw std::runtime_error("mkdtemp failed");
    path = p;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

inline std::vector<std::unique_ptr<TempDir> >& tempdir_pool() {
  static std::vector<std::unique_ptr<TempDir> > v;
  return v;
}

inline TempDir& make_temp_dir() {
  tempdir_pool().push_back(std::make_unique<TempDir>());
  return *tempdir_pool().back();
}

inline void write_file(const fs::path& p, std::string_view content) {
  std::ofstream ofs(p, std::ios::binary);
  if (!ofs) throw std::runtime_error("write_file open: " + p.string());
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!ofs) throw std::runtime_error("write_file write: " + p.string());
}

inline std::string read_file(const fs::path& p) {
  std::ifstream ifs(p, std::ios::binary);
  if (!ifs) throw std::runtime_error("read_file: " + p.string());
  std::ostringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

inline std::size_t count_lines(const std::string& s) {
  if (s.empty()) return 0;
  std::size_t n = 0;
  for (char c : s)
    if (c == '\n') ++n;
  // trailing newline does not add an extra line
  if (s.back() != '\n') ++n;
  return n;
}

inline bool rg_available() {
  return std::system("rg --version > /dev/null 2>&1") == 0;
}

}  // namespace tutil

// =============================================================================
//  5) 测试用例
// =============================================================================

using namespace tutil;

// -------- 5.1 基本匹配 --------

TEST(basic_match) {
  auto& td = make_temp_dir();
  write_file(td.path / "foo.txt", "hello\nworld\nhello again\n");
  write_file(td.path / "bar.txt", "no match here\n");

  GrepTool t(td.path.string(), /*inline*/ 250, /*max_line_bytes*/ 4096);
  GrepParams p;
  p.pattern = "hello";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 2u);
  ASSERT_EQ(r.returned_matches, 2u);
  ASSERT_FALSE(r.truncated);
  ASSERT_TRUE(r.saved_path.empty());
  ASSERT_CONTAINS(r.output, "foo.txt:1:hello");
  ASSERT_CONTAINS(r.output, "foo.txt:3:hello again");
  ASSERT_NOT_CONTAINS(r.output, "bar.txt");
}

TEST(no_match) {
  auto& td = make_temp_dir();
  write_file(td.path / "a.txt", "alpha beta\n");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "xyz_does_not_exist";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 0u);
  ASSERT_EQ(r.returned_matches, 0u);
  ASSERT_FALSE(r.truncated);
  ASSERT_TRUE(r.output.empty());
}

TEST(output_format_path_line_content) {
  auto& td = make_temp_dir();
  write_file(td.path / "x.txt", "line one\nneedle here\nline three\n");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "needle";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_CONTAINS(r.output, "x.txt:2:needle here");
}

TEST(grouping_by_file) {
  auto& td = make_temp_dir();
  write_file(td.path / "a.txt", "hit1\nhit2\n");
  write_file(td.path / "b.txt", "hit3\nhit4\n");
  write_file(td.path / "c.txt", "hit5\nhit6\n");

  GrepTool t(td.path.string());  // 默认 sort_by_path = false，仍按文件分组
  GrepParams p;
  p.pattern = "hit";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 6u);

  // 同文件的行必须连续出现
  auto idx_a1 = r.output.find("a.txt:1:hit1");
  auto idx_a2 = r.output.find("a.txt:2:hit2");
  auto idx_b1 = r.output.find("b.txt:1:hit3");
  ASSERT_TRUE(idx_a1 != std::string::npos);
  ASSERT_TRUE(idx_a2 != std::string::npos);
  ASSERT_TRUE(idx_b1 != std::string::npos);
  ASSERT_LT(idx_a1, idx_a2);
  ASSERT_LT(idx_a2, idx_b1);
}

// -------- 5.2 参数校验 --------

TEST(invalid_pattern_empty) {
  auto& td = make_temp_dir();
  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "";
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "pattern");
}

TEST(invalid_pattern_nul) {
  auto& td = make_temp_dir();
  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = std::string("a\0b", 3);
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "NUL");
}

TEST(invalid_workspace_empty) {
  GrepTool t("");
  GrepParams p;
  p.pattern = "x";
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "workspace");
}

TEST(invalid_workspace_not_dir) {
  auto& td = make_temp_dir();
  auto filepath = td.path / "notadir.txt";
  write_file(filepath, "x\n");

  GrepTool t(filepath.string());
  GrepParams p;
  p.pattern = "x";
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "not a directory");
}

TEST(invalid_path_escape) {
  auto& td = make_temp_dir();
  GrepTool t(td.path.string());

  {
    GrepParams p;
    p.pattern = "x";
    p.path = "../etc";
    auto r = t.run(p);
    ASSERT_FALSE(r.ok);
    ASSERT_CONTAINS(r.error, "escapes the session workspace");
  }
  {
    GrepParams p;
    p.pattern = "x";
    p.path = "/etc";
    auto r = t.run(p);
    ASSERT_FALSE(r.ok);
    ASSERT_CONTAINS(r.error, "escapes the session workspace");
  }
  {
    GrepParams p;
    p.pattern = "x";
    p.path = "sub/../../etc";
    auto r = t.run(p);
    ASSERT_FALSE(r.ok);
    ASSERT_CONTAINS(r.error, "escapes the session workspace");
  }
}

TEST(invalid_path_not_exist) {
  auto& td = make_temp_dir();
  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "x";
  p.path = "does_not_exist";
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "does not exist");
}

TEST(invalid_path_nul) {
  auto& td = make_temp_dir();
  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "x";
  p.path = std::string("a\0b", 3);
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "NUL");
}

TEST(invalid_include_nul) {
  auto& td = make_temp_dir();
  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "x";
  p.include = std::string("*.cp\0p", 6);
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "NUL");
}

// -------- 5.3 路径定位 --------

TEST(path_absolute_inside_workspace) {
  auto& td = make_temp_dir();
  write_file(td.path / "in.txt", "match\n");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "match";
  p.path = (td.path / "in.txt").string();
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 1u);
}

TEST(path_subdirectory) {
  auto& td = make_temp_dir();
  fs::create_directories(td.path / "sub");
  write_file(td.path / "sub" / "a.txt", "match\n");
  write_file(td.path / "b.txt", "match\n");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "match";
  p.path = "sub";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 1u);
  ASSERT_CONTAINS(r.output, "sub/a.txt:1:match");
  ASSERT_NOT_CONTAINS(r.output, "b.txt");
}

TEST(path_dot_equals_default) {
  auto& td = make_temp_dir();
  write_file(td.path / "a.txt", "x\n");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "x";
  p.path = ".";
  auto r = t.run(p);
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 1u);
}

TEST(symlink_escape_rejected) {
  auto& td = make_temp_dir();
  fs::create_symlink("/etc", td.path / "link_etc");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "root";
  p.path = "link_etc/passwd";
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "escapes the session workspace");
}

// -------- 5.4 include glob --------

TEST(include_glob_simple) {
  auto& td = make_temp_dir();
  write_file(td.path / "a.cpp", "needle\n");
  write_file(td.path / "b.txt", "needle\n");
  fs::create_directories(td.path / "sub");
  write_file(td.path / "sub" / "c.cpp", "needle\n");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "needle";
  p.include = "*.cpp";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 2u);
  ASSERT_CONTAINS(r.output, "a.cpp");
  ASSERT_CONTAINS(r.output, "sub/c.cpp");
  ASSERT_NOT_CONTAINS(r.output, "b.txt");
}

TEST(include_glob_brace) {
  auto& td = make_temp_dir();
  write_file(td.path / "a.js", "k\n");
  write_file(td.path / "b.jsx", "k\n");
  write_file(td.path / "c.ts", "k\n");
  write_file(td.path / "d.txt", "k\n");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "k";
  p.include = "*.{js,jsx}";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 2u);
  ASSERT_CONTAINS(r.output, "a.js");
  ASSERT_CONTAINS(r.output, "b.jsx");
  ASSERT_NOT_CONTAINS(r.output, "c.ts");
  ASSERT_NOT_CONTAINS(r.output, "d.txt");
}

TEST(include_negation_rejected) {
  auto& td = make_temp_dir();
  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "x";
  p.include = "!*.txt";
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "negation");
}

TEST(include_dotdot_rejected) {
  auto& td = make_temp_dir();
  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "x";
  p.include = "../*.txt";
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "..");
}

// 关键回归测试：验证 rg 的 cwd 被设为 workspace，而不是继承父进程。
// 如果 cwd 未设置，rg 找不到相对路径 "sub"，会失败或返回空。
TEST(include_vs_workspace_cwd) {
  auto& td = make_temp_dir();
  fs::create_directories(td.path / "sub");
  write_file(td.path / "sub" / "a.cpp", "KEY\n");
  write_file(td.path / "b.cpp", "KEY\n");

  GrepTool t(td.path.string());
  GrepParams p;
  p.pattern = "KEY";
  p.include = "*.cpp";
  p.path = "sub";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 1u);
  ASSERT_CONTAINS(r.output, "sub/a.cpp:1:KEY");
  ASSERT_NOT_CONTAINS(r.output, "b.cpp");
}

// -------- 5.5 truncation / spill --------

TEST(truncation_saved_to_file) {
  auto& td = make_temp_dir();
  std::string big;
  big.reserve(300 * 16);
  for (int i = 0; i < 300; ++i) {
    big += "match line " + std::to_string(i) + "\n";
  }
  write_file(td.path / "big.txt", big);

  GrepTool t(td.path.string(), /*inline*/ 250, /*max_line_bytes*/ 4096);
  GrepParams p;
  p.pattern = "match";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 300u);
  ASSERT_EQ(r.returned_matches, 250u);
  ASSERT_TRUE(r.truncated);
  ASSERT_TRUE(!r.saved_path.empty());
  ASSERT_TRUE(fs::exists(r.saved_path));

  // spill 文件应包含全部 300 行
  std::string spilled = read_file(r.saved_path);
  ASSERT_EQ(count_lines(spilled), 300u);

  // 内联输出应只显示 250 行 + 尾部提示
  ASSERT_CONTAINS(r.output, "[truncated] showing first 250 of 300");
  ASSERT_CONTAINS(r.output, "complete list saved to");
  ASSERT_CONTAINS(r.output, r.saved_path);

  fs::remove(r.saved_path);
}

TEST(spill_size_limit_hit) {
  auto& td = make_temp_dir();
  std::string big;
  for (int i = 0; i < 500; ++i) {
    big += "match line " + std::to_string(i) + "\n";
  }
  write_file(td.path / "big.txt", big);

  GrepOptions opts;
  opts.max_spill_bytes = 200;  // 非常小，肯定溢出
  GrepTool t(td.path.string(), 10, 4096, opts);
  GrepParams p;
  p.pattern = "match";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_TRUE(r.truncated);
  ASSERT_CONTAINS(r.warning, "size limit");
  ASSERT_TRUE(!r.saved_path.empty());
  // 因为 size 上限极小，spill 文件里的行数应远小于 500
  std::size_t saved_lines = count_lines(read_file(r.saved_path));
  ASSERT_LT(saved_lines, 500u);

  fs::remove(r.saved_path);
}

TEST(inline_bytes_limit) {
  auto& td = make_temp_dir();
  // 200 行较长的匹配，每行 ~100 字节，inline_bytes_limit 限制在 500 字节
  std::string content;
  for (int i = 0; i < 200; ++i) {
    content += "match_" + std::to_string(i) + std::string(90, 'x') + "\n";
  }
  write_file(td.path / "big.txt", content);

  GrepOptions opts;
  opts.inline_bytes_limit = 500;
  GrepTool t(td.path.string(), 250, 4096, opts);
  GrepParams p;
  p.pattern = "match";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_TRUE(r.truncated);
  ASSERT_TRUE(r.returned_matches < 250u);
  ASSERT_TRUE(r.returned_matches >= 1u);

  if (!r.saved_path.empty()) fs::remove(r.saved_path);
}

TEST(line_max_columns_truncates) {
  auto& td = make_temp_dir();
  std::string long_line = "KEY:" + std::string(500, 'z');
  write_file(td.path / "long.txt", long_line + "\n");

  GrepTool t(td.path.string(), /*inline*/ 250, /*max_line_bytes*/ 100);
  GrepParams p;
  p.pattern = "KEY";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 1u);
  // 输出应被 --max-columns 截断，长度远小于原始 504 字节
  ASSERT_LT(r.output.size(), long_line.size());
  ASSERT_CONTAINS(r.output, "KEY");
}

// -------- 5.6 sort_by_path 模式 --------

TEST(sort_by_path_true_strict_order) {
  auto& td = make_temp_dir();
  write_file(td.path / "z.txt", "hit\n");
  write_file(td.path / "a.txt", "hit\n");
  write_file(td.path / "m.txt", "hit\n");

  GrepOptions opts;
  opts.sort_by_path = true;
  GrepTool t(td.path.string(), 250, 4096, opts);
  GrepParams p;
  p.pattern = "hit";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 3u);

  auto ia = r.output.find("a.txt:");
  auto im = r.output.find("m.txt:");
  auto iz = r.output.find("z.txt:");
  ASSERT_TRUE(ia != std::string::npos);
  ASSERT_TRUE(im != std::string::npos);
  ASSERT_TRUE(iz != std::string::npos);
  ASSERT_LT(ia, im);
  ASSERT_LT(im, iz);
}

TEST(sort_by_path_false_grouped_only) {
  auto& td = make_temp_dir();
  write_file(td.path / "z.txt", "hit1\nhit2\n");
  write_file(td.path / "a.txt", "hit3\nhit4\n");

  GrepOptions opts;
  opts.sort_by_path = false;  // -j1 模式
  GrepTool t(td.path.string(), 250, 4096, opts);
  GrepParams p;
  p.pattern = "hit";
  auto r = t.run(p);

  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.total_matches, 4u);

  // 同文件的行必须连续
  auto h1 = r.output.find("hit1");
  auto h2 = r.output.find("hit2");
  auto h3 = r.output.find("hit3");
  auto h4 = r.output.find("hit4");
  ASSERT_TRUE(h1 != std::string::npos && h2 != std::string::npos);
  ASSERT_TRUE(h3 != std::string::npos && h4 != std::string::npos);
  // 同文件的相邻，跨文件的至少隔开
  ASSERT_LT(h1, h2);
  ASSERT_LT(h3, h4);
  bool grouped = (h2 < h3) || (h4 < h1);
  ASSERT_TRUE(grouped);
}

// -------- 5.7 错误路径 --------

TEST(rg_not_found) {
  auto& td = make_temp_dir();
  GrepOptions opts;
  opts.rg_path = "/nonexistent/rg_binary_xyz";
  GrepTool t(td.path.string(), 250, 4096, opts);
  GrepParams p;
  p.pattern = "x";
  auto r = t.run(p);

  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "rg");
}

TEST(invalid_timeout) {
  auto& td = make_temp_dir();
  GrepOptions opts;
  opts.timeout = std::chrono::milliseconds{-1};
  GrepTool t(td.path.string(), 250, 4096, opts);
  GrepParams p;
  p.pattern = "x";
  auto r = t.run(p);
  ASSERT_FALSE(r.ok);
  ASSERT_CONTAINS(r.error, "timeout");
}

// -------- 5.8 超时（尽力而为）--------
// 用较大输入 + 1ms 超时，正常机器一定超时。
// 若机器极快跑完，测试也能通过（只验证返回结构自洽）。
TEST(timeout_partial) {
  auto& td = make_temp_dir();
  std::string big;
  big.reserve(10 * 1024 * 1024);
  for (int i = 0; i < 200000; ++i) {
    big += "abcdefghij klmnopqrst uvwxyz line " + std::to_string(i) + "\n";
  }
  write_file(td.path / "huge.txt", big);

  GrepOptions opts;
  opts.timeout = std::chrono::milliseconds{1};
  GrepTool t(td.path.string(), 250, 4096, opts);
  GrepParams p;
  p.pattern = "abcdefghij";
  auto r = t.run(p);

  if (r.ok && !r.truncated) {
    // 没超时：极端快机器，跑完了全部 20 万行。也接受，但检查统计一致。
    ASSERT_EQ(r.total_matches, 200000u);
  } else {
    // 超时分支：必须有 timeout 提示，且 truncated 为 true
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.truncated);
    ASSERT_CONTAINS(r.warning, "timed out");
  }
}

// =============================================================================
//  6) main
// =============================================================================
int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-v") == 0 ||
        std::strcmp(argv[i], "--verbose") == 0) {
      tlog::enabled = true;
    }
  }

  if (!tutil::rg_available()) {
    std::fprintf(
        stderr,
        "ERROR: 'rg' (ripgrep) not found in PATH.\n"
        "       Install ripgrep, e.g.: apt install ripgrep / brew install ripgrep\n");
    return 2;
  }

  std::printf("Running %zu test(s)...\n\n", testing::registry().size());

  int passed = 0, failed = 0;
  std::vector<std::string> fail_names;

  for (auto& tc : testing::registry()) {
    std::printf("[RUN ] %s\n", tc.name.c_str());
    std::fflush(stdout);
    try {
      tc.fn();
      ++passed;
      std::printf("[ OK ] %s\n", tc.name.c_str());
    } catch (const std::exception& e) {
      ++failed;
      fail_names.push_back(tc.name);
      std::printf("[FAIL] %s\n       %s\n", tc.name.c_str(), e.what());
    }
    std::fflush(stdout);
  }

  std::printf("\n=========================================\n");
  std::printf("  %d passed, %d failed (total %d)\n", passed, failed,
              passed + failed);
  if (failed > 0) {
    std::printf("  Failed tests:\n");
    for (auto& n : fail_names) std::printf("    - %s\n", n.c_str());
    std::printf("=========================================\n");
    return 1;
  }
  std::printf("=========================================\n");
  return 0;
}
#endif