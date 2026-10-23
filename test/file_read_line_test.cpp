#include <iostream>
#include "helpers/function.h"

// ============================================================================
//  ==========================  测试框架  ====================================
// ============================================================================

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_TRUE(expr)                                                     \
do {                                                                      \
      if (!(expr)) {                                                        \
        std::cerr << "    FAIL: " #expr " @line " << __LINE__ << "\n";    \
        ++g_fail;                                                         \
  } else { ++g_pass; }                                                  \
} while (0)

#define EXPECT_EQ(a, b)                                                       \
    do {                                                                      \
      auto _va = (a); auto _vb = (b);                                       \
      if (!(_va == _vb)) {                                                  \
        std::cerr << "    FAIL: " #a " == " #b " @line " << __LINE__      \
        << "\n";                                                \
        std::cerr << "      left  = [" << _va << "]\n";                   \
        std::cerr << "      right = [" << _vb << "]\n";                   \
        ++g_fail;                                                         \
  } else { ++g_pass; }                                                  \
} while (0)

#define EXPECT_CONTAINS(hay, needle)                                          \
  do {                                                                      \
        std::string _h = (hay), _n = (needle);                                \
        if (_h.find(_n) == std::string::npos) {                               \
          std::cerr << "    FAIL: not contains \"" << _n                  \
          << "\" @line " << __LINE__ << "\n";                     \
          std::cerr << "      actual = [" << _h << "]\n";                   \
          ++g_fail;                                                         \
    } else { ++g_pass; }                                                  \
  } while (0)

#define EXPECT_NOT_CONTAINS(hay, needle)                                      \
      do {                                                                      \
        std::string _h = (hay), _n = (needle);                                \
        if (_h.find(_n) != std::string::npos) {                               \
          std::cerr << "    FAIL: unexpectedly contains \"" << _n         \
          << "\" @line " << __LINE__ << "\n";                     \
          ++g_fail;                                                         \
    } else { ++g_pass; }                                                  \
  } while (0)

#define RUN_TEST(fn)                                                          \
  do {                                                                      \
        std::cout << "[ RUN  ] " << #fn << "\n";                              \
        int _before = g_fail;                                                 \
        fn();                                                                 \
        std::cout << (g_fail == _before ? "[  OK  ] " : "[ FAIL ] ")          \
        << #fn << "\n";                                             \
  } while (0)

      // ============================================================================
      //  ==========================  测试辅助  ====================================
      // ============================================================================

      std::string WideToUtf8Str(const std::wstring& w) {
    if (w.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                   nullptr, 0, nullptr, nullptr);
    std::string s(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), need, nullptr, nullptr);
    return s;
  }

std::wstring TempDirW() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, buf);
  return std::wstring(buf, n);
}

// tag 是 UTF-8，可以被测试用来验证非 ASCII 路径转换
std::string MakeTempFile(const std::string& tag_utf8, const std::string& content) {
  static int counter = 0;
  std::wstring wtag;
  if (!kw::PathToWide(tag_utf8, wtag)) return {};

  wchar_t name[256];
  swprintf_s(name, L"frl_%ls_%d_%lu.tmp",
             wtag.c_str(), ++counter, GetCurrentProcessId());

  std::wstring full = TempDirW() + name;
  HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    std::cerr << "    setup: cannot create temp file (err="
              << GetLastError() << ")\n";
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
  for (char c : s) if (c == '\n') ++n;
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
  EXPECT_TRUE(kw::file_read_line(p, 1, 0, out));   // end=0 → start+2000=2001
  EXPECT_CONTAINS(out, "1: L 1\n");
  EXPECT_CONTAINS(out, "2001: L 2001\n");
  EXPECT_NOT_CONTAINS(out, "2002: L 2002\n");
  EXPECT_EQ(CountLines(out), 2001);

  out.clear();
  EXPECT_TRUE(kw::file_read_line(p, 100, 0, out)); // 100..2100
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
  std::string p = MakeTempFile("notrail", "a\nb\nc");   // 3 行，最后一行无 \n
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
  content += '\xEF'; content += '\xBB'; content += '\xBF';
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
  content += '\xFF'; content += '\xFE';
  content += 'h'; content += '\0';
  content += 'i'; content += '\0';
  std::string p = MakeTempFile("u16le", content);
  std::string out;
  EXPECT_TRUE(!kw::file_read_line(p, 1, 1, out));
  EXPECT_CONTAINS(out, "UTF-16LE");
  RemoveTemp(p);
}

// ---- 12. UTF-16BE BOM → 拒绝 ----
void test_utf16be_rejected() {
  std::string content;
  content += '\xFE'; content += '\xFF';
  content += '\0'; content += 'h';
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
  EXPECT_EQ(out, std::string("1: ") + u8"你好世界\n" +
                     std::string("2: ") + u8"emoji: \xF0\x9F\x98\x80\n" +
                     std::string("3: ") + u8"中文行 3\n");
  RemoveTemp(p);
}

// ---- 14. 非 ASCII 路径 ----
void test_non_ascii_path() {
  std::string p = MakeTempFile(u8"中文目录", "hello\nworld\n");
  if (p.empty()) { std::cerr << "    skip: cannot create non-ascii path\n"; return; }
  std::string out;
  EXPECT_TRUE(kw::file_read_line(p, 1, 2, out));
  EXPECT_EQ(out, std::string("1: hello\n2: world\n"));
  RemoveTemp(p);
}

// ---- 15. 文件不存在 ----
void test_file_not_found() {
  std::string out;
  EXPECT_TRUE(!kw::file_read_line("Z:\\__definitely_not_exists__.txt", 1, 5, out));
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
  EXPECT_EQ(out.size(), 200ull * 1024 + 4);   // "1: " + content + '\n'
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