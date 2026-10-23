#include "helpers/function.h"
#include <iostream>

// ============================================================
// 极简测试框架
// ============================================================
static int g_pass = 0;
static int g_fail = 0;

namespace fs = std::filesystem;

struct TestEntry { std::string name; void (*fn)(); };
static std::vector<TestEntry>& tests() { static std::vector<TestEntry> t; return t; }

#define TEST(name)                                                    \
static void name();                                               \
    static struct Reg_##name {                                        \
      Reg_##name() { tests().push_back({#name, name}); }            \
} reg_##name;                                                     \
    static void name()

#define CHECK(cond) do {                                              \
    if (cond) { ++g_pass; }                                           \
    else {                                                            \
      ++g_fail;                                                     \
      std::cout << "  [FAIL] " << #cond                            \
      << "  (line " << __LINE__ << ")\n";                 \
}                                                                 \
} while (0)

#define CHECK_EQ(a, b) do {                                           \
  auto _a = (a); auto _b = (b);                                     \
    if (_a == _b) { ++g_pass; }                                       \
    else {                                                            \
      ++g_fail;                                                     \
      std::cout << "  [FAIL] " << #a << " == " << #b               \
      << "  (line " << __LINE__ << ")"                    \
      << "  got=[" << _a << "] want=[" << _b << "]\n";    \
}                                                                 \
} while (0)

  // ============================================================
  // 工具：切 CWD
  // ============================================================
  struct CwdGuard {
    fs::path old;
    CwdGuard() : old(fs::current_path()) {}
    ~CwdGuard() { std::error_code ec; fs::current_path(old, ec); }
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
  write_file(base / "sub" / "d.cpp",
             "int foo = 1;\nint bar = 2;\n");

          // sub/e.js: 1 处 foo
  write_file(base / "sub" / "e.js",
             "let x = foo;\n");

          // empty.txt
  write_file(base / "empty.txt", "");

          // crlf.txt: CRLF 行尾，2 处 foo
  write_file(base / "crlf.txt", "foo\r\nbar\r\nfoo\r\n");

          // nonl.txt: 最后一行无换行符，2 处 foo
  write_file(base / "nonl.txt", "foo\nbar\nfoo");

          // big.txt: 300 处 foo（用于截断测试）
  {
    std::string s;
    for (int i = 0; i < 300; ++i)
      s += "line" + std::to_string(i) + " foo\n";
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
             "\xe4\xbd\xa0\xe5\xa5\xbd foo\n"              // 你好 foo
             "no match\n"
             "world foo \xe4\xb8\x96\xe7\x95\x8c\n");     // world foo 世界

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
  CHECK(r.content.find("1:foo one")   != std::string::npos);
  CHECK(r.content.find("3:foo two")   != std::string::npos);
  CHECK(r.content.find("5:foo three") != std::string::npos);
  CHECK(r.content.find("共 3 处匹配") != std::string::npos);
}

TEST(test_no_match) {
  auto r = kw::grep("zzzz_nothing", (g_fixture / "a.txt").string());
  CHECK(r.ok);                     // 退出码 1 视为成功
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
  // 期望：a(3)+b(2)+d.cpp(1)+e.js(1)+crlf(2)+nonl(2)+big(300)+long(1)+utf8(2)+space(1) = 315
  // 若夹具变更，此数字需同步更新。
  CwdGuard g; fs::current_path(g_fixture);
  auto r = kw::grep("foo", ".", "", 10000);   // 关掉截断以便校验总数
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
  CwdGuard g; fs::current_path(g_fixture);
  auto r = kw::grep("foo", ".", "*.cpp", 10000);
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 1u);
  CHECK(r.content.find("d.cpp") != std::string::npos);
  CHECK(r.content.find("a.txt") == std::string::npos);
}

TEST(test_include_glob_braces) {
  // txt(3+2+2+2+300+1+2+1=313) + cpp(1) = 314
  CwdGuard g; fs::current_path(g_fixture);
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
  CHECK(r.error.size() > 5);       // 不能再是空的 "rg exited with code 2"
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
  CHECK(r.content.find('\r') == std::string::npos);   // 无残留 \r
  CHECK(r.content.find("1:foo") != std::string::npos);
  CHECK(r.content.find("3:foo") != std::string::npos);
}

TEST(test_no_trailing_newline) {
  auto r = kw::grep("foo", (g_fixture / "nonl.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 2u);   // 最后一行虽无 \n 也应命中
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
  CHECK(r.content.find("你好")   != std::string::npos);
  CHECK(r.content.find("世界")   != std::string::npos);
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
  CHECK(r.ok);                      // 命令能正常起，无引号错乱
  CHECK_EQ(r.totalMatches, 0u);
}

TEST(test_pattern_alternation) {
  auto r = kw::grep("foo|bar", (g_fixture / "a.txt").string());
  CHECK(r.ok);
  CHECK_EQ(r.totalMatches, 4u);     // 3×foo + 1×bar
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
        r.content.find("[...]")   != std::string::npos);
}

// ---------- 8. 确定性 / 默认路径 ----------

TEST(test_deterministic_output) {
  CwdGuard g; fs::current_path(g_fixture);
  auto r1 = kw::grep("foo", ".", "*.txt", 10000);
  auto r2 = kw::grep("foo", ".", "*.txt", 10000);
  CHECK_EQ(r1.content, r2.content);   // --sort path 保证顺序一致
}

TEST(test_default_path_is_cwd) {
  CwdGuard g; fs::current_path(g_fixture);
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