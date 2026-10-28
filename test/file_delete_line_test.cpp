#include "helpers/function.h"

#if defined(_WIN32)
// =====================================================================
// 测试框架（极简）
// =====================================================================
static int g_pass = 0;
static int g_fail = 0;

struct TestCase {
  std::string name;
  std::function<bool()> fn;
};
static std::vector<TestCase> g_tests;

#define TEST(name) \
  static bool name(); \
  static struct name##_reg_t { \
    name##_reg_t() { g_tests.push_back({#name, name}); } \
  } name##_reg_inst; \
  static bool name()

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_fail; \
      return false; \
    } \
  } while (0)

#define CHECK_EQ(a, b) \
  do { \
    auto _a = (a); \
    auto _b = (b); \
    if (!(_a == _b)) { \
      std::fprintf(stderr, "    FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, \
                   #a, #b); \
      std::fprintf(stderr, "      left : [%s]\n", _a.c_str()); \
      std::fprintf(stderr, "      right: [%s]\n", _b.c_str()); \
      ++g_fail; \
      return false; \
    } \
  } while (0)

// =====================================================================
// 测试辅助
// =====================================================================
static std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
  return s;
}

static std::string get_temp_dir_utf8() {
  wchar_t buf[MAX_PATH] = {};
  DWORD n = ::GetTempPathW(MAX_PATH, buf);
  if (n == 0) return ".\\";
  return wide_to_utf8(std::wstring(buf, n));
}

static std::string path_for(const char* tag) {
  static const std::string dir = get_temp_dir_utf8();
  return dir + "fdl_test_" + tag + ".txt";
}

static bool write_file(const std::string& path, const std::string& content) {
  const std::wstring wp = kw::utf8_to_wide(path);
  FILE* fp = nullptr;
  if (::_wfopen_s(&fp, wp.c_str(), L"wb") != 0 || !fp) return false;
  bool ok = content.empty() || std::fwrite(content.data(), 1, content.size(),
                                           fp) == content.size();
  std::fclose(fp);
  return ok;
}

static std::string read_file(const std::string& path) {
  const std::wstring wp = kw::utf8_to_wide(path);
  FILE* fp = nullptr;
  if (::_wfopen_s(&fp, wp.c_str(), L"rb") != 0 || !fp) return "<open-failed>";
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
  std::fclose(fp);
  return out;
}

static bool file_exists(const std::string& path) {
  return ::GetFileAttributesW(kw::utf8_to_wide(path).c_str()) !=
         INVALID_FILE_ATTRIBUTES;
}

static void remove_file(const std::string& path) {
  ::DeleteFileW(kw::utf8_to_wide(path).c_str());
}

static bool is_readonly(const std::string& path) {
  const DWORD a = ::GetFileAttributesW(kw::utf8_to_wide(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
}

// 断言返回值为错误（非 "success"）
#define CHECK_FAIL(expr) \
  do { \
    const std::string _r = (expr); \
    if (_r == "success") { \
      std::fprintf(stderr, "    FAIL %s:%d: expected failure, got success\n", \
                   __FILE__, __LINE__); \
      ++g_fail; \
      return false; \
    } \
  } while (0)

#define CHECK_SUCCESS(expr) \
  do { \
    const std::string _r = (expr); \
    if (_r != "success") { \
      std::fprintf(stderr, "    FAIL %s:%d: expected success, got [%s]\n", \
                   __FILE__, __LINE__, _r.c_str()); \
      ++g_fail; \
      return false; \
    } \
  } while (0)

// =====================================================================
// 一、参数校验
// =====================================================================
TEST(test_empty_path) {
  CHECK_FAIL(kw::file_delete_line("", 1, 1));
  return true;
}

TEST(test_start_zero) {
  CHECK_FAIL(kw::file_delete_line("some.txt", 0, 1));
  return true;
}

TEST(test_start_negative) {
  CHECK_FAIL(kw::file_delete_line("some.txt", -5, 1));
  return true;
}

TEST(test_end_less_than_start) {
  CHECK_FAIL(kw::file_delete_line("some.txt", 5, 3));
  return true;
}

TEST(test_end_equals_start_minus_one) {
  CHECK_FAIL(kw::file_delete_line("some.txt", 2, 1));
  return true;
}

// =====================================================================
// 二、路径
// =====================================================================
TEST(test_nonexistent_file) {
  const std::string p = path_for("nonexistent");
  remove_file(p);
  CHECK(!file_exists(p));
  CHECK_FAIL(kw::file_delete_line(p, 1, 1));
  return true;
}

TEST(test_path_is_directory) {
  const std::string dir = get_temp_dir_utf8() + "fdl_test_directory";
  const std::wstring wdir = kw::utf8_to_wide(dir);
  ::CreateDirectoryW(wdir.c_str(), nullptr);
  CHECK_FAIL(kw::file_delete_line(dir, 1, 1));
  ::RemoveDirectoryW(wdir.c_str());
  return true;
}

TEST(test_invalid_utf8_path) {
  // 0xFF 0xFE 不是合法 UTF-8 起始字节
  std::string bad = "fdl_";
  bad.push_back((char)0xFF);
  bad.push_back((char)0xFE);
  bad += ".txt";
  CHECK_FAIL(kw::file_delete_line(bad, 1, 1));
  return true;
}

// =====================================================================
// 三、基本删除
// =====================================================================
TEST(test_delete_middle_single_line) {
  const std::string p = path_for("mid1");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("A\nC\n"));
  remove_file(p);
  return true;
}

TEST(test_delete_first_line) {
  const std::string p = path_for("first");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 1));
  CHECK_EQ(read_file(p), std::string("B\nC\n"));
  remove_file(p);
  return true;
}

TEST(test_delete_last_line) {
  const std::string p = path_for("last");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 3, 3));
  CHECK_EQ(read_file(p), std::string("A\nB\n"));
  remove_file(p);
  return true;
}

