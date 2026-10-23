#include "helpers/function.h"
#include <iostream>

// ============================================================================
//  测试基础设施
// ============================================================================
namespace {

int g_checks   = 0;
int g_failures = 0;
std::filesystem::path g_tmp_dir;

// 把不可见字节转成可读形式，便于定位失败
std::string printable(const std::string& s, size_t maxn = 160) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (i >= maxn) { out += "..."; break; }
    if      (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20 || c >= 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

#define CHECK(cond)                                                        \
do {                                                                   \
      ++g_checks;                                                        \
      if (!(cond)) {                                                     \
        ++g_failures;                                                  \
        std::cerr << "    FAIL  " << __FILE__ << ":" << __LINE__       \
        << "  " #cond << "\n";                               \
  }                                                                  \
} while (0)

#define CHECK_STR_EQ(a, b)                                                 \
    do {                                                                   \
      ++g_checks;                                                        \
      std::string _a = (a);                                              \
      std::string _b = (b);                                              \
      if (_a != _b) {                                                    \
        ++g_failures;                                                  \
        std::cerr << "    FAIL  " << __FILE__ << ":" << __LINE__       \
        << "\n      实际: \"" << printable(_a)               \
        << "\"\n      期望: \"" << printable(_b)             \
        << "\"\n      长度: " << _a.size() << " vs "         \
        << _b.size() << "\n";                                \
  }                                                                  \
} while (0)

  // UTF-8 <-> wide 转换（测试代码本身用它读写文件、构造路径）
  std::wstring to_wide_utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                          static_cast<int>(s.size()), w.data(), n);
    return w;
  }

std::string path_to_utf8(const std::filesystem::path& p) {
  const std::wstring w = p.wstring();
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
  return s;
}

std::string make_path(const std::string& name) {
  auto p = g_tmp_dir / std::filesystem::path(to_wide_utf8(name));
  return path_to_utf8(p);
}

bool write_file(const std::string& path, const std::string& content) {
  std::ofstream f(std::filesystem::path(to_wide_utf8(path)),
                  std::ios::binary | std::ios::trunc);
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  return f.good();
}

