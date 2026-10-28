

#if defined(_WIN32)
// ============================================================================
//  ==========================  测试框架  ====================================
// ============================================================================

#include <iostream>

#include "helpers/function.h"

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_TRUE(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "    FAIL: " #expr " @line " << __LINE__ << "\n"; \
      ++g_fail; \
    } else { \
      ++g_pass; \
    } \
  } while (0)

#define EXPECT_EQ(a, b) \
  do { \
    auto _va = (a); \
    auto _vb = (b); \
    if (!(_va == _vb)) { \
      std::cerr << "    FAIL: " #a " == " #b " @line " << __LINE__ << "\n"; \
      std::cerr << "      left  = [" << _va << "]\n"; \
      std::cerr << "      right = [" << _vb << "]\n"; \
      ++g_fail; \
    } else { \
      ++g_pass; \
    } \
  } while (0)

#define EXPECT_CONTAINS(hay, needle) \
  do { \
    std::string _h = (hay), _n = (needle); \
    if (_h.find(_n) == std::string::npos) { \
      std::cerr << "    FAIL: not contains \"" << _n << "\" @line " \
                << __LINE__ << "\n"; \
      std::cerr << "      actual = [" << _h << "]\n"; \
      ++g_fail; \
    } else { \
      ++g_pass; \
    } \
  } while (0)

#define EXPECT_NOT_CONTAINS(hay, needle) \
  do { \
    std::string _h = (hay), _n = (needle); \
    if (_h.find(_n) != std::string::npos) { \
      std::cerr << "    FAIL: unexpectedly contains \"" << _n << "\" @line " \
                << __LINE__ << "\n"; \
      ++g_fail; \
    } else { \
      ++g_pass; \
    } \
  } while (0)

#define RUN_TEST(fn) \
  do { \
    std::cout << "[ RUN  ] " << #fn << "\n"; \
    int _before = g_fail; \
    fn(); \
    std::cout << (g_fail == _before ? "[  OK  ] " : "[ FAIL ] ") << #fn \
              << "\n"; \
  } while (0)

// ============================================================================
//  ==========================  测试辅助  ====================================
// ============================================================================

std::string WideToUtf8Str(const std::wstring& w) {
  if (w.empty()) return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr,
                                 0, nullptr, nullptr);
  std::string s(need, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), need,
                      nullptr, nullptr);
  return s;
}

std::wstring TempDirW() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, buf);
  return std::wstring(buf, n);
}

// tag 是 UTF-8，可以被测试用来验证非 ASCII 路径转换
std::string MakeTempFile(const std::string& tag_utf8,
                         const std::string& content) {
  static int counter = 0;
  std::wstring wtag;
  if (!kw::PathToWide(tag_utf8, wtag)) return {};

  wchar_t name[256];
  swprintf_s(name, L"frl_%ls_%d_%lu.tmp", wtag.c_str(), ++counter,
             GetCurrentProcessId());

  std::wstring full = TempDirW() + name;
  HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    std::cerr << "    setup: cannot create temp file (err=" << GetLastError()
              << ")\n";
    return {};
  }
  if (!content.empty()) {
    DWORD written = 0;
    WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
  }
  CloseHandle(h);
  return WideToUtf8Str(full);
}

void RemoveTemp(const std::string& utf8path) {
  std::wstring w;
  if (kw::PathToWide(utf8path, w)) DeleteFileW(w.c_str());
}

// 生成 N 行 "line N\n"
std::string MakeLines(int n, const char* prefix = "line") {
  std::string s;
  for (int i = 1; i <= n; ++i) {
    s += prefix;
    s += ' ';
    s += std::to_string(i);
    s += '\n';
  }
  return s;
}

// 数一个字符串里有多少行（'\n' 计数）
int CountLines(const std::string& s) {
  int n = 0;
  for (char c : s)
    if (c == '\n') ++n;
  return n;
}

// ============================================================================
//  ==========================  测试用例  ====================================
// ============================================================================