TEST(test_delete_range_1_2) {
  const std::string p = path_for("r12");
  CHECK(write_file(p, "A\nB\nC\nD\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 2));
  CHECK_EQ(read_file(p), std::string("C\nD\n"));
  remove_file(p);
  return true;
}

TEST(test_delete_range_2_3) {
  const std::string p = path_for("r23");
  CHECK(write_file(p, "A\nB\nC\nD\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 3));
  CHECK_EQ(read_file(p), std::string("A\nD\n"));
  remove_file(p);
  return true;
}

TEST(test_delete_range_2_4_of_5) {
  const std::string p = path_for("r24");
  CHECK(write_file(p, "A\nB\nC\nD\nE\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 4));
  CHECK_EQ(read_file(p), std::string("A\nE\n"));
  remove_file(p);
  return true;
}

TEST(test_delete_all_lines) {
  const std::string p = path_for("all");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 3));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

TEST(test_delete_all_lines_with_big_end) {
  const std::string p = path_for("allbig");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 1000));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

// =====================================================================
// 四、end 超出末行：删除到文件尾
// =====================================================================
TEST(test_end_beyond_file_tail) {
  const std::string p = path_for("endbeyond");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 100));
  CHECK_EQ(read_file(p), std::string("A\n"));
  remove_file(p);
  return true;
}

TEST(test_end_int_max) {
  const std::string p = path_for("endintmax");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, INT_MAX));
  CHECK_EQ(read_file(p), std::string("A\n"));
  remove_file(p);
  return true;
}

TEST(test_end_beyond_no_trailing_newline) {
  const std::string p = path_for("endbeyondnonl");
  CHECK(write_file(p, "A\nB\nC"));  // 末行无 \n
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 100));
  CHECK_EQ(read_file(p), std::string("A\n"));
  remove_file(p);
  return true;
}

// =====================================================================
// 五、start 超过文件总行数：必须报错且原文件不变
// =====================================================================
TEST(test_start_beyond_3_lines) {
  const std::string p = path_for("sb3");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_FAIL(kw::file_delete_line(p, 4, 5));
  CHECK_EQ(read_file(p), std::string("A\nB\nC\n"));  // 文件未变
  remove_file(p);
  return true;
}

TEST(test_start_beyond_single_line_no_newline) {
  const std::string p = path_for("sb1");
  CHECK(write_file(p, "abc"));  // 只有 1 行，无尾 \n
  CHECK_FAIL(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("abc"));
  remove_file(p);
  return true;
}