std::string read_file(const std::string& path) {
  std::ifstream f(std::filesystem::path(to_wide_utf8(path)), std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void remove_file(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(to_wide_utf8(path)), ec);
}

// ---- 数据驱动：通用成功用例 ------------------------------------------------
struct Case {
  std::string name;
  std::string initial;
  size_t      line;
  std::string data;
  std::string expected;
};

void run_case(const Case& c) {
  std::cout << "  [case] " << c.name << "\n";
  const std::string path = make_path(c.name + ".txt");
  write_file(path, c.initial);

  std::string r = kw::file_insert_line(path.c_str(), c.line,
                                   c.data.data(), c.data.size());
  if (r != "success") {
    ++g_checks; ++g_failures;
    std::cerr << "    FAIL 期望 success，实际: " << r << "\n";
  } else {
    CHECK_STR_EQ(read_file(path), c.expected);
  }
  remove_file(path);
}

void run_simple(const std::string& name,
                const std::string& initial,
                size_t line,
                const std::string& data,
                const std::string& expected) {
  run_case({name, initial, line, data, expected});
}

// ============================================================================
//  1. 功能：基本插入位置、空文件、无末尾换行、CRLF、UTF-8 内容
// ============================================================================
void test_simple_cases() {
  std::cout << "[test_simple_cases]\n";

          // 3 行 + 末尾换行，插入到各行之后
  run_simple("t01_mid_1",  "line1\nline2\nline3\n", 1, "INSERT\n",
             "line1\nINSERT\nline2\nline3\n");
  run_simple("t02_mid_2",  "line1\nline2\nline3\n", 2, "INSERT\n",
             "line1\nline2\nINSERT\nline3\n");
  run_simple("t03_mid_3",  "line1\nline2\nline3\n", 3, "INSERT\n",
             "line1\nline2\nline3\nINSERT\n");
  run_simple("t04_after_last", "line1\nline2\nline3\n", 4, "INSERT\n",
             "line1\nline2\nline3\nINSERT\n");   // 超出行数 → 追加
  run_simple("t05_way_after",  "line1\nline2\nline3\n", 100, "INSERT\n",
             "line1\nline2\nline3\nINSERT\n");

          // line == 0 → 文件头
  run_simple("t06_head", "line1\nline2\n", 0, "INSERT",
             "INSERTline1\nline2\n");

          // 空文件：任何 line 都等价于写文件头
  run_simple("t07_empty_l0",   "", 0,   "INSERT", "INSERT");
  run_simple("t08_empty_l1",   "", 1,   "INSERT", "INSERT");
  run_simple("t09_empty_l100", "", 100, "INSERT", "INSERT");

          // 无末尾换行
  run_simple("t10_nolf_head", "hello", 0, "X", "Xhello");
  run_simple("t11_nolf_l1",   "hello", 1, "X", "hello\nX");
  run_simple("t12_nolf_l2",   "hello", 2, "X", "hello\nX");

          // "a\nb\nc"
  run_simple("t13_abc_l1", "a\nb\nc", 1, "X", "a\nXb\nc");
  run_simple("t14_abc_l2", "a\nb\nc", 2, "X", "a\nb\nXc");
  run_simple("t15_abc_l3", "a\nb\nc", 3, "X", "a\nb\nc\nX");
  run_simple("t16_abc_l4", "a\nb\nc", 4, "X", "a\nb\nc\nX");

          // 只有换行符
  run_simple("t17_nl_l0",   "\n",     0, "X", "X\n");
  run_simple("t18_nl_l1",   "\n",     1, "X", "\nX");
  run_simple("t19_nl3_l2",  "\n\n\n", 2, "X", "\n\nX\n");

          // CRLF 保留 \r
  run_simple("t20_crlf_l1", "a\r\nb\r\nc\r\n", 1, "X", "a\r\nXb\r\nc\r\n");
  run_simple("t21_crlf_l0", "a\r\nb\r\nc\r\n", 0, "X", "Xa\r\nb\r\nc\r\n");
  run_simple("t22_crlf_l3", "a\r\nb\r\nc\r\n", 3, "X",
             "a\r\nb\r\nc\r\nX");

          // 空数据 / len==0：内容不变，仍返回 success
  run_simple("t23_empty_data_l0", "abc\n", 0, "", "abc\n");
  run_simple("t24_empty_data_l1", "abc\n", 1, "", "abc\n");
  run_simple("t25_empty_data_l5", "abc\n", 5, "", "abc\n");
  run_simple("t26_empty_all",     "",     0, "", "");

          // 插入的数据本身含 \n / \r\n
  run_simple("t27_data_has_nl", "abc\n", 1, "M\nN\r\n", "abc\nM\nN\r\n");

          // UTF-8 内容（"测试\n" + 插入 "中"）
          //   \xE6\xB5\x8B\xE8\xAF\x95  = "测试"
          //   \xE4\xB8\xAD              = "中"
  run_simple("t28_utf8_content",
             "\xE6\xB5\x8B\xE8\xAF\x95\n", 1, "\xE4\xB8\xAD",
             "\xE6\xB5\x8B\xE8\xAF\x95\n\xE4\xB8\xAD");
}

// ============================================================================
//  2. 参数校验与错误路径
// ============================================================================
void test_invalid_args() {
  std::cout << "[test_invalid_args]\n";

          // file_path == nullptr
  {
    std::string r = kw::file_insert_line(nullptr, 0, "x", 1);
    CHECK(r != "success");
    CHECK_STR_EQ(r, std::string("invalid file_path"));
  }

          // file_path == ""
  {
    std::string r = kw::file_insert_line("", 0, "x", 1);
    CHECK(r != "success");
    CHECK_STR_EQ(r, std::string("invalid file_path"));
  }

          // data == nullptr 且 len > 0 → 应该报错，且不改动文件
  {
    const std::string path = make_path("inv_data_null.txt");
    write_file(path, "abc\n");
    std::string r = kw::file_insert_line(path.c_str(), 0, nullptr, 5);
    CHECK(r != "success");
    CHECK_STR_EQ(r, std::string("invalid data"));
    CHECK_STR_EQ(read_file(path), std::string("abc\n"));
    remove_file(path);
  }

          // data == nullptr 且 len == 0 → 合法（no-op）
  {
    const std::string path = make_path("null_data_zero_len.txt");
    write_file(path, "abc\n");
    std::string r = kw::file_insert_line(path.c_str(), 1, nullptr, 0);
    CHECK_STR_EQ(r, std::string("success"));
    CHECK_STR_EQ(read_file(path), std::string("abc\n"));
    remove_file(path);
  }

          // 文件不存在
  {
    const std::string path = make_path("no_such_file_xyz.txt");
    remove_file(path);
    std::string r = kw::file_insert_line(path.c_str(), 0, "x", 1);
    CHECK(r != "success");
    CHECK(r.find("open failed") != std::string::npos);
  }

          // 非法 UTF-8 路径（\xC0\x80 是 overlong 编码，永远非法）
  {
    std::string bad = "invalid_";
    bad += static_cast<char>(0xC0);
    bad += static_cast<char>(0x80);
    bad += ".txt";
    std::string r = kw::file_insert_line(bad.c_str(), 0, "x", 1);
    CHECK(r != "success");
    CHECK_STR_EQ(r, std::string("invalid path encoding"));
  }

          // 目录不存在 → 打开失败（CreateFileW 返回 ERROR_PATH_NOT_FOUND）
  {
    std::string path = make_path("no_such_dir/sub/file.txt");
    std::string r = kw::file_insert_line(path.c_str(), 0, "x", 1);
    CHECK(r != "success");
    CHECK(r.find("open failed") != std::string::npos);
  }
}

// ============================================================================
//  3. 64KB 缓冲区边界（扫描 / 拷贝）
// ============================================================================
void test_buffer_boundaries() {
  std::cout << "[test_buffer_boundaries]\n";

          // '\n' 落在第一个 64KB 缓冲区的最后一个字节（偏移 65535）
  {
    const std::string path = make_path("nl_at_65535.txt");
    std::string content(65535, 'x');
    content += '\n';
    content += "tail";
    write_file(path, content);

    std::string r = kw::file_insert_line(path.c_str(), 1, "INSERT", 6);
    CHECK_STR_EQ(r, std::string("success"));

    std::string expect(65535, 'x');
    expect += '\n';
    expect += "INSERT";
    expect += "tail";
    CHECK_STR_EQ(read_file(path), expect);
    remove_file(path);
  }

          // '\n' 恰好在第二个缓冲区开头（偏移 65536）
  {
    const std::string path = make_path("nl_at_65536.txt");
    std::string content(65536, 'x');
    content += '\n';
    content += "tail";
    write_file(path, content);

    std::string r = kw::file_insert_line(path.c_str(), 1, "INSERT", 6);
    CHECK_STR_EQ(r, std::string("success"));

    std::string expect(65536, 'x');
    expect += '\n';
    expect += "INSERT";
    expect += "tail";
    CHECK_STR_EQ(read_file(path), expect);
    remove_file(path);
  }

          // 两个 '\n' 分别位于两个缓冲区
  {
    const std::string path = make_path("two_nl_across.txt");
    std::string content(65536, 'x');
    content += '\n';                    // 第一个 \n 在 65536
    content += std::string(100, 'y');
    content += '\n';                    // 第二个 \n
    write_file(path, content);

            // 在第 2 行之后插入（第二段之后）
    std::string r = kw::file_insert_line(path.c_str(), 2, "AAA", 3);
    CHECK_STR_EQ(r, std::string("success"));

    std::string expect(65536, 'x');
    expect += '\n';
    expect += std::string(100, 'y');
    expect += '\n';
    expect += "AAA";
    CHECK_STR_EQ(read_file(path), expect);
    remove_file(path);
  }

          // 插入点正好是文件大小（追加场景，无尾部拷贝）
  {
    const std::string path = make_path("append_at_eof.txt");
    write_file(path, std::string(200 * 1024, 'a') + "\n");
    std::string r = kw::file_insert_line(path.c_str(), 1, "ZZZ", 3);
    CHECK_STR_EQ(r, std::string("success"));
    CHECK_STR_EQ(read_file(path),
                 std::string(200 * 1024, 'a') + "\nZZZ");
    remove_file(path);
  }
}

// ============================================================================
//  4. 大数据 / 大文件
// ============================================================================
void test_large_data() {
  std::cout << "[test_large_data]\n";

          // 插入数据 > 64KB（拷贝头部 + 写入 data + 拷贝尾部）
  {
    const std::string path = make_path("big_insert_200k.txt");
    write_file(path, "head\nmid\n");

    std::string big(200 * 1024, 'Z');
    std::string r = kw::file_insert_line(path.c_str(), 1, big.data(), big.size());
    CHECK_STR_EQ(r, std::string("success"));

    std::string expect = "head\n" + big + "mid\n";
    CHECK_STR_EQ(read_file(path), expect);
    remove_file(path);
  }

          // 插入数据跨越 kw::write_all 的 1MB 分块
  {
    const std::string path = make_path("big_insert_1mb.txt");
    write_file(path, "a\nb\n");

    std::string big((1u << 20) + 4096, 'Q');
    std::string r = kw::file_insert_line(path.c_str(), 1, big.data(), big.size());
    CHECK_STR_EQ(r, std::string("success"));

    std::string expect = "a\n" + big + "b\n";
    CHECK_STR_EQ(read_file(path), expect);
    remove_file(path);
  }

          // 大源文件（> 64KB 拷贝缓冲区）+ 小插入
  {
    const std::string path = make_path("big_file_small_insert.txt");
    std::string content;
    content.reserve(300 * 1024);
    content += "L0\n";
    for (int i = 1; i < 10000; ++i) {
      content += "line" + std::to_string(i) + "\n";
    }
    write_file(path, content);

    std::string r = kw::file_insert_line(path.c_str(), 1, "NEW\n", 4);
    CHECK_STR_EQ(r, std::string("success"));
    CHECK_STR_EQ(read_file(path), "L0\nNEW\n" + content.substr(3));
    remove_file(path);
  }

          // 空源文件 + 大插入
  {
    const std::string path = make_path("empty_big_insert.txt");
    write_file(path, "");
    std::string big(500 * 1024, 'E');
    std::string r = kw::file_insert_line(path.c_str(), 0, big.data(), big.size());
    CHECK_STR_EQ(r, std::string("success"));
    CHECK_STR_EQ(read_file(path), big);
    remove_file(path);
  }
}

// ============================================================================
//  5. 二进制内容（NUL 字节）
// ============================================================================
void test_binary() {
  std::cout << "[test_binary]\n";

          // 源文件含 NUL 字节（注意 NUL 不是行分隔符，\n 仍按字节扫描）
  {
    const std::string path = make_path("nul_in_source.txt");
    std::string content;
    content += 'a'; content += 'b'; content += '\0';
    content += 'c'; content += 'd'; content += '\n';
    content += 'e'; content += '\n';
    write_file(path, content);

    std::string r = kw::file_insert_line(path.c_str(), 1, "X", 1);
    CHECK_STR_EQ(r, std::string("success"));

    std::string expect;
    expect += 'a'; expect += 'b'; expect += '\0';
    expect += 'c'; expect += 'd'; expect += '\n';
    expect += 'X';
    expect += 'e'; expect += '\n';
    CHECK_STR_EQ(read_file(path), expect);
    remove_file(path);
  }

          // 插入数据含 NUL 字节
  {
    const std::string path = make_path("nul_in_insert.txt");
    write_file(path, "a\nb\n");

    std::string data;
    data += 'P'; data += '\0'; data += 'Q';

    std::string r = kw::file_insert_line(path.c_str(), 1, data.data(), data.size());
    CHECK_STR_EQ(r, std::string("success"));

    std::string expect;
    expect += 'a'; expect += '\n';
    expect += 'P'; expect += '\0'; expect += 'Q';
    expect += 'b'; expect += '\n';
    CHECK_STR_EQ(read_file(path), expect);
    remove_file(path);
  }

          // 高字节（0x80..0xFF）序列：\n 仍按字节扫描，不应误判
  {
    const std::string path = make_path("high_bytes.txt");
    std::string content;
    for (int i = 0; i < 300; ++i) {
      content += static_cast<char>(0x80 + (i % 0x7f));
    }
    content += '\n';
    content += "T";
    write_file(path, content);

    std::string r = kw::file_insert_line(path.c_str(), 1, "|", 1);
    CHECK_STR_EQ(r, std::string("success"));

    std::string expect = content.substr(0, content.size() - 1) + "|T";
    CHECK_STR_EQ(read_file(path), expect);
    remove_file(path);
  }
}

// ============================================================================
//  6. UTF-8 路径
// ============================================================================
void test_utf8_path() {
  std::cout << "[test_utf8_path]\n";

          // 中文文件名："测试.txt"
  const std::string name = "\xE6\xB5\x8B\xE8\xAF\x95";
  const std::string path = make_path(name + ".txt");
  write_file(path, "a\nb\n");

  std::string r = kw::file_insert_line(path.c_str(), 1, "X", 1);
  CHECK_STR_EQ(r, std::string("success"));
  CHECK_STR_EQ(read_file(path), "a\nXb\n");
  remove_file(path);

          // 中文目录 + 中文文件名
  const std::string dir  = "\xE7\x9B\xAE\xE5\xBD\x95";  // "目录"
  const std::string f2   = "\xE6\x96\x87\xE4\xBB\xB6";  // "文件"
  std::error_code ec;
  std::filesystem::create_directories(
      g_tmp_dir / std::filesystem::path(to_wide_utf8(dir)), ec);
  const std::string p2 = path_to_utf8(
      g_tmp_dir / std::filesystem::path(to_wide_utf8(dir))
      / std::filesystem::path(to_wide_utf8(f2 + ".txt")));
  write_file(p2, "1\n2\n");

  r = kw::file_insert_line(p2.c_str(), 1, "M", 1);
  CHECK_STR_EQ(r, std::string("success"));
  CHECK_STR_EQ(read_file(p2), "1\nM2\n");
  remove_file(p2);
}

// ============================================================================
//  7. 压力：重复插入 + 幂等（多次调用应可叠加）
// ============================================================================
void test_repeated() {
  std::cout << "[test_repeated]\n";

  const std::string path = make_path("repeated.txt");
  write_file(path, "");

          // 逐次追加到文件头，共 100 次
  for (int i = 0; i < 100; ++i) {
    const std::string d = std::to_string(i) + "\n";
    std::string r = kw::file_insert_line(path.c_str(), 0, d.data(), d.size());
    if (r != "success") {
      ++g_checks; ++g_failures;
      std::cerr << "    FAIL 第 " << i << " 次插入返回: " << r << "\n";
      break;
    }
  }

          // 期望顺序：99, 98, ..., 0
  std::string expect;
  for (int i = 99; i >= 0; --i) {
    expect += std::to_string(i) + "\n";
  }
  CHECK_STR_EQ(read_file(path), expect);
  remove_file(path);
}

} // namespace

