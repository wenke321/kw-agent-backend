#include "helpers/function.h"

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

#define TEST(name)                                                              \
static bool name();                                                         \
    static struct name##_reg_t {                                                \
      name##_reg_t() { g_tests.push_back({#name, name}); }                    \
} name##_reg_inst;                                                          \
    static bool name()

#define CHECK(cond) do {                                                        \
    if (!(cond)) {                                                              \
      std::fprintf(stderr, "    FAIL %s:%d: %s\n",                            \
                   __FILE__, __LINE__, #cond);                                \
      ++g_fail;                                                               \
      return false;                                                           \
}                                                                           \
} while (0)

#define CHECK_EQ(a, b) do {                                                     \
  auto _a = (a); auto _b = (b);                                               \
    if (!(_a == _b)) {                                                          \
      std::fprintf(stderr, "    FAIL %s:%d: %s != %s\n",                      \
                   __FILE__, __LINE__, #a, #b);                               \
      std::fprintf(stderr, "      left : [%s]\n", _a.c_str());                \
      std::fprintf(stderr, "      right: [%s]\n", _b.c_str());                \
      ++g_fail;                                                               \
      return false;                                                           \
}                                                                           \
} while (0)

  // =====================================================================
  // 测试辅助
  // =====================================================================
  static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                          s.data(), n, nullptr, nullptr);
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
  bool ok = content.empty() ||
            std::fwrite(content.data(), 1, content.size(), fp) == content.size();
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
  return ::GetFileAttributesW(kw::utf8_to_wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void remove_file(const std::string& path) {
  ::DeleteFileW(kw::utf8_to_wide(path).c_str());
}

static bool is_readonly(const std::string& path) {
  const DWORD a = ::GetFileAttributesW(kw::utf8_to_wide(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
}

// 断言返回值为错误（非 "success"）
#define CHECK_FAIL(expr) do {                                                   \
const std::string _r = (expr);                                              \
    if (_r == "success") {                                                      \
      std::fprintf(stderr, "    FAIL %s:%d: expected failure, got success\n", \
                   __FILE__, __LINE__);                                       \
      ++g_fail;                                                               \
      return false;                                                           \
}                                                                           \
} while (0)

#define CHECK_SUCCESS(expr) do {                                                \
  const std::string _r = (expr);                                              \
    if (_r != "success") {                                                      \
      std::fprintf(stderr, "    FAIL %s:%d: expected success, got [%s]\n",    \
                   __FILE__, __LINE__, _r.c_str());                           \
      ++g_fail;                                                               \
      return false;                                                           \
}                                                                           \
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
  CHECK(write_file(p, "A\nB\nC"));   // 末行无 \n
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
  CHECK(write_file(p, "abc"));       // 只有 1 行，无尾 \n
  CHECK_FAIL(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("abc"));
  remove_file(p);
  return true;
}

TEST(test_start_beyond_empty_file) {
  const std::string p = path_for("sbempty");
  CHECK(write_file(p, ""));          // 空文件 = 0 行
  CHECK_FAIL(kw::file_delete_line(p, 1, 1));
  CHECK_EQ(read_file(p), std::string(""));
  remove_file(p);
  return true;
}

TEST(test_start_beyond_blank_lines) {
  const std::string p = path_for("sbblank");
  CHECK(write_file(p, "\n\n\n"));    // 3 个空行
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
  CHECK(write_file(p, "A\nB\nC"));     // 末行 "C" 无 \n
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
  CHECK(write_file(p, "\xEF\xBB\xBF" "A\nB\nC\n"));
  CHECK_SUCCESS(kw::file_delete_line(p, 2, 2));
  CHECK_EQ(read_file(p), std::string("\xEF\xBB\xBF" "A\nC\n"));
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
  CHECK(is_readonly(p));   // 只读属性应恢复

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
  const int N = 50000;              // 约 600 KB，跨多个 64K 缓冲块
  CHECK(write_file(p, make_large_file(N)));

  const int del = 23456;
  CHECK_SUCCESS(kw::file_delete_line(p, del, del));

  const std::string out = read_file(p);
  int lines = 0;
  for (char c : out) if (c == '\n') ++lines;
  CHECK_EQ(std::to_string(lines), std::to_string(N - 1));

  char del_tag[32], before[32], after[32];
  std::snprintf(del_tag, sizeof(del_tag), "line_%06d\n", del);
  std::snprintf(before,  sizeof(before),  "line_%06d\n", del - 1);
  std::snprintf(after,   sizeof(after),   "line_%06d\n", del + 1);
  CHECK(out.find(del_tag) == std::string::npos);
  CHECK(out.find(before)  != std::string::npos);
  CHECK(out.find(after)   != std::string::npos);
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
  CHECK_EQ(read_file(p), original);   // 文件必须一字不差
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
  content.append(kTestBufSize, 'A');    // 偏移 0 .. 65535
  content += "\n";                      // 偏移 65536：下一块首字节
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
  content += "\xE4\xB8\xAD";              // 跨块字符
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