TEST(test_start_beyond_empty_file) {
  const std::string p = path_for("sbempty");
  CHECK(write_file(p, ""));  // 空文件 = 0 行
  CHECK_FAIL(kw::file_delete_line(p, 1, 1));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

TEST(test_start_beyond_blank_lines) {
  const std::string p = path_for("sbblank");
  CHECK(write_file(p, "\n\n\n"));  // 3 个空行
  CHECK_FAIL(kw::file_delete_line(p, 4, 4));
  CHECK_EQ(read_file(p), std::string("\n\n\n"));
  remove_file(p);
  return true;
}

TEST(test_start_one_on_empty_file) {
  const std::string p = path_for("s1empty");
  CHECK(write_file(p, ""));
  CHECK_FAIL(kw::file_delete_line(p, 1, 1));
  remove_file(p);
  return true;
}

// =====================================================================
// 六、单行、空行、行尾
// =====================================================================
TEST(test_delete_only_line_with_newline) {
  const std::string p = path_for("onlynl");
  CHECK(write_file(p, "abc\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 1));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

TEST(test_delete_only_line_no_newline) {
  const std::string p = path_for("onlynonl");
  CHECK(write_file(p, "abc"));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 1));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

TEST(test_delete_only_blank_line) {
  const std::string p = path_for("blank1");
  CHECK(write_file(p, "\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 1));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

TEST(test_delete_middle_blank_line) {
  const std::string p = path_for("blankmid");
  CHECK(write_file(p, "A\n\nB\n"));  // 第 2 行是空行
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("A\nB\n"));
  remove_file(p);
  return true;
}

TEST(test_delete_multiple_blank_lines) {
  const std::string p = path_for("blankmulti");
  CHECK(write_file(p, "A\n\n\nB\n"));  // 第 2、3 行为空行
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 3));
  CHECK_EQ(read_file(p), std::string("A\nB\n"));
  remove_file(p);
  return true;
}

TEST(test_delete_middle_line_no_trailing_newline) {
  const std::string p = path_for("midnonl");
  CHECK(write_file(p, "A\nB\nC"));  // 末行 "C" 无 \n
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("A\nC"));
  remove_file(p);
  return true;
}

TEST(test_delete_last_line_no_trailing_newline) {
  const std::string p = path_for("lastnonl");
  CHECK(write_file(p, "A\nB\nC"));
  CHECK_SUCCESS(kw::file_delete_line(p, 3, 3));
  CHECK_EQ(read_file(p), std::string("A\nB\n"));
  remove_file(p);
  return true;
}

// =====================================================================
// 七、CRLF / 混合行尾
// =====================================================================
TEST(test_crlf_delete_middle) {
  const std::string p = path_for("crlfmid");
  CHECK(write_file(p, "A\r\nB\r\nC\r\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("A\r\nC\r\n"));
  remove_file(p);
  return true;
}

TEST(test_crlf_delete_last_no_trailing) {
  const std::string p = path_for("crlflast");
  CHECK(write_file(p, "A\r\nB\r\nC"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("A\r\nC"));
  remove_file(p);
  return true;
}

TEST(test_mixed_line_endings) {
  const std::string p = path_for("mixed");
  CHECK(write_file(p, "A\r\nB\nC\r\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("A\r\nC\r\n"));
  remove_file(p);
  return true;
}

TEST(test_cr_only_treated_as_one_line) {
  // 老 Mac 风格只有 \r：当前实现把整段当一行处理
  const std::string p = path_for("cronly");
  CHECK(write_file(p, "A\rB\rC"));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 1));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

// =====================================================================
// 八、UTF-8 内容
// =====================================================================
TEST(test_utf8_chinese_content) {
  const std::string p = path_for("utf8");
  CHECK(write_file(p, "你好\n世界\n再见\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("你好\n再见\n"));
  remove_file(p);
  return true;
}

TEST(test_utf8_with_bom) {
  const std::string p = path_for("utf8bom");
  // BOM 会被并到第 1 行内容里
  CHECK(write_file(p,
                   "\xEF\xBB\xBF"
                   "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("\xEF\xBB\xBF"
                                     "A\nC\n"));
  remove_file(p);
  return true;
}

// =====================================================================
// 九、只读文件
// =====================================================================
TEST(test_readonly_file_deleted_and_attribute_restored) {
  const std::string p = path_for("ro");
  CHECK(write_file(p, "A\nB\nC\n"));
  const std::wstring wp = kw::utf8_to_wide(p);
  DWORD a = ::GetFileAttributesW(wp.c_str());
  CHECK(a != INVALID_FILE_ATTRIBUTES);
  CHECK(::SetFileAttributesW(wp.c_str(), a | FILE_ATTRIBUTE_READONLY));
  CHECK(is_readonly(p));

  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("A\nC\n"));
  CHECK(is_readonly(p));  // 只读属性应恢复

  // 清掉只读再删除
  a = ::GetFileAttributesW(wp.c_str());
  ::SetFileAttributesW(wp.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
  remove_file(p);
  return true;
}

// =====================================================================
// 十、大文件 / 跨缓冲区边界
// =====================================================================
static std::string make_large_file(int n) {
  std::string s;
  s.reserve((size_t)n * 12);
  for (int i = 1; i <= n; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "line_%06d\n", i);
    s += buf;
  }
  return s;
}

TEST(test_large_delete_middle) {
  const std::string p = path_for("large_mid");
  const int N = 50000;  // 约 600 KB，跨多个 64K 缓冲块
  CHECK(write_file(p, make_large_file(N)));

  const int del = 23456;
  CHECK_SUCCESS(kw::file_delete_line(p, del, del));

  const std::string out = read_file(p);
  int lines = 0;
  for (char c : out)
    if (c == '\n') ++lines;
  CHECK_EQ(std::to_string(lines), std::to_string(N - 1));

  char del_tag[32], before[32], after[32];
  std::snprintf(del_tag, sizeof(del_tag), "line_%06d\n", del);
  std::snprintf(before, sizeof(before), "line_%06d\n", del - 1);
  std::snprintf(after, sizeof(after), "line_%06d\n", del + 1);
  CHECK(out.find(del_tag) == std::string::npos);
  CHECK(out.find(before) != std::string::npos);
  CHECK(out.find(after) != std::string::npos);
  remove_file(p);
  return true;
}

TEST(test_large_delete_range_near_start) {
  const std::string p = path_for("large_head");
  const int N = 20000;
  CHECK(write_file(p, make_large_file(N)));

  CHECK_SUCCESS(kw::file_delete_line(p, 1, 100));
  const std::string out = read_file(p);
  CHECK(out.find("line_000001\n") == std::string::npos);
  CHECK(out.find("line_000100\n") == std::string::npos);
  CHECK(out.find("line_000101\n") != std::string::npos);
  CHECK(out.find("line_020000\n") != std::string::npos);
  remove_file(p);
  return true;
}

TEST(test_large_delete_tail_range) {
  const std::string p = path_for("large_tail");
  const int N = 20000;
  CHECK(write_file(p, make_large_file(N)));

  CHECK_SUCCESS(kw::file_delete_line(p, N - 99, N));
  const std::string out = read_file(p);
  CHECK(out.find("line_019900\n") != std::string::npos);
  CHECK(out.find("line_019901\n") == std::string::npos);
  CHECK(out.find("line_020000\n") == std::string::npos);
  remove_file(p);
  return true;
}

TEST(test_large_delete_all) {
  const std::string p = path_for("large_all");
  const int N = 20000;
  CHECK(write_file(p, make_large_file(N)));

  CHECK_SUCCESS(kw::file_delete_line(p, 1, N));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

TEST(test_large_start_beyond) {
  const std::string p = path_for("large_sb");
  const int N = 20000;
  const std::string original = make_large_file(N);
  CHECK(write_file(p, original));

  CHECK_FAIL(kw::file_delete_line(p, N + 1, N + 10));
  CHECK_EQ(read_file(p), original);  // 文件必须一字不差
  remove_file(p);
  return true;
}

// =====================================================================
// 十一、失败时文件不被破坏
// =====================================================================
TEST(test_file_unchanged_on_start_beyond) {
  const std::string p = path_for("unchanged");
  const std::string original = "A\nB\nC\nD\nE\n";
  CHECK(write_file(p, original));
  CHECK_FAIL(kw::file_delete_line(p, 99, 100));
  CHECK_EQ(read_file(p), original);
  remove_file(p);
  return true;
}

TEST(test_file_still_exists_after_failure) {
  const std::string p = path_for("exists_after_fail");
  CHECK(write_file(p, "A\nB\nC\n"));
  CHECK_FAIL(kw::file_delete_line(p, 10, 20));
  CHECK(file_exists(p));
  remove_file(p);
  return true;
}

// =====================================================================
// 十二、跨 64K 缓冲区边界
// =====================================================================
// 实现内部使用 64K 的 fread 缓冲：只有把关键字节放到偏移 65535/65536
// 上，才能真正触发跨块的解析路径。
static constexpr size_t kTestBufSize = 64 * 1024;

TEST(test_lf_is_first_byte_of_second_chunk) {
  // 第 1 行恰好 64K 字节，'\n' 成为第二块的第一个字节。
  const std::string p = path_for("lf_split");

  std::string content;
  content.reserve(kTestBufSize + 8);
  content.append(kTestBufSize, 'A');  // 偏移 0 .. 65535
  content += "\n";                    // 偏移 65536：下一块首字节
  content += "B\n";

  CHECK(write_file(p, content));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));

  std::string expect;
  expect.append(kTestBufSize, 'A');
  expect += "\n";
  CHECK_EQ(read_file(p), expect);
  remove_file(p);
  return true;
}

TEST(test_crlf_split_across_buffer_boundary) {
  // 让 '\r' 是第 65536 个字节（偏移 65535），'\n' 落到下一块首字节。
  const std::string p = path_for("crlf_split");

  std::string content;
  content.reserve(kTestBufSize + 8);
  content.append(kTestBufSize - 1, 'A');  // 偏移 0 .. 65534
  content += "\r\n";                      // 偏移 65535 = '\r'，65536 = '\n'
  content += "B\n";

  CHECK(write_file(p, content));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 1));

  CHECK_EQ(read_file(p), std::string("B\n"));
  remove_file(p);
  return true;
}

TEST(test_crlf_split_preserved_when_line_kept) {
  // 反向验证：跨块的 "\r\n" 在保留时也必须原样输出。
  const std::string p = path_for("crlf_split_keep");

  std::string content;
  content.reserve(kTestBufSize + 8);
  content.append(kTestBufSize - 1, 'A');
  content += "\r\n";
  content += "B\n";

  CHECK(write_file(p, content));
  // 删掉第 2 行，第 1 行的 "\r\n" 必须原样留下
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));

  std::string expect;
  expect.append(kTestBufSize - 1, 'A');
  expect += "\r\n";
  CHECK_EQ(read_file(p), expect);
  remove_file(p);
  return true;
}

TEST(test_utf8_multibyte_split_across_buffer_boundary) {
  // 3 字节 UTF-8 字符 "中"（E4 B8 AD）横跨边界：
  // 偏移 65535 = E4，65536 = B8，65537 = AD。
  const std::string p = path_for("utf8_split");

  std::string content;
  content.reserve(kTestBufSize + 16);
  content.append(kTestBufSize - 1, 'A');  // 0 .. 65534
  content += "\xE4\xB8\xAD";              // 65535 .. 65537
  content += "\n";                        // 65538
  content += "B\n";

  CHECK(write_file(p, content));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));

  std::string expect;
  expect.append(kTestBufSize - 1, 'A');
  expect += "\xE4\xB8\xAD";
  expect += "\n";
  CHECK_EQ(read_file(p), expect);
  remove_file(p);
  return true;
}