// ============================================================
//  极简测试框架
// ============================================================
static int g_pass = 0, g_fail = 0;

#define EXPECT_TRUE(x) do { \
if (x) { ++g_pass; } else { ++g_fail; \
      std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ \
      << "  EXPECT_TRUE(" #x ")\n"; } } while(0)

#define EXPECT_FALSE(x) do { \
  if (!(x)) { ++g_pass; } else { ++g_fail; \
        std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ \
        << "  EXPECT_FALSE(" #x ")\n"; } } while(0)

#define EXPECT_EQ(a, b) do { \
  auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_pass; } else { ++g_fail; \
      std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ \
      << "  EXPECT_EQ(" #a ", " #b ") = '" << _a << "' vs '" << _b << "'\n"; } \
} while(0)

  // ============================================================
  //  测试辅助
  // ============================================================
  std::wstring temp_dir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = ::GetTempPathW(MAX_PATH, buf);
    return std::wstring(buf, n ? n : 0);
  }

std::wstring make_temp_path() {
  wchar_t buf[MAX_PATH] = {0};
  if (!::GetTempFileNameW(temp_dir().c_str(), L"tst", 0, buf))
    std::abort();
  return buf;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), len, nullptr, nullptr);
  return s;
}

void write_file_raw(const std::wstring& path, const std::string& data) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) std::abort();
  DWORD written = 0;
  if (!data.empty())
    ::WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
  ::CloseHandle(h);
}