// ---- 1. 基础范围读取 ----
void test_basic_range() {
  std::string p = MakeTempFile("basic", MakeLines(10));
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 3, 5, out));
  EXPECT_EQ(out, std::string("3: line 3\n4: line 4\n5: line 5\n"));
  RemoveTemp(p);
}

// ---- 2. 只读第 1 行 ----
void test_first_line() {
  std::string p = MakeTempFile("first", MakeLines(10));
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 1, out));
  EXPECT_EQ(out, std::string("1: line 1\n"));
  RemoveTemp(p);
}

// ---- 3. 范围跨越 EOF（start 在文件内、end 超出） ----
void test_range_crossing_eof() {
  std::string p = MakeTempFile("cross", MakeLines(10));
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 8, 15, out));
  EXPECT_EQ(out, std::string("8: line 8\n9: line 9\n10: line 10\n"));
  RemoveTemp(p);
}

// ---- 4. start 完全超出 EOF → 错误 + 行数提示 ----
void test_start_beyond_eof() {
  std::string p = MakeTempFile("beyond", MakeLines(10));
  std::string out;
  EXPECT_TRUE(!kw::file_read_line(p, 100, 200, out));
  EXPECT_CONTAINS(out, "contains no content");
  EXPECT_CONTAINS(out, "file has only 10 line(s)");
  RemoveTemp(p);
}

// ---- 5. 默认 end（start + 2000） ----
void test_default_end() {
  std::string p = MakeTempFile("defend", MakeLines(5000, "L"));
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 0, out));  // end=0 → start+2000=2001
  EXPECT_CONTAINS(out, "1: L 1\n");
  EXPECT_CONTAINS(out, "2001: L 2001\n");
  EXPECT_NOT_CONTAINS(out, "2002: L 2002\n");
  EXPECT_EQ(CountLines(out), 2001);

  out.clear();
  EXPECT_TRUE(kw::file_read_line(p, 100, 0, out));  // 100..2100
  EXPECT_CONTAINS(out, "100: L 100\n");
  EXPECT_CONTAINS(out, "2100: L 2100\n");
  EXPECT_NOT_CONTAINS(out, "2101: L 2101\n");
  EXPECT_EQ(CountLines(out), 2001);
  RemoveTemp(p);
}

// ---- 6. 空文件 → 错误 ----
void test_empty_file() {
  std::string p = MakeTempFile("empty", "");
  std::string out;
  EXPECT_TRUE(!kw::file_read_line(p, 1, 10, out));
  EXPECT_CONTAINS(out, "contains no content");
  RemoveTemp(p);
}

// ---- 7. 文件末尾无换行符 ----
void test_no_trailing_newline() {
  std::string p = MakeTempFile("notrail", "a\nb\nc");  // 3 行，最后一行无 \n
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 3, out));
  EXPECT_EQ(out, std::string("1: a\n2: b\n3: c\n"));

  out.clear();
  EXPECT_TRUE(kw::file_read_line(p, 3, 3, out));
  EXPECT_EQ(out, std::string("3: c\n"));
  RemoveTemp(p);
}

// ---- 8. CRLF 行尾 ----
void test_crlf() {
  std::string p = MakeTempFile("crlf", "a\r\nb\r\nc\r\n");
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 3, out));
  EXPECT_EQ(out, std::string("1: a\n2: b\n3: c\n"));
  RemoveTemp(p);
}

// ---- 9. 空行保留 ----
void test_empty_lines() {
  std::string p = MakeTempFile("emptylines", "a\n\nb\n\n\nc\n");
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 6, out));
  EXPECT_EQ(out, std::string("1: a\n2: \n3: b\n4: \n5: \n6: c\n"));
  RemoveTemp(p);
}

// ---- 10. UTF-8 BOM 跳过 ----
void test_utf8_bom() {
  std::string content;
  content += '\xEF';
  content += '\xBB';
  content += '\xBF';
  content += "hello\nworld\n";
  std::string p = MakeTempFile("bom8", content);
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 2, out));
  EXPECT_EQ(out, std::string("1: hello\n2: world\n"));  // 无 BOM 残留
  RemoveTemp(p);
}