TEST(test_utf8_multibyte_split_then_delete_containing_line) {
  // 跨块的多字节字符在整行被删掉时，也必须一字节不剩地清掉。
  const std::string p = path_for("utf8_split_del");

  std::string content;
  content.reserve(kTestBufSize + 16);
  content.append(kTestBufSize - 1, 'A');
  content += "\xE4\xB8\xAD";  // 跨块字符
  content += "\n";
  content += "B\n";
  content += "C\n";

  CHECK(write_file(p, content));
  CHECK_SUCCESS(kw::file_delete_line(p, 1, 1));

  CHECK_EQ(read_file(p), std::string("B\nC\n"));
  remove_file(p);
  return true;
}

// =====================================================================
// 十三、只读 + 失败路径
// =====================================================================
TEST(test_readonly_file_restored_on_start_beyond) {
  const std::string p = path_for("ro_fail");
  const std::string original = "A\nB\nC\n";
  CHECK(write_file(p, original));

  const std::wstring wp = kw::utf8_to_wide(p);
  DWORD a = ::GetFileAttributesW(wp.c_str());
  CHECK(a != INVALID_FILE_ATTRIBUTES);
  CHECK(::SetFileAttributesW(wp.c_str(), a | FILE_ATTRIBUTE_READONLY));
  CHECK(is_readonly(p));

  // start 越界 -> 失败；只读属性必须恢复，内容必须一字不差
  CHECK_FAIL(kw::file_delete_line(p, 99, 100));
  CHECK(is_readonly(p));
  CHECK_EQ(read_file(p), original);

  // 清掉只读再删除
  a = ::GetFileAttributesW(wp.c_str());
  ::SetFileAttributesW(wp.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
  remove_file(p);
  return true;
}

TEST(test_readonly_file_restored_on_nonpositive_start) {
  // 参数校验更早失败：只读属性根本不应该被动过
  const std::string p = path_for("ro_param");
  CHECK(write_file(p, "A\nB\nC\n"));

  const std::wstring wp = kw::utf8_to_wide(p);
  DWORD a = ::GetFileAttributesW(wp.c_str());
  CHECK(a != INVALID_FILE_ATTRIBUTES);
  CHECK(::SetFileAttributesW(wp.c_str(), a | FILE_ATTRIBUTE_READONLY));
  CHECK(is_readonly(p));

  CHECK_FAIL(kw::file_delete_line(p, 0, 0));
  CHECK(is_readonly(p));
  CHECK_EQ(read_file(p), std::string("A\nB\nC\n"));

  a = ::GetFileAttributesW(wp.c_str());
  ::SetFileAttributesW(wp.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
  remove_file(p);
  return true;
}

// =====================================================================
// main
// =====================================================================
int main() {
  std::printf("Running %zu tests...\n\n", g_tests.size());
  for (auto& t : g_tests) {
    std::printf("[ RUN  ] %s\n", t.name.c_str());
    const bool ok = t.fn();
    if (ok) {
      std::printf("[  OK  ] %s\n", t.name.c_str());
      ++g_pass;
    } else {
      std::printf("[ FAIL ] %s\n", t.name.c_str());
    }
  }
  std::printf("\n===== %d passed, %d failed =====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#elif defined(__linux__)

// ============================================================
// edline_test.cpp — single-file POSIX-only test suite for the
// production-grade "delete line range from file" implementation.
// ============================================================

#define _GNU_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>  // memfd_create
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "helpers/log.h"

// ============================================================
// ======================= Test framework =====================
// ============================================================

static int g_pass = 0;
static int g_fail = 0;
static int g_case_pass = 0;
static int g_case_fail = 0;

std::string errno_str(int e) {
  char buf[256] = {};
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  const char* s = ::strerror_r(e, buf, sizeof(buf));
  if (s && s[0]) return std::string(s) + " (errno=" + std::to_string(e) + ")";
#else
  if (::strerror_r(e, buf, sizeof(buf)) == 0 && buf[0])
    return std::string(buf) + " (errno=" + std::to_string(e) + ")";
#endif
  return "errno=" + std::to_string(e);
}

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::cerr << "  [FAIL] " << __LINE__ << ": " #cond "\n"; \
      ++g_fail; \
      ++g_case_fail; \
    } else { \
      ++g_pass; \
      ++g_case_pass; \
    } \
  } while (0)

#define CHECK_EQ(a, b) \
  do { \
    auto _a = (a); \
    auto _b = (b); \
    if (!(_a == _b)) { \
      std::cerr << "  [FAIL] " << __LINE__ << ": " #a " == " #b "\n" \
                << "         got : " << _a << "\n" \
                << "         want: " << _b << "\n"; \
      ++g_fail; \
      ++g_case_fail; \
    } else { \
      ++g_pass; \
      ++g_case_pass; \
    } \
  } while (0)

#define RUN_TEST(fn) \
  do { \
    std::cerr << "RUN " #fn "\n"; \
    g_case_pass = g_case_fail = 0; \
    fn(); \
    std::cerr << "  " << (g_case_fail == 0 ? "PASS" : "FAIL") << " (" \
              << g_case_pass << " ok / " << (g_case_pass + g_case_fail) \
              << " total)\n"; \
  } while (0)

// ---------------- helpers ----------------

static void rm_rf(const std::string& p) {
  DIR* d = ::opendir(p.c_str());
  if (!d) {
    ::unlink(p.c_str());
    return;
  }
  struct dirent* e;
  while ((e = ::readdir(d)) != nullptr) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
    std::string sub = p + "/" + e->d_name;
    struct stat st{};
    if (::lstat(sub.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      rm_rf(sub);
    else
      ::unlink(sub.c_str());
  }
  ::closedir(d);
  ::rmdir(p.c_str());
}

struct TestDir {
  std::string path;
  TestDir() {
    char tmpl[] = "/tmp/edline_test.XXXXXX";
    char* p = ::mkdtemp(tmpl);
    if (!p) throw std::runtime_error("mkdtemp failed");
    path = p;
  }
  std::string file(const std::string& n) const { return path + "/" + n; }
  ~TestDir() { rm_rf(path); }
};

static int mkfd(const std::string& content) {
  int fd = ::memfd_create("edline_test", MFD_CLOEXEC);
  if (fd < 0) throw std::runtime_error("memfd_create failed");
  if (!content.empty()) {
    ssize_t w = ::write(fd, content.data(), content.size());
    if (w != static_cast<ssize_t>(content.size())) {
      ::close(fd);
      throw std::runtime_error("write memfd failed");
    }
  }
  return fd;
}

static std::string read_fd_range(int fd, off_t off, off_t len) {
  std::string out;
  if (len <= 0) return out;
  out.resize(static_cast<size_t>(len), '\0');
  ssize_t n;
  do {
    n = ::pread(fd, out.data(), out.size(), off);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return {};
  out.resize(static_cast<size_t>(n));
  return out;
}

static std::string read_file(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return out;
}

static void write_file(const std::string& path, const std::string& c,
                       mode_t mode = 0644) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) throw std::runtime_error("open write_file failed");
  if (!c.empty()) {
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(c.size())) {
      ssize_t w = ::write(fd, c.data() + off, c.size() - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        break;
      }
      off += w;
    }
  }
  ::close(fd);
}

// ============================================================
// =========================== Tests ==========================
// ============================================================

// ---------------- errno_str ----------------
static void test_errno_str() {
  for (int e : {0, ENOENT, EACCES, EINVAL, EIO, ERANGE}) {
    std::string s = errno_str(e);
    CHECK(!s.empty());
    CHECK(s.find("errno=" + std::to_string(e)) != std::string::npos);
  }
  // unknown errno
  std::string s = errno_str(9999);
  CHECK(s.find("errno=9999") != std::string::npos);
}

int locate_lines(int fd, off_t file_size, int start, int end, off_t* S_out,
                 off_t* E_out) {
  if (file_size < 0) {
    errno = EINVAL;
    return -1;
  }
  if (start < 1 || end < start) {
    errno = EINVAL;
    return -1;
  }
  if (!S_out || !E_out) {
    errno = EINVAL;
    return -1;
  }
  *S_out = 0;
  *E_out = 0;

  off_t S = 0;
  bool found_S = (start == 1);
  const int64_t end_p1 = static_cast<int64_t>(end) + 1;
  int64_t cur = 1;
  off_t pos = 0;

  constexpr size_t BUF_SZ = 256 * 1024;
  std::vector<char> buf(BUF_SZ);

  while (pos < file_size) {
    const off_t want =
        std::min<off_t>(static_cast<off_t>(BUF_SZ), file_size - pos);
    ssize_t n;
    do {
      n = ::pread(fd, buf.data(), static_cast<size_t>(want), pos);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if (n == 0) {
      errno = EIO;
      return -1;
    }  // 文件被并发截断

    size_t i = 0;
    const size_t un = static_cast<size_t>(n);
    const char* base = buf.data();
    while (i < un) {
      const void* p = ::memchr(base + i, '\n', un - i);
      if (!p) break;
      const size_t idx = static_cast<const char*>(p) - base;
      const off_t next = pos + static_cast<off_t>(idx) + 1;
      ++cur;
      if (cur == end_p1) {
        *S_out = S;
        *E_out = next;
        return 0;
      }
      if (!found_S && cur == start) {
        S = next;
        found_S = true;
      }
      i = idx + 1;
    }
    pos += static_cast<off_t>(n);
  }

  int64_t total_lines = 0;
  if (file_size > 0) {
    char lastc = 0;
    ssize_t n;
    do {
      n = ::pread(fd, &lastc, 1, file_size - 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) return -1;
    total_lines = (lastc == '\n') ? (cur - 1) : cur;
  }
  if (static_cast<int64_t>(start) > total_lines) return -2;
  if (!found_S) return -2;
  *S_out = S;
  *E_out = file_size;
  return 0;
}

// ---------------- locate_lines ----------------
static void test_locate_lines() {
  auto run = [](const std::string& content, int start, int end, off_t* S,
                off_t* E) -> int {
    int fd = mkfd(content);
    int rc =
        locate_lines(fd, static_cast<off_t>(content.size()), start, end, S, E);
    ::close(fd);
    return rc;
  };

  off_t S = -1, E = -1;

  // empty file
  CHECK_EQ(run("", 1, 1, &S, &E), -2);

  // one line + trailing newline
  CHECK_EQ(run("a\n", 1, 1, &S, &E), 0);
  CHECK_EQ(S, 0);
  CHECK_EQ(E, 2);
  CHECK_EQ(run("a\n", 2, 2, &S, &E), -2);

  // one line, no trailing newline
  CHECK_EQ(run("a", 1, 1, &S, &E), 0);
  CHECK_EQ(S, 0);
  CHECK_EQ(E, 1);

  // multiple lines with trailing newline
  CHECK_EQ(run("a\nb\nc\n", 1, 1, &S, &E), 0);
  CHECK_EQ(S, 0);
  CHECK_EQ(E, 2);
  CHECK_EQ(run("a\nb\nc\n", 2, 2, &S, &E), 0);
  CHECK_EQ(S, 2);
  CHECK_EQ(E, 4);
  CHECK_EQ(run("a\nb\nc\n", 2, 3, &S, &E), 0);
  CHECK_EQ(S, 2);
  CHECK_EQ(E, 6);
  CHECK_EQ(run("a\nb\nc\n", 1, 3, &S, &E), 0);
  CHECK_EQ(S, 0);
  CHECK_EQ(E, 6);
  CHECK_EQ(run("a\nb\nc\n", 3, 3, &S, &E), 0);
  CHECK_EQ(S, 4);
  CHECK_EQ(E, 6);
  CHECK_EQ(run("a\nb\nc\n", 4, 4, &S, &E), -2);

  // multiple lines without trailing newline
  CHECK_EQ(run("a\nb\nc", 3, 3, &S, &E), 0);
  CHECK_EQ(S, 4);
  CHECK_EQ(E, 5);
  CHECK_EQ(run("a\nb\nc", 2, 3, &S, &E), 0);
  CHECK_EQ(S, 2);
  CHECK_EQ(E, 5);
  CHECK_EQ(run("a\nb\nc", 1, 3, &S, &E), 0);
  CHECK_EQ(S, 0);
  CHECK_EQ(E, 5);

  // empty line (just "\n")
  CHECK_EQ(run("\n", 1, 1, &S, &E), 0);
  CHECK_EQ(S, 0);
  CHECK_EQ(E, 1);

  // parameter validation (needs a real fd)
  {
    int fd = mkfd("a\n");
    off_t s, e;
    CHECK_EQ(locate_lines(fd, 2, 1, 1, nullptr, &e), -1);
    CHECK_EQ(locate_lines(fd, 2, 1, 1, &s, nullptr), -1);
    CHECK_EQ(locate_lines(fd, -1, 1, 1, &s, &e), -1);
    CHECK_EQ(locate_lines(fd, 2, 0, 1, &s, &e), -1);
    CHECK_EQ(locate_lines(fd, 2, 3, 2, &s, &e), -1);
    ::close(fd);
  }

  // larger-than-buffer test
  {
    std::string big;
    big.reserve(80000 * 10);
    for (int i = 0; i < 80000; ++i) big += "line" + std::to_string(i) + "\n";

    int fd = mkfd(big);
    off_t s, e;
    int rc =
        locate_lines(fd, static_cast<off_t>(big.size()), 79999, 80000, &s, &e);
    CHECK_EQ(rc, 0);

    off_t expect_s = 0;
    for (int i = 0; i < 79998; ++i)
      expect_s = static_cast<off_t>(big.find('\n', expect_s)) + 1;
    CHECK_EQ(s, expect_s);
    CHECK_EQ(e, static_cast<off_t>(big.size()));
    ::close(fd);
  }
}

int copy_fd_range(int src, int dst, off_t src_offset, off_t dst_offset,
                  off_t length) {
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  if (length == 0) return 0;

  // 防御：O_APPEND 下 pwrite/copy_file_range 会忽略显式偏移
  {
    const int fl = ::fcntl(dst, F_GETFL);
    if (fl < 0) return -1;
    if (fl & O_APPEND) {
      errno = EINVAL;
      return -1;
    }
  }

  off_t sp = src_offset;
  off_t dp = dst_offset;
  off_t remaining = length;

  // 阶段一：copy_file_range
  while (remaining > 0) {
    const size_t chunk = static_cast<size_t>(
        std::min<off_t>(static_cast<off_t>(SIZE_MAX / 2), remaining));
    const ssize_t n = ::copy_file_range(src, &sp, dst, &dp, chunk, 0);
    if (n > 0) {
      remaining -= n;
      continue;
    }
    if (n == 0) {
      errno = EIO;
      return -1;
    }
    if (errno == EINTR) continue;
    if (errno == EXDEV || errno == ENOSYS || errno == EOPNOTSUPP ||
        errno == EINVAL || errno == EPERM || errno == EBADF) {
      break;  // 退回 pread/pwrite
    }
    return -1;
  }
  if (remaining == 0) return 0;

  // 阶段二：pread/pwrite 显式偏移
  constexpr size_t BUF_SZ = 1024 * 1024;
  std::vector<char> buf(BUF_SZ);

  while (remaining > 0) {
    const size_t want = static_cast<size_t>(
        std::min<off_t>(static_cast<off_t>(BUF_SZ), remaining));
    ssize_t r;
    do {
      r = ::pread(src, buf.data(), want, sp);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    if (r == 0) {
      errno = EIO;
      return -1;
    }

    ssize_t written = 0;
    while (written < r) {
      const ssize_t w =
          ::pwrite(dst, buf.data() + written, static_cast<size_t>(r - written),
                   dp + written);
      if (w < 0) {
        if (errno == EINTR) continue;
        return -1;
      }
      if (w == 0) {
        errno = EIO;
        return -1;
      }
      written += w;
    }
    sp += r;
    dp += r;
    remaining -= r;
  }
  return 0;
}

// ---------------- copy_fd_range ----------------
static void test_copy_fd_range() {
  // same offset
  {
    int src = mkfd("hello world");
    int dst = mkfd("");
    CHECK_EQ(copy_fd_range(src, dst, 0, 0, 11), 0);
    CHECK_EQ(read_fd_range(dst, 0, 11), std::string("hello world"));
    ::close(src);
    ::close(dst);
  }
  // mid source
  {
    int src = mkfd("abcdefghij");
    int dst = mkfd("");
    CHECK_EQ(copy_fd_range(src, dst, 3, 0, 5), 0);
    CHECK_EQ(read_fd_range(dst, 0, 5), std::string("defgh"));
    ::close(src);
    ::close(dst);
  }
  // mid destination (does not clobber other bytes)
  {
    int src = mkfd("XYZ");
    int dst = mkfd("______");
    CHECK_EQ(copy_fd_range(src, dst, 0, 2, 3), 0);
    CHECK_EQ(read_fd_range(dst, 0, 6), std::string("__XYZ_"));
    ::close(src);
    ::close(dst);
  }
  // length == 0
  {
    int src = mkfd("abc");
    int dst = mkfd("");
    CHECK_EQ(copy_fd_range(src, dst, 0, 0, 0), 0);
    ::close(src);
    ::close(dst);
  }
  // negative length
  {
    int src = mkfd("abc");
    int dst = mkfd("");
    errno = 0;
    int r = copy_fd_range(src, dst, 0, 0, -1);
    int saved = errno;
    CHECK_EQ(r, -1);
    CHECK_EQ(saved, EINVAL);
    ::close(src);
    ::close(dst);
  }
  // O_APPEND on dst must be rejected (pwrite semantics)
  {
    int src = mkfd("abc");
    int dst = mkfd("");
    int fl = ::fcntl(dst, F_GETFL);
    CHECK(fl >= 0);
    CHECK_EQ(::fcntl(dst, F_SETFL, fl | O_APPEND), 0);
    errno = 0;
    int r = copy_fd_range(src, dst, 0, 0, 3);
    int saved = errno;
    CHECK_EQ(r, -1);
    CHECK_EQ(saved, EINVAL);
    ::close(src);
    ::close(dst);
  }
  // cross-buffer (3 MiB)
  {
    std::string big(3 * 1024 * 1024, 'X');
    for (size_t i = 0; i < big.size(); i += 1000)
      big[i] = static_cast<char>('a' + (i / 1000) % 26);
    int src = mkfd(big);
    int dst = mkfd("");
    CHECK_EQ(copy_fd_range(src, dst, 0, 0, static_cast<off_t>(big.size())), 0);
    CHECK_EQ(read_fd_range(dst, 0, static_cast<off_t>(big.size())), big);
    ::close(src);
    ::close(dst);
  }
  // source shorter than requested → EIO
  {
    int src = mkfd("abc");
    int dst = mkfd("");
    errno = 0;
    int r = copy_fd_range(src, dst, 0, 0, 10);
    int saved = errno;
    CHECK_EQ(r, -1);
    CHECK_EQ(saved, EIO);
    ::close(src);
    ::close(dst);
  }
}

int copy_xattrs(int src, int dst) {
  std::vector<char> names;
  bool listed = false;

  // 获取名称列表，处理 ERANGE（期间有 xattr 被增加/删除）
  for (int attempt = 0; attempt < 8; ++attempt) {
    errno = 0;
    const ssize_t len = ::flistxattr(src, nullptr, 0);
    if (len < 0) {
      if (errno == ENOTSUP || errno == EOPNOTSUPP)
        return 0;  // src 不支持 xattr
      return -1;
    }
    if (len == 0) return 0;  // 无 xattr
    names.resize(static_cast<size_t>(len));
    const ssize_t nread = ::flistxattr(src, names.data(), names.size());
    if (nread < 0) {
      if (errno == ERANGE) continue;
      return -1;
    }
    if (nread == 0) return 0;
    names.resize(static_cast<size_t>(nread));
    listed = true;
    break;
  }
  if (!listed) {  // 多次重试仍 ERANGE
    errno = ERANGE;
    return -1;
  }

  int rc = 0;  // -1 表示 security.* 失败
  for (size_t i = 0; i < names.size();) {
    const char* name = names.data() + i;
    const size_t nlen = std::strlen(name);
    if (nlen == 0) {
      ++i;
      continue;
    }
    i += nlen + 1;

    const bool is_security = (std::strncmp(name, "security.", 9) == 0);

    // 读取值：先取长度，再读内容；ERANGE 重试
    std::vector<char> value;
    ssize_t got = -1;
    bool enodata = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
      const ssize_t vlen = ::fgetxattr(src, name, nullptr, 0);
      if (vlen < 0) {
        if (errno == ENODATA) enodata = true;  // 并发删除
        break;
      }
      value.assign(static_cast<size_t>(vlen), '\0');
      got = ::fgetxattr(src, name, value.data(), value.size());
      if (got >= 0) break;
      if (errno != ERANGE) break;
    }
    if (enodata) continue;
    if (got < 0) {
      if (is_security) {
        LOG_ERROR("copy_xattrs: fgetxattr('{}') failed: {}", name,
                  errno_str(errno));
        rc = -1;
      } else {
        LOG_WARN("copy_xattrs: fgetxattr('{}') failed: {}", name,
                 errno_str(errno));
      }
      continue;
    }
    value.resize(static_cast<size_t>(got));

    // size==0 时用 "" 而不是 nullptr，兼容性更好
    const char* vptr = value.empty() ? "" : value.data();
    if (::fsetxattr(dst, name, vptr, value.size(), 0) < 0) {
      if (is_security) {
        // security.selinux / security.capability 丢失属于安全边界变化
        LOG_ERROR("copy_xattrs: fsetxattr('{}') on target failed: {}", name,
                  errno_str(errno));
        rc = -1;
      } else {
        LOG_WARN("copy_xattrs: fsetxattr('{}') on target failed: {}", name,
                 errno_str(errno));
      }
    }
  }
  return rc;
}

// ---------------- copy_xattrs ----------------
static void test_copy_xattrs() {
  // no xattrs
  {
    int src = mkfd("a");
    int dst = mkfd("b");
    CHECK_EQ(copy_xattrs(src, dst), 0);
    ::close(src);
    ::close(dst);
  }
  // single xattr
  {
    int src = mkfd("a");
    int dst = mkfd("b");
    CHECK_EQ(::fsetxattr(src, "user.foo", "bar", 3, 0), 0);
    CHECK_EQ(copy_xattrs(src, dst), 0);
    char buf[16] = {};
    ssize_t n = ::fgetxattr(dst, "user.foo", buf, sizeof(buf));
    CHECK_EQ(n, 3);
    CHECK(std::string(buf, 3) == "bar");
    ::close(src);
    ::close(dst);
  }
  // multiple xattrs
  {
    int src = mkfd("a");
    int dst = mkfd("b");
    CHECK_EQ(::fsetxattr(src, "user.a", "1", 1, 0), 0);
    CHECK_EQ(::fsetxattr(src, "user.b", "22", 2, 0), 0);
    CHECK_EQ(::fsetxattr(src, "user.c", "333", 3, 0), 0);
    CHECK_EQ(copy_xattrs(src, dst), 0);
    char buf[8];
    CHECK_EQ(::fgetxattr(dst, "user.a", buf, sizeof(buf)), 1);
    CHECK_EQ(::fgetxattr(dst, "user.b", buf, sizeof(buf)), 2);
    CHECK_EQ(::fgetxattr(dst, "user.c", buf, sizeof(buf)), 3);
    ::close(src);
    ::close(dst);
  }
  // empty value
  {
    int src = mkfd("a");
    int dst = mkfd("b");
    CHECK_EQ(::fsetxattr(src, "user.empty", "", 0, 0), 0);
    CHECK_EQ(copy_xattrs(src, dst), 0);
    ssize_t n = ::fgetxattr(dst, "user.empty", nullptr, 0);
    CHECK_EQ(n, 0);
    ::close(src);
    ::close(dst);
  }
  // overwrite existing value on dst
  {
    int src = mkfd("a");
    int dst = mkfd("b");
    CHECK_EQ(::fsetxattr(src, "user.k", "new", 3, 0), 0);
    CHECK_EQ(::fsetxattr(dst, "user.k", "old", 3, 0), 0);
    CHECK_EQ(copy_xattrs(src, dst), 0);
    char buf[8] = {};
    ssize_t n = ::fgetxattr(dst, "user.k", buf, sizeof(buf));
    CHECK_EQ(n, 3);
    CHECK(std::string(buf, 3) == "new");
    ::close(src);
    ::close(dst);
  }
}

// ---------------- file_delete_line ----------------
static void test_file_delete_line() {
  TestDir td;

  // basic: middle range
  {
    std::string p = td.file("a.txt");
    write_file(p, "a\nb\nc\nd\n");
    CHECK_EQ(kw::file_delete_line(p, 2, 3), std::string("success"));
    CHECK_EQ(read_file(p), std::string("a\nd\n"));
  }
  // delete first line
  {
    std::string p = td.file("b.txt");
    write_file(p, "a\nb\nc\n");
    CHECK_EQ(kw::file_delete_line(p, 1, 1), std::string("success"));
    CHECK_EQ(read_file(p), std::string("b\nc\n"));
  }
  // delete last line
  {
    std::string p = td.file("c.txt");
    write_file(p, "a\nb\nc\n");
    CHECK_EQ(kw::file_delete_line(p, 3, 3), std::string("success"));
    CHECK_EQ(read_file(p), std::string("a\nb\n"));
  }
  // delete everything
  {
    std::string p = td.file("d.txt");
    write_file(p, "a\nb\n");
    CHECK_EQ(kw::file_delete_line(p, 1, 2), std::string("success"));
    CHECK_EQ(read_file(p), std::string(""));
  }
  // last line without trailing newline
  {
    std::string p = td.file("e.txt");
    write_file(p, "a\nb\nc");
    CHECK_EQ(kw::file_delete_line(p, 3, 3), std::string("success"));
    CHECK_EQ(read_file(p), std::string("a\nb\n"));
  }
  // end > total lines → clamped to EOF (documented behaviour)
  {
    std::string p = td.file("n.txt");
    write_file(p, "a\nb\nc\n");
    CHECK_EQ(kw::file_delete_line(p, 2, 100), std::string("success"));
    CHECK_EQ(read_file(p), std::string("a\n"));
  }
  // single line, no trailing newline
  {
    std::string p = td.file("o.txt");
    write_file(p, "only");
    CHECK_EQ(kw::file_delete_line(p, 1, 1), std::string("success"));
    CHECK_EQ(read_file(p), std::string(""));
  }

  // ---- error / refusal paths ----
  {
    CHECK(kw::file_delete_line(td.file("no_such.txt"), 1, 1) != "success");
  }
  {
    std::string p = td.file("subdir");
    CHECK_EQ(::mkdir(p.c_str(), 0755), 0);
    CHECK(kw::file_delete_line(p, 1, 1) != "success");
  }
  {
    std::string p = td.file("parent_missing/x.txt");
    CHECK(kw::file_delete_line(p, 1, 1) != "success");
  }
  {
    std::string p = td.file("f.txt");
    write_file(p, "a\n");
    CHECK(kw::file_delete_line(p, 0, 1) != "success");
  }
  {
    std::string p = td.file("g.txt");
    write_file(p, "a\nb\n");
    CHECK(kw::file_delete_line(p, 2, 1) != "success");
  }
  {
    std::string p = td.file("h.txt");
    write_file(p, "a\nb\n");
    CHECK(kw::file_delete_line(p, 5, 5) != "success");
  }
  // hard link must be refused
  {
    std::string p1 = td.file("i1.txt");
    std::string p2 = td.file("i2.txt");
    write_file(p1, "a\nb\n");
    CHECK_EQ(::link(p1.c_str(), p2.c_str()), 0);
    CHECK(kw::file_delete_line(p1, 1, 1) != "success");
  }

  // ---- metadata preservation ----
  // mode preservation
  {
    std::string p = td.file("j.txt");
    write_file(p, "a\nb\n");
    CHECK_EQ(::chmod(p.c_str(), 0755), 0);
    CHECK_EQ(kw::file_delete_line(p, 1, 1), std::string("success"));
    struct stat st{};
    CHECK_EQ(::stat(p.c_str(), &st), 0);
    CHECK_EQ(static_cast<int>(st.st_mode & 07777), 0755);
  }
  // xattr preservation
  {
    std::string p = td.file("k.txt");
    write_file(p, "a\nb\n");
    CHECK_EQ(::setxattr(p.c_str(), "user.k", "v", 1, 0), 0);
    CHECK_EQ(kw::file_delete_line(p, 1, 1), std::string("success"));
    char buf[8] = {};
    ssize_t n = ::getxattr(p.c_str(), "user.k", buf, sizeof(buf));
    CHECK_EQ(n, 1);
    CHECK(std::string(buf, 1) == "v");
  }

  // ---- symlink: follow to target, symlink stays ----
  {
    std::string real = td.file("p_real.txt");
    std::string link = td.file("p_link.txt");
    write_file(real, "a\nb\n");
    CHECK_EQ(::symlink(real.c_str(), link.c_str()), 0);
    CHECK_EQ(kw::file_delete_line(link, 1, 1), std::string("success"));
    CHECK_EQ(read_file(real), std::string("b\n"));
    struct stat st{};
    CHECK_EQ(::lstat(link.c_str(), &st), 0);
    CHECK(S_ISLNK(st.st_mode));
  }

  // ---- big file across buffer boundary ----
  {
    std::string p = td.file("m.txt");
    std::string big;
    big.reserve(50000 * 10);
    for (int i = 0; i < 50000; ++i) big += "line " + std::to_string(i) + "\n";
    write_file(p, big);
    CHECK_EQ(kw::file_delete_line(p, 25000, 25000), std::string("success"));
    std::string result = read_file(p);

    int nl = 0;
    for (char c : result)
      if (c == '\n') ++nl;
    CHECK_EQ(nl, 49999);

    // after delete, line 25000 (1-based) must be "line 25000"
    off_t pos = 0;
    for (int i = 0; i < 24999; ++i)
      pos = static_cast<off_t>(result.find('\n', pos)) + 1;
    CHECK_EQ(result.substr(static_cast<size_t>(pos), 10),
             std::string("line 25000"));
  }
}

// ============================================================
// ============================ main ==========================
// ============================================================
int main() {
  std::cerr << "=== edline_test suite ===\n";
  RUN_TEST(test_errno_str);
  RUN_TEST(test_locate_lines);
  RUN_TEST(test_copy_fd_range);
  RUN_TEST(test_copy_xattrs);
  RUN_TEST(test_file_delete_line);

  std::cerr << "\nTotal: " << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}

#endif