std::string read_file_raw(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER sz{};
  ::GetFileSizeEx(h, &sz);
  std::string out(static_cast<size_t>(sz.QuadPart), '\0');
  DWORD got = 0;
  if (!out.empty())
    ::ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
  ::CloseHandle(h);
  return out;
}

// ============================================================
//  1. kw::utf8_to_wide 测试
// ============================================================
void test_utf8_null_returns_true_and_clears() {
  std::wstring out = L"leftover";
  EXPECT_TRUE(kw::utf8_to_wide(nullptr, out));
  EXPECT_TRUE(out.empty());
}

void test_utf8_empty_returns_true_and_empty() {
  std::wstring out = L"leftover";
  EXPECT_TRUE(kw::utf8_to_wide("", out));
  EXPECT_TRUE(out.empty());
}

void test_utf8_ascii() {
  std::wstring out;
  EXPECT_TRUE(kw::utf8_to_wide("hello world", out));
  EXPECT_TRUE(out == L"hello world");
}

void test_utf8_bmp_chinese() {
  std::wstring out;
  // "中文" = E4 B8 AD E6 96 87
  EXPECT_TRUE(kw::utf8_to_wide("\xE4\xB8\xAD\xE6\x96\x87", out));
  EXPECT_EQ(out.size(), size_t(2));
  EXPECT_EQ(static_cast<int>(out[0]), 0x4E2D);
  EXPECT_EQ(static_cast<int>(out[1]), 0x6587);
}