// ---- 11. UTF-16LE BOM → 拒绝 ----
void test_utf16le_rejected() {
  std::string content;
  content += '\xFF';
  content += '\xFE';
  content += 'h';
  content += '\0';
  content += 'i';
  content += '\0';
  std::string p = MakeTempFile("u16le", content);
  std::string out;
  EXPECT_TRUE(!kw::file_read_line(p, 1, 1, out));
  EXPECT_CONTAINS(out, "UTF-16LE");
  RemoveTemp(p);
}

// ---- 12. UTF-16BE BOM → 拒绝 ----
void test_utf16be_rejected() {
  std::string content;
  content += '\xFE';
  content += '\xFF';
  content += '\0';
  content += 'h';
  std::string p = MakeTempFile("u16be", content);
  std::string out;
  EXPECT_TRUE(!kw::file_read_line(p, 1, 1, out));
  EXPECT_CONTAINS(out, "UTF-16BE");
  RemoveTemp(p);
}

// ---- 13. UTF-8 多字节内容原样保留 ----
void test_utf8_content() {
  std::string content = u8"你好世界\n";
  content += u8"emoji: \xF0\x9F\x98\x80\n";  // 😀
  content += u8"中文行 3\n";
  std::string p = MakeTempFile("u8", content);
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 3, out));
  EXPECT_EQ(out, std::string("1: ") + u8"你好世界\n" + std::string("2: ") +
                     u8"emoji: \xF0\x9F\x98\x80\n" + std::string("3: ") +
                     u8"中文行 3\n");
  RemoveTemp(p);
}

// ---- 14. 非 ASCII 路径 ----
void test_non_ascii_path() {
  std::string p = MakeTempFile(u8"中文目录", "hello\nworld\n");
  if (p.empty()) {
    std::cerr << "    skip: cannot create non-ascii path\n";
    return;
  }
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 2, out));
  EXPECT_EQ(out, std::string("1: hello\n2: world\n"));
  RemoveTemp(p);
}

// ---- 15. 文件不存在 ----
void test_file_not_found() {
  std::string out;
  EXPECT_TRUE(
      !kw::file_read_line("Z:\\__definitely_not_exists__.txt", 1, 5, out));
  EXPECT_CONTAINS(out, "cannot open");
  RemoveTemp("Z:\\__definitely_not_exists__.txt");
}

// ---- 16. 空路径 ----
void test_empty_path() {
  std::string out;
  EXPECT_TRUE(!kw::file_read_line("", 1, 5, out));
  EXPECT_CONTAINS(out, "file_path is empty");
}

// ---- 17. start < 1 ----
void test_start_zero() {
  std::string out;
  EXPECT_TRUE(!kw::file_read_line("some.txt", 0, 5, out));
  EXPECT_CONTAINS(out, "start must be >= 1");
}

// ---- 18. end < start ----
void test_end_lt_start() {
  std::string p = MakeTempFile("endlt", MakeLines(10));
  std::string out;
  EXPECT_TRUE(!kw::file_read_line(p, 5, 3, out));
  EXPECT_CONTAINS(out, "must be >= start");
  RemoveTemp(p);
}

// ---- 19. 路径是目录 → 打开失败 ----
void test_directory_rejected() {
  std::string dir = WideToUtf8Str(TempDirW());
  std::string out;
  EXPECT_TRUE(!kw::file_read_line(dir, 1, 1, out));
  EXPECT_CONTAINS(out, "cannot open");
}

// ---- 20. 单行超长 → 行截断标记 ----
void test_long_line_truncated() {
  std::string content(2 * 1024 * 1024, 'x');  // 2MB 单行
  content += '\n';
  content += "tail\n";
  std::string p = MakeTempFile("longline", content);
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 1, out));
  EXPECT_CONTAINS(out, "...[line truncated]");
  // 第 1 行内容 ≈ 1MB + 行号 + 标记 + \n
  EXPECT_TRUE(out.size() <= kw::kMaxLineBytes + 64);
  // 第 2 行应该仍然可读
  out.clear();
  EXPECT_TRUE(kw::file_read_line(p, 2, 2, out));
  EXPECT_EQ(out, std::string("2: tail\n"));
  RemoveTemp(p);
}