void test_utf8_emoji_surrogate_pair() {
  std::wstring out;
  // U+1F600 = F0 9F 98 80 -> UTF-16 D83D DE00
  EXPECT_TRUE(kw::utf8_to_wide("\xF0\x9F\x98\x80", out));
  EXPECT_EQ(out.size(), size_t(2));
  EXPECT_EQ(static_cast<int>(out[0]), 0xD83D);
  EXPECT_EQ(static_cast<int>(out[1]), 0xDE00);
}

void test_utf8_rejects_lone_continuation() {
  std::wstring out = L"garbage";
  EXPECT_FALSE(kw::utf8_to_wide("\x80", out));
  EXPECT_TRUE(out.empty());   // 失败时必须清空
}

void test_utf8_rejects_overlong_encoding() {
  std::wstring out = L"garbage";
  EXPECT_FALSE(kw::utf8_to_wide("\xC0\x80", out));   // overlong NUL
  EXPECT_TRUE(out.empty());
}

void test_utf8_rejects_truncated_multibyte() {
  std::wstring out = L"garbage";
  EXPECT_FALSE(kw::utf8_to_wide("\xE4\xB8", out));   // 3-byte 序列被截断
  EXPECT_TRUE(out.empty());
}

void test_utf8_rejects_utf16_surrogate_range() {
  std::wstring out = L"garbage";
  EXPECT_FALSE(kw::utf8_to_wide("\xED\xA0\x80", out));  // U+D800
  EXPECT_TRUE(out.empty());
}

void test_utf8_rejects_out_of_unicode_range() {
  std::wstring out = L"garbage";
  EXPECT_FALSE(kw::utf8_to_wide("\xF5\x80\x80\x80", out));  // > U+10FFFF
  EXPECT_TRUE(out.empty());
}

void test_utf8_nul_embedded_is_truncated_at_nul() {
  // 因为按 -1 长度调用，遇到嵌入的 '\0' 会停止
  std::wstring out;
  EXPECT_TRUE(kw::utf8_to_wide("ab\0cd", out));
  EXPECT_TRUE(out == L"ab");
}

// ============================================================
//  2. write_all 测试
// ============================================================
void test_write_all_zero_length_succeeds_without_touching_handle() {
  // len == 0 时不应触碰句柄
  EXPECT_TRUE(kw::write_all(INVALID_HANDLE_VALUE, "", 0));
  EXPECT_TRUE(kw::write_all(nullptr, "x", 0));
}

void test_write_all_small() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  {
    kw::UniqueHandle h(::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    EXPECT_TRUE(h.valid());
    EXPECT_TRUE(kw::write_all(h.get(), "hello world", 11));
  }
  EXPECT_TRUE(read_file_raw(p) == "hello world");
}

void test_write_all_exactly_1mb() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  std::string data(1u << 20, 'x');
  {
    kw::UniqueHandle h(::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    EXPECT_TRUE(kw::write_all(h.get(), data.data(), data.size()));
  }
  EXPECT_TRUE(read_file_raw(p) == data);
}

void test_write_all_multi_chunk_boundary() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  std::string data((1u << 20) + 12345, 'y');   // 跨 1MB 边界
  {
    kw::UniqueHandle h(::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    EXPECT_TRUE(kw::write_all(h.get(), data.data(), data.size()));
  }
  EXPECT_TRUE(read_file_raw(p) == data);
}

void test_write_all_invalid_handle() {
  EXPECT_FALSE(kw::write_all(INVALID_HANDLE_VALUE, "abc", 3));
  EXPECT_FALSE(kw::write_all(nullptr, "abc", 3));
}

void test_write_all_readonly_handle() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "seed");
  kw::UniqueHandle h(::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  EXPECT_TRUE(h.valid());
  EXPECT_FALSE(kw::write_all(h.get(), "abc", 3));
}

// ============================================================
//  3. kw::copy_exact 测试
// ============================================================
void test_copy_exact_zero_short_circuits() {
  // count == 0 时直接返回 true，不触碰句柄
  EXPECT_TRUE(kw::copy_exact(nullptr, nullptr, 0));
  EXPECT_TRUE(kw::copy_exact(INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, 0));
}

void test_copy_exact_small() {
  std::wstring src = make_temp_path();
  std::wstring dst = make_temp_path();
  kw::TempFileGuard gs(src.c_str()), gd(dst.c_str());
  write_file_raw(src, "abcdefghij");

  kw::UniqueHandle hi(::CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  kw::UniqueHandle ho(::CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  EXPECT_TRUE(hi.valid() && ho.valid());
  EXPECT_TRUE(kw::copy_exact(hi.get(), ho.get(), 5));
  ho.close();
  EXPECT_TRUE(read_file_raw(dst) == "abcde");
}

void test_copy_exact_large_crosses_buffer() {
  std::wstring src = make_temp_path();
  std::wstring dst = make_temp_path();
  kw::TempFileGuard gs(src.c_str()), gd(dst.c_str());
  std::string data(200 * 1024, 'A');    // 200KB > kCopyBufSize
  for (size_t i = 0; i < data.size(); ++i)
    data[i] = static_cast<char>('A' + (i % 26));
  write_file_raw(src, data);

  kw::UniqueHandle hi(::CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  kw::UniqueHandle ho(::CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  EXPECT_TRUE(kw::copy_exact(hi.get(), ho.get(), data.size()));
  ho.close();
  EXPECT_TRUE(read_file_raw(dst) == data);
}

void test_copy_exact_source_too_short() {
  std::wstring src = make_temp_path();
  std::wstring dst = make_temp_path();
  kw::TempFileGuard gs(src.c_str()), gd(dst.c_str());
  write_file_raw(src, "short");    // 5 bytes

  kw::UniqueHandle hi(::CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  kw::UniqueHandle ho(::CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  ::SetLastError(0);
  EXPECT_FALSE(kw::copy_exact(hi.get(), ho.get(), 10));
  EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_HANDLE_EOF));
}

void test_copy_exact_exact_file_size() {
  std::wstring src = make_temp_path();
  std::wstring dst = make_temp_path();
  kw::TempFileGuard gs(src.c_str()), gd(dst.c_str());
  write_file_raw(src, "12345");

  kw::UniqueHandle hi(::CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  kw::UniqueHandle ho(::CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  EXPECT_TRUE(kw::copy_exact(hi.get(), ho.get(), 5));
  ho.close();
  EXPECT_TRUE(read_file_raw(dst) == "12345");
}

// ============================================================
//  4. dir_of 测试
// ============================================================
void test_dir_of_empty()             { EXPECT_TRUE(kw::dir_of(L"") == L"."); }
void test_dir_of_no_slash()          { EXPECT_TRUE(kw::dir_of(L"file.txt") == L"."); }
void test_dir_of_relative_back()     { EXPECT_TRUE(kw::dir_of(L"a\\b\\file.txt") == L"a\\b"); }
void test_dir_of_relative_fwd()      { EXPECT_TRUE(kw::dir_of(L"a/b/file.txt") == L"a/b"); }
void test_dir_of_drive_root_file()   { EXPECT_TRUE(kw::dir_of(L"C:\\file.txt") == L"C:\\"); }
void test_dir_of_drive_subdir()      { EXPECT_TRUE(kw::dir_of(L"C:\\foo\\bar.txt") == L"C:\\foo"); }
void test_dir_of_drive_root_only()   { EXPECT_TRUE(kw::dir_of(L"C:\\") == L"C:\\"); }
void test_dir_of_forward_slash()     { EXPECT_TRUE(kw::dir_of(L"C:/foo/bar.txt") == L"C:/foo"); }
void test_dir_of_unc_root_file()     { EXPECT_TRUE(kw::dir_of(L"\\\\server\\share\\f.txt") == L"\\\\server\\share"); }
void test_dir_of_unc_subdir()        { EXPECT_TRUE(kw::dir_of(L"\\\\server\\share\\dir\\f.txt") == L"\\\\server\\share\\dir"); }
void test_dir_of_double_backslash()  { EXPECT_TRUE(kw::dir_of(L"\\\\") == L"."); }
void test_dir_of_trailing_slash()    { EXPECT_TRUE(kw::dir_of(L"a/b/") == L"a/b"); }
void test_dir_of_mixed_separators()  { EXPECT_TRUE(kw::dir_of(L"a\\b/c\\file") == L"a\\b/c"); }

// ============================================================
//  5. kw::file_insert_line 测试
// ============================================================

// ---- 参数校验 ----
void test_insert_null_path() {
  EXPECT_TRUE(kw::file_insert_line(nullptr, 0, "x", 1) == "invalid file_path");
}

void test_insert_empty_path() {
  EXPECT_TRUE(kw::file_insert_line("", 0, "x", 1) == "invalid file_path");
}

void test_insert_null_data_with_nonzero_len() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 0, nullptr, 5) == "invalid data");
}

void test_insert_null_data_zero_len_is_ok() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "abc\n");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 1, nullptr, 0) == "success");
  EXPECT_TRUE(read_file_raw(p) == "abc\n");
}

void test_insert_invalid_utf8_path() {
  // 0xFF 是 UTF-8 中无效的首字节
  EXPECT_TRUE(kw::file_insert_line("\xFF\xFE", 0, "x", 1) == "invalid path encoding");
}