// ---- 21. 恰好填满行上限（seg_len == room 边界） ----
void test_line_exactly_max() {
  // 构造恰好 kw::kMaxLineBytes 字节的一行，不触发截断
  std::string content(kw::kMaxLineBytes, 'y');
  content += '\n';
  std::string p = MakeTempFile("exact", content);
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 1, out));
  EXPECT_NOT_CONTAINS(out, "...[line truncated]");
  // 长度应是 kw::kMaxLineBytes + "1: " + '\n'
  EXPECT_EQ(out.size(), kw::kMaxLineBytes + 4);
  RemoveTemp(p);
}

// ---- 22. 空文件路径含前导/尾随空格（应当正常打开失败，不是崩溃） ----
void test_weird_path() {
  std::string out;
  EXPECT_TRUE(!kw::file_read_line("   ", 1, 1, out));
  // 只要不崩溃、不抛异常即可
}

// ---- 23. 跨越 64KB 缓冲区边界的长行（验证跨 ReadFile 拼接） ----
void test_line_across_buffer() {
  // 单行 200KB，跨越多个 64KB chunk，但没到 1MB 上限
  std::string content(200 * 1024, 'z');
  content += '\n';
  content += "next\n";
  std::string p = MakeTempFile("across", content);
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 1, out));
  EXPECT_NOT_CONTAINS(out, "...[line truncated]");
  EXPECT_EQ(out.size(), 200ull * 1024 + 4);  // "1: " + content + '\n'
  RemoveTemp(p);
}

// ---- 24. 范围 start=end=1 ----
void test_single_line_range() {
  std::string p = MakeTempFile("single", MakeLines(10));
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 1, out));
  EXPECT_EQ(out, "1: line 1\n");
  out.clear();
  EXPECT_TRUE(kw::file_read_line(p, 10, 10, out));
  EXPECT_EQ(out, "10: line 10\n");
  RemoveTemp(p);
}

// ---- 25. 大量行 + start 大：验证正确寻址 ----
void test_deep_line_range() {
  std::string p = MakeTempFile("deep", MakeLines(100000, "N"));
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 99998, 100000, out));
  EXPECT_CONTAINS(out, "99998: N 99998\n");
  EXPECT_CONTAINS(out, "99999: N 99999\n");
  EXPECT_CONTAINS(out, "100000: N 100000\n");
  EXPECT_EQ(CountLines(out), 3);
  RemoveTemp(p);
}

// ---- 26. 文件中只有换行符 ----
void test_all_newlines() {
  std::string p = MakeTempFile("nl", "\n\n\n");
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 3, out));
  EXPECT_EQ(out, std::string("1: \n2: \n3: \n"));
  RemoveTemp(p);
}

// ---- 27. \r 单独出现（旧 Mac 风格，不作为换行） ----
void test_lone_cr_not_newline() {
  // "a\rb\nc\n"：\r 不是换行符，只是行内的一个字节
  std::string p = MakeTempFile("lonecr", "a\rb\nc\n");
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 2, out));
  EXPECT_EQ(out, std::string("1: a\rb\n2: c\n"));
  RemoveTemp(p);
}

// ---- 28. 文件尾多余换行 ----
void test_trailing_newline_only() {
  std::string p = MakeTempFile("trailnl", "a\n");
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 1, out));
  EXPECT_EQ(out, "1: a\n");
  // 第 2 行应该不存在
  out.clear();
  EXPECT_TRUE(!kw::file_read_line(p, 2, 2, out));
  RemoveTemp(p);
}

// ============================================================================
//  ============================  main  ======================================
// ============================================================================

int main() {
  std::cout << "=== kw::file_read_line tests ===\n\n";

  RUN_TEST(test_basic_range);
  RUN_TEST(test_first_line);
  RUN_TEST(test_range_crossing_eof);
  RUN_TEST(test_start_beyond_eof);
  RUN_TEST(test_default_end);
  RUN_TEST(test_empty_file);
  RUN_TEST(test_no_trailing_newline);
  RUN_TEST(test_crlf);
  RUN_TEST(test_empty_lines);
  RUN_TEST(test_utf8_bom);
  RUN_TEST(test_utf16le_rejected);
  RUN_TEST(test_utf16be_rejected);
  RUN_TEST(test_utf8_content);
  RUN_TEST(test_non_ascii_path);
  RUN_TEST(test_file_not_found);
  RUN_TEST(test_empty_path);
  RUN_TEST(test_start_zero);
  RUN_TEST(test_end_lt_start);
  RUN_TEST(test_directory_rejected);
  RUN_TEST(test_long_line_truncated);
  RUN_TEST(test_line_exactly_max);
  RUN_TEST(test_weird_path);
  RUN_TEST(test_line_across_buffer);
  RUN_TEST(test_single_line_range);
  RUN_TEST(test_deep_line_range);
  RUN_TEST(test_all_newlines);
  RUN_TEST(test_lone_cr_not_newline);
  RUN_TEST(test_trailing_newline_only);

  std::cout << "\n=== summary ===\n";
  std::cout << "  pass: " << g_pass << "\n";
  std::cout << "  fail: " << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}

#elif defined(__linux__)

#include <iostream>

#include "helpers/log.h"