void test_insert_nonexistent_file() {
  std::wstring p = make_temp_path();
  ::DeleteFileW(p.c_str());
  std::string utf8_path = wide_to_utf8(p);
  std::string r = kw::file_insert_line(utf8_path.c_str(), 0, "x", 1);
  EXPECT_TRUE(r.rfind("open failed:", 0) == 0);
}

// ---- 空文件 ----
void test_insert_empty_file_line0() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 0, "hello", 5) == "success");
  EXPECT_TRUE(read_file_raw(p) == "hello");
}

void test_insert_empty_file_line5() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 5, "hi\n", 3) == "success");
  EXPECT_TRUE(read_file_raw(p) == "hi\n");
}

// ---- line == 0（插到文件头）----
void test_insert_at_head() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "abc\ndef\n");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 0, "X\n", 2) == "success");
  EXPECT_TRUE(read_file_raw(p) == "X\nabc\ndef\n");
}

void test_insert_at_head_empty_data_is_noop() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  const std::string original = "abc\ndef\n";
  write_file_raw(p, original);
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 0, "", 0) == "success");
  EXPECT_TRUE(read_file_raw(p) == original);
}

// ---- 中间行插入 ----
void test_insert_after_line1() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "abc\ndef\nghi\n");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 1, "X\n", 2) == "success");
  EXPECT_TRUE(read_file_raw(p) == "abc\nX\ndef\nghi\n");
}

void test_insert_after_line2() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "abc\ndef\nghi\n");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 2, "Y\n", 2) == "success");
  EXPECT_TRUE(read_file_raw(p) == "abc\ndef\nY\nghi\n");
}

void test_insert_after_middle_multibyte_data() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "a\nb\nc\n");
  std::string utf8_path = wide_to_utf8(p);
  // "中文\n" = 7 bytes
  const char* data = "\xE4\xB8\xAD\xE6\x96\x87\n";
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 1, data, 7) == "success");
  EXPECT_TRUE(read_file_raw(p) == std::string("a\n") + data + "b\nc\n");
}

// ---- 追加到文件末尾 ----
void test_insert_after_last_line_with_trailing_nl() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "abc\ndef\n");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 2, "Z\n", 2) == "success");
  EXPECT_TRUE(read_file_raw(p) == "abc\ndef\nZ\n");
}

void test_insert_beyond_total_no_trailing_nl_adds_leading_nl() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "abc\ndef");    // 无尾换行
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 100, "Z\n", 2) == "success");
  // 必须补上前导 '\n'，否则 Z 会粘在 def 后面
  EXPECT_TRUE(read_file_raw(p) == "abc\ndef\nZ\n");
}

void test_insert_beyond_total_with_trailing_nl_no_extra() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "abc\ndef\n");  // 尾换行
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 100, "Z\n", 2) == "success");
  EXPECT_TRUE(read_file_raw(p) == "abc\ndef\nZ\n");
}

void test_insert_single_line_no_trailing_nl() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "onlyline");
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 1, "next\n", 5) == "success");
  EXPECT_TRUE(read_file_raw(p) == "onlyline\nnext\n");
}

void test_insert_at_end_zero_len_should_not_add_leading_nl() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  const std::string original = "abc\ndef";   // 无尾换行
  write_file_raw(p, original);
  std::string utf8_path = wide_to_utf8(p);
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 100, "", 0) == "success");
  // 空插入不应导致文件末尾被加上 '\n'
  EXPECT_TRUE(read_file_raw(p) == original);
}

// ---- 大文件 / 大数据 ----
void test_insert_large_data_2mb() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "header\nfooter\n");
  std::string utf8_path = wide_to_utf8(p);

  std::string data(2 * 1024 * 1024, 'D');
  data += '\n';
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 1, data.data(), data.size()) == "success");
  EXPECT_TRUE(read_file_raw(p) == std::string("header\n") + data + "footer\n");
}

void test_insert_into_large_file_100k_lines() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());

  std::string content;
  content.reserve(100000 * 10);
  for (int i = 0; i < 100000; ++i)
    content += "line" + std::to_string(i) + "\n";
  write_file_raw(p, content);
  std::string utf8_path = wide_to_utf8(p);

  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 50000, "INSERTED\n", 9) == "success");

          // 找到第 50000 个 '\n' 之后的位置
  size_t pos = 0;
  for (int i = 0; i < 50000; ++i)
    pos = content.find('\n', pos) + 1;

  std::string expected = content.substr(0, pos) + "INSERTED\n" + content.substr(pos);
  EXPECT_TRUE(read_file_raw(p) == expected);
}

void test_insert_data_containing_nul_bytes() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "a\nb\n");
  std::string utf8_path = wide_to_utf8(p);

  const char data[] = { 'X', '\0', 'Y', '\n' };   // 含 NUL
  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 1,
                               data, sizeof(data)) == "success");

  std::string expected = "a\n";
  expected.append(data, sizeof(data));
  expected += "b\n";
  EXPECT_TRUE(read_file_raw(p) == expected);
}

// ---- 每次调用应只增一次文件大小 ----
void test_insert_repeatedly_accumulates_correctly() {
  std::wstring p = make_temp_path();
  kw::TempFileGuard g(p.c_str());
  write_file_raw(p, "L1\nL2\n");
  std::string utf8_path = wide_to_utf8(p);

  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 1, "A\n", 2) == "success");
  EXPECT_TRUE(read_file_raw(p) == "L1\nA\nL2\n");

  EXPECT_TRUE(kw::file_insert_line(utf8_path.c_str(), 2, "B\n", 2) == "success");
  EXPECT_TRUE(read_file_raw(p) == "L1\nA\nB\nL2\n");
}

// ============================================================
//  测试运行器
// ============================================================
#define RUN(t) do { \
int before = g_fail; \
    std::cout << "[ RUN  ] " #t "\n"; \
    t(); \
    std::cout << (g_fail == before ? "[  OK  ] " : "[ FAIL ] ") << #t "\n"; \
} while(0)

// ============================================================================
//  main
// ============================================================================
int main() {
  // 让 spdlog 安静，测试输出更干净
  spdlog::set_level(spdlog::level::off);

          // 准备临时目录
  std::error_code ec;
  g_tmp_dir = std::filesystem::temp_directory_path(ec)
              / "kw_file_insert_line_tests";
  std::filesystem::create_directories(g_tmp_dir, ec);
  std::filesystem::remove_all(g_tmp_dir, ec);
  std::filesystem::create_directories(g_tmp_dir, ec);

  std::cout << "临时目录: " << path_to_utf8(g_tmp_dir) << "\n\n";

  test_simple_cases();
  test_invalid_args();
  test_buffer_boundaries();
  test_large_data();
  test_binary();
  test_utf8_path();
  test_repeated();

  std::cout << "\n================ 结果 ================\n";
  std::cout << "检查项: " << g_checks
            << "  失败: " << g_failures << "\n";

  std::filesystem::remove_all(g_tmp_dir, ec);

  std::cout << "==== utf8_to_wide ====\n";
  RUN(test_utf8_null_returns_true_and_clears);
  RUN(test_utf8_empty_returns_true_and_empty);
  RUN(test_utf8_ascii);
  RUN(test_utf8_bmp_chinese);
  RUN(test_utf8_emoji_surrogate_pair);
  RUN(test_utf8_rejects_lone_continuation);
  RUN(test_utf8_rejects_overlong_encoding);
  RUN(test_utf8_rejects_truncated_multibyte);
  RUN(test_utf8_rejects_utf16_surrogate_range);
  RUN(test_utf8_rejects_out_of_unicode_range);
  RUN(test_utf8_nul_embedded_is_truncated_at_nul);

  std::cout << "\n==== write_all ====\n";
  RUN(test_write_all_zero_length_succeeds_without_touching_handle);
  RUN(test_write_all_small);
  RUN(test_write_all_exactly_1mb);
  RUN(test_write_all_multi_chunk_boundary);
  RUN(test_write_all_invalid_handle);
  RUN(test_write_all_readonly_handle);

  std::cout << "\n==== copy_exact ====\n";
  RUN(test_copy_exact_zero_short_circuits);
  RUN(test_copy_exact_small);
  RUN(test_copy_exact_large_crosses_buffer);
  RUN(test_copy_exact_source_too_short);
  RUN(test_copy_exact_exact_file_size);

  std::cout << "\n==== dir_of ====\n";
  RUN(test_dir_of_empty);
  RUN(test_dir_of_no_slash);
  RUN(test_dir_of_relative_back);
  RUN(test_dir_of_relative_fwd);
  RUN(test_dir_of_drive_root_file);
  RUN(test_dir_of_drive_subdir);
  RUN(test_dir_of_drive_root_only);
  RUN(test_dir_of_forward_slash);
  RUN(test_dir_of_unc_root_file);
  RUN(test_dir_of_unc_subdir);
  RUN(test_dir_of_double_backslash);
  RUN(test_dir_of_trailing_slash);
  RUN(test_dir_of_mixed_separators);

  std::cout << "\n==== file_insert_line ====\n";
  RUN(test_insert_null_path);
  RUN(test_insert_empty_path);
  RUN(test_insert_null_data_with_nonzero_len);
  RUN(test_insert_null_data_zero_len_is_ok);
  RUN(test_insert_invalid_utf8_path);
  RUN(test_insert_nonexistent_file);
  RUN(test_insert_empty_file_line0);
  RUN(test_insert_empty_file_line5);
  RUN(test_insert_at_head);
  RUN(test_insert_at_head_empty_data_is_noop);
  RUN(test_insert_after_line1);
  RUN(test_insert_after_line2);
  RUN(test_insert_after_middle_multibyte_data);
  RUN(test_insert_after_last_line_with_trailing_nl);
  RUN(test_insert_beyond_total_no_trailing_nl_adds_leading_nl);
  RUN(test_insert_beyond_total_with_trailing_nl_no_extra);
  RUN(test_insert_single_line_no_trailing_nl);
  RUN(test_insert_at_end_zero_len_should_not_add_leading_nl);
  RUN(test_insert_large_data_2mb);
  RUN(test_insert_into_large_file_100k_lines);
  RUN(test_insert_data_containing_nul_bytes);
  RUN(test_insert_repeatedly_accumulates_correctly);

  std::cout << "\n==== summary ====\n";
  std::cout << "passed: " << g_pass << "  failed: " << g_fail << "\n";
}