// ============================================================
// 被测试实现（与改进版一致）
// ============================================================
bool file_read_line(const std::string& file_path, int start, int end,
                    std::string& out, std::string& error,
                    int* actual_last = nullptr) {
  out.clear();
  error.clear();
  if (actual_last) *actual_last = 0;

  if (file_path.empty()) {
    error = "empty file path";
    LOG_ERROR("file_read_line: {}", error);
    return false;
  }
  if (start < 1) {
    error = "invalid start line: " + std::to_string(start);
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }
  if (end < start) {
    error = "invalid range: start=" + std::to_string(start) +
            ", end=" + std::to_string(end);
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }

  struct stat st{};
  if (::stat(file_path.c_str(), &st) != 0) {
    int e = errno;
    error =
        "stat failed: " + std::error_code(e, std::system_category()).message();
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    error = "not a regular file";
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }

  std::ifstream ifs(file_path, std::ios::in | std::ios::binary);
  if (!ifs) {
    int e = errno;
    error =
        "cannot open: " + std::error_code(e, std::system_category()).message();
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }

  constexpr std::size_t kMaxLineLen = 16ULL * 1024 * 1024;
  constexpr std::size_t kMaxTotalOut = 64ULL * 1024 * 1024;

  const std::size_t n_lines =
      static_cast<std::size_t>(end) - static_cast<std::size_t>(start) + 1;
  std::size_t est = (n_lines > kMaxTotalOut / 80) ? kMaxTotalOut : n_lines * 80;
  out.reserve(est);

  std::string line;
  int cur = 0;
  while (cur < end && std::getline(ifs, line)) {
    ++cur;
    if (cur < start) continue;

    if (line.size() > kMaxLineLen) {
      out.clear();
      error = "line " + std::to_string(cur) + " exceeds " +
              std::to_string(kMaxLineLen) + " bytes";
      LOG_ERROR("file_read_line({}): {}", file_path, error);
      return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::size_t need = std::to_string(cur).size() + 2 + line.size() + 1;
    if (out.size() + need > kMaxTotalOut) {
      out.clear();
      error = "output exceeds " + std::to_string(kMaxTotalOut) + " bytes";
      LOG_ERROR("file_read_line({}): {}", file_path, error);
      return false;
    }
    out.append(std::to_string(cur)).append(": ").append(line).append("\n");
  }

  if (ifs.bad() || (ifs.fail() && !ifs.eof())) {
    out.clear();
    error = "I/O error while reading '" + file_path + "'";
    LOG_ERROR("file_read_line: {}", error);
    return false;
  }

  if (cur < start) {
    out.clear();
    error = "range out of bounds: start=" + std::to_string(start) +
            ", total lines=" + std::to_string(cur) + ", file='" + file_path +
            "'";
    LOG_ERROR("file_read_line: {}", error);
    return false;
  }

  if (actual_last) *actual_last = cur;
  return true;
}

// ============================================================
// 最小测试框架
// ============================================================
namespace t {
struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& cases() {
  static std::vector<Case> v;
  return v;
}
bool& failed_flag() {
  static bool f = false;
  return f;
}

struct Reg {
  Reg(const char* n, void (*fn)()) { cases().push_back({n, fn}); }
};

void report_fail(const char* file, int line, const char* expr) {
  failed_flag() = true;
  std::cerr << "    FAIL " << file << ":" << line << ": " << expr << "\n";
}

template <class T>
std::string to_str(const T& v) {
  if constexpr (std::is_same_v<T, std::string>) {
    constexpr std::size_t kMax = 200;
    if (v.size() <= kMax) return "\"" + v + "\"";
    return "\"" + v.substr(0, kMax) +
           "...\" (size=" + std::to_string(v.size()) + ")";
  } else {
    std::ostringstream oss;
    oss << v;
    return oss.str();
  }
}

int run_all() {
  int passed = 0, failed = 0;
  for (auto& c : cases()) {
    failed_flag() = false;
    std::cout << "[RUN ] " << c.name << "\n";
    try {
      c.fn();
    } catch (const std::exception& e) {
      failed_flag() = true;
      std::cerr << "    EXCEPTION: " << e.what() << "\n";
    } catch (...) {
      failed_flag() = true;
      std::cerr << "    UNKNOWN EXCEPTION\n";
    }
    if (failed_flag()) {
      ++failed;
      std::cout << "[FAIL] " << c.name << "\n";
    } else {
      ++passed;
      std::cout << "[ OK ] " << c.name << "\n";
    }
  }
  std::cout << "\n==== " << passed << " passed, " << failed << " failed ====\n";
  return failed == 0 ? 0 : 1;
}
}  // namespace t

#define TEST(name) \
  static void name(); \
  static ::t::Reg name##_reg(#name, name); \
  static void name()

#define CHECK(cond) \
  do { \
    if (!(cond)) ::t::report_fail(__FILE__, __LINE__, #cond); \
  } while (0)
#define CHECK_EQ(a, b) \
  do { \
    auto&& _va = (a); \
    auto&& _vb = (b); \
    if (!(_va == _vb)) { \
      std::cerr << "    FAIL " << __FILE__ << ":" << __LINE__ \
                << ": " #a " == " #b " (" << ::t::to_str(_va) << " vs " \
                << ::t::to_str(_vb) << ")\n"; \
      ::t::failed_flag() = true; \
    } \
  } while (0)
#define CHECK_TRUE(c) CHECK(c)
#define CHECK_FALSE(c) CHECK(!(c))

// ============================================================
// 测试辅助
// ============================================================
struct TempFile {
  std::string path;
  explicit TempFile(const std::string& content) {
    static int counter = 0;
    auto base = std::filesystem::temp_directory_path();
    path = (base / ("frl_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter++)))
               .string();
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
};

struct TempDir {
  std::string path;
  TempDir() {
    static int counter = 0;
    auto base = std::filesystem::temp_directory_path();
    path = (base / ("frl_test_dir_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter++)))
               .string();
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

static bool skip_slow() {
  const char* v = std::getenv("FRL_SKIP_SLOW");
  return v && *v && std::strcmp(v, "0") != 0;
}

// ============================================================
// 1) 基本功能
// ============================================================
TEST(t_basic_range) {
  TempFile f("alpha\nbeta\ngamma\n");
  std::string out, err;
  int last = -1;
  bool ok = file_read_line(f.path, 1, 2, out, err, &last);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("1: alpha\n2: beta\n"));
  CHECK_EQ(err, std::string(""));
  CHECK_EQ(last, 2);
}

TEST(t_read_all) {
  TempFile f("a\nb\nc\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 3, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("1: a\n2: b\n3: c\n"));
}

TEST(t_middle_range) {
  std::string content;
  for (int i = 1; i <= 100; ++i) content += "L" + std::to_string(i) + "\n";
  TempFile f(content);
  std::string out, err;
  bool ok = file_read_line(f.path, 10, 12, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("10: L10\n11: L11\n12: L12\n"));
}

TEST(t_single_line) {
  TempFile f("one\ntwo\nthree\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 2, 2, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("2: two\n"));
}

TEST(t_single_line_no_newline) {
  TempFile f("only");
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 1, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("1: only\n"));
}

TEST(t_empty_line_preserved) {
  TempFile f("a\n\nc\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 3, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("1: a\n2: \n3: c\n"));
}

TEST(t_no_trailing_newline) {
  TempFile f("a\nb\nlast");
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 3, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("1: a\n2: b\n3: last\n"));
}

TEST(t_many_lines) {
  std::string content;
  for (int i = 1; i <= 1000; ++i) content += "row" + std::to_string(i) + "\n";
  TempFile f(content);
  std::string out, err;
  bool ok = file_read_line(f.path, 500, 502, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("500: row500\n501: row501\n502: row502\n"));
}

// ============================================================
// 2) 行尾 CRLF 与二进制字符
// ============================================================
TEST(t_crlf_stripped) {
  TempFile f("a\r\nb\r\nc\r\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 3, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("1: a\n2: b\n3: c\n"));
}

TEST(t_cr_in_middle_preserved) {
  // 只有行尾 \r 剥离；行内 \r 保留
  TempFile f("a\rb\r\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 1, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("1: a\rb\n"));
}

TEST(t_nul_byte_in_line) {
  std::string content = "ab";
  content.push_back('\0');
  content += "cd\n";
  TempFile f(content);
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 1, out, err);
  CHECK_TRUE(ok);
  std::string expected = "1: ab";
  expected.push_back('\0');
  expected += "cd\n";
  CHECK_EQ(out.size(), expected.size());
  CHECK(std::equal(out.begin(), out.end(), expected.begin()));
}

// ============================================================
// 3) 越界/区间边界
// ============================================================
TEST(t_start_beyond_eof) {
  TempFile f("a\nb\nc\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 10, 12, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
  CHECK_TRUE(err.find("out of bounds") != std::string::npos);
}

TEST(t_end_beyond_eof_partial) {
  TempFile f("a\nb\nc\n");
  std::string out, err;
  int last = -1;
  bool ok = file_read_line(f.path, 2, 100, out, err, &last);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("2: b\n3: c\n"));
  CHECK_EQ(last, 3);
  CHECK_EQ(err, std::string(""));
}

TEST(t_start_equals_total) {
  TempFile f("a\nb\nc\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 3, 3, out, err);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("3: c\n"));
}

TEST(t_empty_file) {
  TempFile f("");
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 1, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
  CHECK_TRUE(err.find("out of bounds") != std::string::npos);
}

// ============================================================
// 4) 入参校验
// ============================================================
TEST(t_empty_path) {
  std::string out, err;
  bool ok = file_read_line("", 1, 1, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
  CHECK_TRUE(err.find("empty") != std::string::npos);
}

TEST(t_start_zero) {
  TempFile f("a\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 0, 1, out, err);
  CHECK_FALSE(ok);
  CHECK_TRUE(err.find("start") != std::string::npos);
}

TEST(t_start_negative) {
  TempFile f("a\n");
  std::string out, err;
  bool ok = file_read_line(f.path, -5, 1, out, err);
  CHECK_FALSE(ok);
  CHECK_TRUE(err.find("start") != std::string::npos);
}

TEST(t_end_less_start) {
  TempFile f("a\n");
  std::string out, err;
  bool ok = file_read_line(f.path, 5, 3, out, err);
  CHECK_FALSE(ok);
  CHECK_TRUE(err.find("invalid") != std::string::npos);
}

// ============================================================
// 5) 文件系统相关
// ============================================================
TEST(t_nonexistent_file) {
  std::string out, err;
  bool ok =
      file_read_line("/tmp/frl_definitely_not_exist_12345", 1, 1, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
  CHECK_TRUE(err.find("stat failed") != std::string::npos);
}

TEST(t_directory_path) {
  TempDir d;
  std::string out, err;
  bool ok = file_read_line(d.path, 1, 1, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
  CHECK_TRUE(err.find("not a regular file") != std::string::npos);
}

TEST(t_symlink_to_file) {
  TempFile f("a\nb\n");
  std::string link = f.path + ".link";
  std::filesystem::create_symlink(f.path, link);
  std::string out, err;
  bool ok = file_read_line(link, 1, 2, out, err);
  std::error_code ec;
  std::filesystem::remove(link, ec);
  CHECK_TRUE(ok);
  CHECK_EQ(out, std::string("1: a\n2: b\n"));
}

// ============================================================
// 6) int 边界
// ============================================================
TEST(t_int_max_end) {
  TempFile f("a\nb\nc\n");
  std::string out, err;
  int last = -1;
  bool ok = file_read_line(f.path, 1, INT_MAX, out, err, &last);
  CHECK_TRUE(ok);
  CHECK_EQ(last, 3);
  CHECK_EQ(out, std::string("1: a\n2: b\n3: c\n"));
}

TEST(t_int_max_start) {
  TempFile f("a\nb\n");
  std::string out, err;
  bool ok = file_read_line(f.path, INT_MAX, INT_MAX, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
  CHECK_TRUE(err.find("out of bounds") != std::string::npos);
}

// ============================================================
// 7) 出参契约
// ============================================================
TEST(t_out_cleared_on_failure) {
  TempFile f("a\nb\nc\n");
  std::string out = "PRE-EXISTING";
  std::string err;
  bool ok = file_read_line(f.path, 10, 12, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
}

TEST(t_error_cleared_on_success) {
  TempFile f("a\n");
  std::string out;
  std::string err = "stale";
  bool ok = file_read_line(f.path, 1, 1, out, err);
  CHECK_TRUE(ok);
  CHECK(err.empty());
}

TEST(t_actual_last_reset_on_failure) {
  TempFile f("a\n");
  int last = 777;
  std::string out, err;
  bool ok = file_read_line(f.path, 5, 5, out, err, &last);
  CHECK_FALSE(ok);
  CHECK_EQ(last, 0);
}

// ============================================================
// 8) 大文件（慢测试，FRL_SKIP_SLOW 可跳过）
// ============================================================
TEST(t_slow_line_too_long) {
  if (skip_slow()) {
    std::cout << "    [SKIPPED slow test]\n";
    return;
  }
  const std::size_t huge = 16ULL * 1024 * 1024 + 1024;  // > 16MB
  std::string content(huge, 'x');
  content += "\n";
  TempFile f(content);
  std::string out, err;
  bool ok = file_read_line(f.path, 1, 1, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
  CHECK_TRUE(err.find("exceeds") != std::string::npos);
}

TEST(t_slow_total_out_too_big) {
  if (skip_slow()) {
    std::cout << "    [SKIPPED slow test]\n";
    return;
  }
  // 65 行 × 1MB ≈ 65MB > 64MB 上限
  const std::size_t line_len = 1024 * 1024;
  std::string line(line_len, 'y');
  std::string content;
  content.reserve(65 * (line_len + 1));
  for (int i = 0; i < 65; ++i) {
    content += line;
    content += '\n';
  }
  TempFile f(content);
  content.clear();
  content.shrink_to_fit();

  std::string out, err;
  bool ok = file_read_line(f.path, 1, 65, out, err);
  CHECK_FALSE(ok);
  CHECK(out.empty());
  CHECK_TRUE(err.find("exceeds") != std::string::npos);
}

// ============================================================
int main() { return t::run_all(); }
#endif