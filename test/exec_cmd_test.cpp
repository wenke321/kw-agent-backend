#include "helpers/function.h"
#include <future>
#include <iostream>

struct TestCtx {
  const char* name = "";
  int checks = 0;
  int fails  = 0;
};
TestCtx* g_ctx = nullptr;
int g_total_checks = 0;
int g_total_fails  = 0;

#define CHECK(cond) do {                                                   \
if (g_ctx) ++g_ctx->checks;                                            \
    if (!(cond)) {                                                         \
      if (g_ctx) ++g_ctx->fails;                                         \
      std::printf("  [FAIL] %s:%d  CHECK(%s)\n",                         \
                  __FILE__, __LINE__, #cond);                            \
}                                                                      \
} while (0)

#define CHECK_MSG(cond, msg) do {                                          \
  if (g_ctx) ++g_ctx->checks;                                            \
    if (!(cond)) {                                                         \
      if (g_ctx) ++g_ctx->fails;                                         \
      std::printf("  [FAIL] %s:%d  %s  (%s)\n",                          \
                  __FILE__, __LINE__, #cond, (msg));                     \
}                                                                      \
} while (0)

#define RUN(fn) do {                                                       \
  TestCtx ctx{#fn, 0, 0};                                                \
    g_ctx = &ctx;                                                          \
    std::printf("\n[ RUN  ] %s\n", #fn);                                   \
    try { fn(); }                                                          \
    catch (const std::exception& e) {                                      \
      ctx.fails++;                                                       \
      std::printf("  [FAIL] uncaught: %s\n", e.what());                  \
}                                                                      \
    std::printf("[ %s ] %s  (checks=%d, fails=%d)\n",                      \
                                                                               ctx.fails ? "FAIL" : " OK ", #fn, ctx.checks, ctx.fails);          \
    g_total_checks += ctx.checks;                                          \
    g_total_fails  += ctx.fails;                                           \
    g_ctx = nullptr;                                                       \
} while (0)

  // ---- 辅助 ----

  std::string u8_bytes(std::initializer_list<unsigned char> v) {
    std::string s;
    s.reserve(v.size());
    for (unsigned char c : v) s.push_back(static_cast<char>(c));
    return s;
  }

// 严格 UTF-8 合法性检查（含 overlong / 代理区 / 越界）
bool is_valid_utf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int extra = 0;
    if      (c < 0x80)                  extra = 0;
    else if ((c & 0xE0) == 0xC0) { if (c < 0xC2) return false; extra = 1; }
    else if ((c & 0xF0) == 0xE0)        extra = 2;
    else if ((c & 0xF8) == 0xF0) { if (c > 0xF4) return false; extra = 3; }
    else return false;
    if (i + static_cast<size_t>(extra) >= s.size()) return false;
    for (int k = 1; k <= extra; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    // 禁 overlong / surrogate / > U+10FFFF
    if (extra == 2) {
      unsigned u = (static_cast<unsigned>(c) & 0x0F) << 12 |
                   (static_cast<unsigned>(static_cast<unsigned char>(s[i+1])) & 0x3F) << 6 |
                   (static_cast<unsigned>(static_cast<unsigned char>(s[i+2])) & 0x3F);
      if (u < 0x800 || (u >= 0xD800 && u <= 0xDFFF)) return false;
    }
    i += 1 + static_cast<size_t>(extra);
  }
  return true;
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

int count_crlf(const std::string& s) {
  int n = 0;
  for (size_t i = 0; i + 1 < s.size(); ++i)
    if (s[i] == '\r' && s[i + 1] == '\n') ++n;
  return n;
}

// ==================== 基础功能 ====================

void test_echo_hello() {
  auto r = kw::exec_cmd("echo hello", 5000);
  CHECK(!r.timed_out);
  CHECK(r.exit_code == 0);
  CHECK(r.std_out.find("hello") != std::string::npos);
  CHECK(r.std_err.empty());
}

void test_nonzero_exit() {
  auto r = kw::exec_cmd("exit /b 42", 5000);
  CHECK(!r.timed_out);
  CHECK(r.exit_code == 42);
}

void test_stderr_capture() {
  auto r = kw::exec_cmd("echo err_marker 1>&2", 5000);
  CHECK(r.exit_code == 0);
  CHECK(r.std_err.find("err_marker")  != std::string::npos);
  CHECK(r.std_out.find("err_marker") == std::string::npos);
}

// ==================== 边界与错误 ====================

void test_empty_command() {
  auto r = kw::exec_cmd("", 5000);
  CHECK(r.exit_code == -1);
  CHECK(!r.std_err.empty());
  CHECK(!r.timed_out);
}

void test_nul_in_command() {
  std::string cmd = "echo hi";
  cmd.push_back('\0');
  cmd += "tail";
  auto r = kw::exec_cmd(cmd, 5000);
  CHECK(r.exit_code == -1);
  CHECK(r.std_err.find("NUL") != std::string::npos);
}

void test_invalid_utf8_command() {
  // 0xFF 是非法 UTF-8 前导字节，0x80 是孤立 continuation
  std::string cmd = "echo " + u8_bytes({0xFF, 0xFE, 0xFD, 0x80});
  auto r = kw::exec_cmd(cmd, 5000);
  CHECK(r.exit_code == -1);
  CHECK(r.std_err.find("UTF-8") != std::string::npos);
}

void test_special_chars() {
  // 带引号、带 &，验证 argv/引号处理
  auto r = kw::exec_cmd("echo \"hello world\"", 5000);
  CHECK(r.exit_code == 0);
  CHECK(r.std_out.find("hello world") != std::string::npos);

  auto r2 = kw::exec_cmd("echo a&echo b", 5000);
  CHECK(r2.exit_code == 0);
  CHECK(r2.std_out.find("a") != std::string::npos);
  CHECK(r2.std_out.find("b") != std::string::npos);
}

void test_pipe() {
  auto r = kw::exec_cmd("echo hello | findstr hello", 5000);
  CHECK(r.exit_code == 0);
  CHECK(r.std_out.find("hello") != std::string::npos);
}

void test_empty_output() {
  auto r = kw::exec_cmd("ver > nul", 5000);
  CHECK(r.exit_code == 0);
  CHECK(r.std_out.empty());
}

void test_command_too_long() {
  // cmd.exe /c 参数上限 8191，超长应在 CreateProcess 前被拒
  std::string cmd = "echo ";
  cmd.append(9000, 'a');
  auto r = kw::exec_cmd(cmd, 5000);
  CHECK(r.exit_code == -1);
  CHECK(r.std_err.find("too long") != std::string::npos);
}

// ==================== 编码 ====================

void test_utf8_command_accepted() {
  // 合法 UTF-8 字节（"你好"）。cmd 在 OEM 下会解释成别的字节，
  // 但 kw::exec_cmd 不应报 "not valid UTF-8"。
  std::string cmd = "echo " + u8_bytes({0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD});
  auto r = kw::exec_cmd(cmd, 5000);
  CHECK(r.std_err.find("not valid UTF-8") == std::string::npos);
  CHECK(!r.timed_out);
}

void test_output_is_valid_utf8() {
  // 输出（无论来自 ASCII、OEM 还是 UTF-16 BOM 源）都应是合法 UTF-8
  auto r = kw::exec_cmd("echo hello", 5000);
  CHECK(is_valid_utf8(r.std_out));
  CHECK(is_valid_utf8(r.std_err));

          // stderr 也验证一次
  auto r2 = kw::exec_cmd("echo err 1>&2", 5000);
  CHECK(is_valid_utf8(r2.std_out));
  CHECK(is_valid_utf8(r2.std_err));
}

void test_oem_to_utf8_conversion() {
  // 用 chcp 输出当前代码页信息（OEM 编码），确保转换路径被触发
  // 不依赖具体代码页内容，只验证结果是合法 UTF-8
  auto r = kw::exec_cmd("chcp", 5000);
  CHECK(r.exit_code == 0);
  CHECK(!r.std_out.empty());
  CHECK(is_valid_utf8(r.std_out));
}

// ==================== 超时与进程管理 ====================

void test_timeout() {
  auto t0 = now_ms();
  // ping 每 1 秒一次，-n 5 大概 4 秒
  auto r = kw::exec_cmd("ping -n 5 127.0.0.1 > nul", 300);
  auto dt = now_ms() - t0;
  CHECK(r.timed_out);
  CHECK_MSG(dt < 5000, "timeout must not wait for child completion");
  CHECK(r.exit_code != 0);
}

void test_timeout_precision() {
  auto t0 = now_ms();
  auto r = kw::exec_cmd("ping -n 10 127.0.0.1 > nul", 200);
  auto dt = now_ms() - t0;
  CHECK(r.timed_out);
  CHECK_MSG(dt >= 150, "should honor the timeout");
  CHECK_MSG(dt < 5000, "should not wait for 9s ping to finish");
}

void test_timeout_kills_child() {
  // 超时后子进程必须被终止；用 tasklist 现场抽查不可靠，
  // 改为：超时返回后立刻再跑一个短命令，能正常完成即证明
  // 超时路径不污染后续执行。
  auto r = kw::exec_cmd("ping -n 10 127.0.0.1 > nul", 300);
  CHECK(r.timed_out);
  auto r2 = kw::exec_cmd("echo after_timeout", 5000);
  CHECK(!r2.timed_out);
  CHECK(r2.exit_code == 0);
  CHECK(r2.std_out.find("after_timeout") != std::string::npos);
}

void test_background_grandchild() {
  // start /b 启动后台 cmd /c ping，孙进程会持有管道写端；
  // Job 生效时 TerminateJobObject 立刻杀掉孙进程；
  // Job 不生效时 join_reader 宽限期兜底；
  // 任一路径都不允许卡死 kw::exec_cmd。
  auto t0 = now_ms();
  auto r = kw::exec_cmd(
      "start /b cmd /c \"ping -n 5 127.0.0.1 > nul\" & echo done",
      10000);
  auto dt = now_ms() - t0;
  CHECK(!r.timed_out);
  CHECK(r.exit_code == 0);
  CHECK(r.std_out.find("done") != std::string::npos);
  CHECK_MSG(dt < 15000, "must not hang on grandchild holding pipe");
}

// ==================== 压力与并发 ====================

void test_large_output() {
  // 10000 行，约 100KB，验证输出完整、顺序正确、无丢失
  auto r = kw::exec_cmd("for /l %i in (1,1,10000) do @echo line%i", 30000);
  CHECK(r.exit_code == 0);
  CHECK(!r.timed_out);
  CHECK(r.std_out.find("line1\r\n")    != std::string::npos);
  CHECK(r.std_out.find("line10000\r\n") != std::string::npos);
  CHECK_MSG(count_crlf(r.std_out) == 10000,
            "all 10000 lines must be captured");
  CHECK(!r.truncated);
}

void test_stdout_stderr_concurrent() {
  // 双路各 2000 行，总量远超管道缓冲（128KB），
  // 若单线程串行读会死锁；双线程读应全部捕获。
  auto r = kw::exec_cmd(
      "powershell -NoProfile -Command "
      "\"for($i=1;$i -le 2000;$i++){"
      "Write-Output ('out'+$i); "
      "[Console]::Error.WriteLine('err'+$i)}\"",
      60000);
  CHECK(r.exit_code == 0);
  CHECK(r.std_out.find("out1\r\n")    != std::string::npos);
  CHECK(r.std_out.find("out2000\r\n") != std::string::npos);
  CHECK(r.std_err.find("err1\r\n")    != std::string::npos);
  CHECK(r.std_err.find("err2000\r\n") != std::string::npos);
  CHECK(count_crlf(r.std_out) == 2000);
  CHECK(count_crlf(r.std_err) == 2000);
}

void test_concurrent() {
  constexpr int N = 8;
  std::vector<std::future<kw::command_result>> futs; for (int i = 0; i < N; ++i) {
    futs.push_back(std::async(std::launch::async, [i]() {
      return kw::exec_cmd("echo test" + std::to_string(i), 5000);
    }));
  }
  for (int i = 0; i < N; ++i) {
    auto r = futs[static_cast<size_t>(i)].get();
    CHECK(r.exit_code == 0);
    CHECK(r.std_out.find("test" + std::to_string(i)) != std::string::npos);
  }
}

void test_stress_repeat() {
  // 连续 20 次，验证无句柄泄漏 / 线程泄漏
  for (int i = 0; i < 20; ++i) {
    auto r = kw::exec_cmd("echo run" + std::to_string(i), 5000);
    CHECK(r.exit_code == 0);
    CHECK(r.std_out.find("run" + std::to_string(i)) != std::string::npos);
    CHECK(!r.timed_out);
  }
}

void test_comspec_fallback() {
  // 临时清空 COMSPEC，验证回退到 System32\cmd.exe（绝对路径）
  wchar_t buf[4096];
  DWORD n = GetEnvironmentVariableW(L"COMSPEC", buf, 4096);
  std::wstring saved = (n > 0 && n < 4096) ? std::wstring(buf, n) : std::wstring();
  SetEnvironmentVariableW(L"COMSPEC", L"");

  auto r = kw::exec_cmd("echo fallback_ok", 5000);

  if (!saved.empty()) SetEnvironmentVariableW(L"COMSPEC", saved.c_str());
  else                SetEnvironmentVariableW(L"COMSPEC", nullptr);

  CHECK(r.exit_code == 0);
  CHECK(r.std_out.find("fallback_ok") != std::string::npos);
}


// ============================================================
// 迷你测试框架
// ============================================================
static int g_pass = 0, g_fail = 0;

void check(bool cond, const std::string &name, const std::string &detail = "") {
  std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name;
  if (!cond && !detail.empty()) std::cout << "   (" << detail << ")";
  std::cout << "\n";
  if (cond) ++g_pass; else ++g_fail;
}

static bool contains(const std::string &s, const std::string &sub) {
  return s.find(sub) != std::string::npos;
}

static long long elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start).count();
}

static void header(const char *name) {
  std::cout << "\n[ RUN  ] " << name << "\n";
}

// ============================================================
// 正常路径
// ============================================================
void test_echo_basic() {
  header("test_echo_basic");
  auto r = kw::exec_cmd("echo hello_world");
  check(!r.timed_out,          "not timed out");
  check(r.exit_code == 0,      "exit_code == 0", "got " + std::to_string(r.exit_code));
  check(contains(r.std_out, "hello_world"), "stdout contains hello_world");
}

void test_exit_code() {
  header("test_exit_code");
  auto r = kw::exec_cmd("exit 42");
  check(!r.timed_out,     "not timed out");
  check(r.exit_code == 42, "exit_code == 42", "got " + std::to_string(r.exit_code));
}

void test_stderr_capture_new() {
  header("test_stderr_capture");
  auto r = kw::exec_cmd("echo err_msg_xyz 1>&2");
  check(r.exit_code == 0, "exit_code == 0");
  check(contains(r.std_err, "err_msg_xyz"),
        "stderr contains err_msg_xyz",
        "stderr = [" + r.std_err + "]");
  check(r.std_out.find("err_msg_xyz") == std::string::npos,
        "stdout does NOT contain err_msg_xyz");
}

void test_internal_command() {
  header("test_internal_command");
  // dir 是 cmd 内部命令，验证 cmd.exe /c 前缀确实有效
  auto r = kw::exec_cmd("dir /b");
  check(r.exit_code == 0, "exit_code == 0");
  check(!r.std_out.empty(), "stdout not empty");
}

void test_unknown_command() {
  header("test_unknown_command");
  auto r = kw::exec_cmd("this_command_definitely_does_not_exist_xyz");
  check(!r.timed_out,        "not timed out");
  check(r.exit_code != 0,    "exit_code != 0",
        "got " + std::to_string(r.exit_code));
}

// ============================================================
// 超时逻辑
// ============================================================
void test_no_timeout_when_fast() {
  header("test_no_timeout_when_fast");
  // 命令很快结束，超时 5 秒不应触发
  auto t0 = std::chrono::steady_clock::now();
  auto r  = kw::exec_cmd("echo fast", 5000);
  auto el = elapsed_ms(t0);
  check(!r.timed_out,       "not timed out");
  check(r.exit_code == 0,   "exit_code == 0");
  check(el < 3000,          "returned quickly",
        "elapsed = " + std::to_string(el) + " ms");
}

void test_zero_means_infinite() {
  header("test_zero_means_infinite");
  // timeout_ms == 0 兼容旧语义：无限等待
  auto r = kw::exec_cmd("echo infinite_wait", 0);
  check(!r.timed_out,     "not timed out");
  check(r.exit_code == 0, "exit_code == 0");
}

void test_simple_timeout() {
  header("test_simple_timeout");
  // ping -n 20 约 19 秒，1.5 秒必定触发超时
  auto t0 = std::chrono::steady_clock::now();
  auto r  = kw::exec_cmd("ping -n 20 127.0.0.1", 1500);
  auto el = elapsed_ms(t0);
  check(r.timed_out,  "timed_out == true");
  check(el >= 1400,   "waited at least the timeout",
        "elapsed = " + std::to_string(el) + " ms");
  check(el < 4000,    "did not hang past the timeout",
        "elapsed = " + std::to_string(el) + " ms");
}

void test_process_tree_timeout() {
  header("test_process_tree_timeout");
  // 关键测试：ping 是 cmd.exe 的子进程，并且继承了 stdout/stderr 写句柄。
  // 如果只 TerminateProcess(cmd.exe)，ping 会继续持有管道写端，
  // 两个读取线程会一直阻塞在 ReadFile 上 —— 整个函数就会挂住 30 秒。
  // Job Object 生效时，TerminateJobObject 会一次干掉整棵树。
  auto t0 = std::chrono::steady_clock::now();
  auto r  = kw::exec_cmd("ping -n 30 127.0.0.1", 1000);
  auto el = elapsed_ms(t0);

  check(r.timed_out, "timed_out == true");
  check(el < 6000,
        "returned quickly (job object killed the whole tree)",
        "elapsed = " + std::to_string(el) + " ms  (expected < 6000, hang would be ~30000)");
}

void test_timeout_with_output() {
  header("test_timeout_with_output");
  // 子进程持续产生输出，我们在它写到一半时把它杀掉。
  // 管道中缓冲的数据应被读取线程完整读出，且函数要能正常返回。
  auto t0 = std::chrono::steady_clock::now();
  auto r  = kw::exec_cmd(
      "for /L %i in (1,1,100000000) do "
      "@echo line_%i_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      1500);
  auto el = elapsed_ms(t0);

  check(r.timed_out,          "timed_out == true");
  check(el < 6000,            "returned quickly",
        "elapsed = " + std::to_string(el) + " ms");
  check(!r.std_out.empty(),   "captured partial stdout",
        "std_out.size() = " + std::to_string(r.std_out.size()));
  std::cout << "          captured " << r.std_out.size() << " bytes before timeout\n";
}

// ============================================================
// 大输出 / 双流并发（防死锁）
// ============================================================
void test_large_stdout() {
  header("test_large_stdout");
  // 约 5 万行 × ~70 字节 ≈ 3.5MB，远超 64KB 管道缓冲区。
  // 若没有并发读取线程，子进程写满缓冲区后会阻塞，造成死锁。
  auto t0 = std::chrono::steady_clock::now();
  auto r  = kw::exec_cmd(
      "for /L %i in (1,1,50000) do "
      "@echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  auto el = elapsed_ms(t0);

  check(r.exit_code == 0,          "exit_code == 0");
  check(!r.timed_out,              "not timed out");
  check(r.std_out.size() > 1000000,
        "captured > 1 MB of stdout",
        "size = " + std::to_string(r.std_out.size()));
  std::cout << "          captured " << r.std_out.size() << " bytes in "
            << el << " ms\n";
}

void test_large_mixed_streams() {
  header("test_large_mixed_streams");
  // 同时往 stdout 和 stderr 写大量数据。若只用单线程顺序读取，
  // 一定会在某个管道上死锁。
  auto r = kw::exec_cmd(
      "for /L %i in (1,1,5000) do "
      "@(echo STDOUT_LINE_%i & echo STDERR_LINE_%i 1>&2)");

  check(r.exit_code == 0, "exit_code == 0");
  check(!r.timed_out,     "not timed out");
  check(contains(r.std_out, "STDOUT_LINE_5000"),
        "stdout complete (has last line)",
        "stdout size = " + std::to_string(r.std_out.size()));
  check(contains(r.std_err, "STDERR_LINE_5000"),
        "stderr complete (has last line)",
        "stderr size = " + std::to_string(r.std_err.size()));
}

// ============================================================
// 超时后的状态恢复
// ============================================================
void test_state_clean_after_timeout() {
  header("test_state_clean_after_timeout");
  // 先制造一次超时
  auto r1 = kw::exec_cmd("ping -n 20 127.0.0.1", 800);
  check(r1.timed_out, "first call timed out");

          // 紧接着执行一个普通命令，应完全正常
  auto r2 = kw::exec_cmd("echo back_to_normal");
  check(!r2.timed_out,     "second call not timed out");
  check(r2.exit_code == 0, "second call exit_code == 0",
        "got " + std::to_string(r2.exit_code));
  check(contains(r2.std_out, "back_to_normal"),
        "second call stdout correct");
}

void test_repeated_timeouts() {
  header("test_repeated_timeouts");
  // 连续多次超时，验证没有句柄泄漏或状态污染
  for (int i = 0; i < 5; ++i) {
    auto r = kw::exec_cmd("ping -n 20 127.0.0.1", 300);
    if (!r.timed_out) {
      check(false, "iteration " + std::to_string(i) + " timed out");
      return;
    }
  }
  check(true, "5 consecutive timeouts all reported timed_out");
}

// ============================================================
// 追加测试 1: 超时精度
// ============================================================
void test_timeout_precision_new() {
  header("test_timeout_precision");
  // timeout 只有 50ms，ping -n 3 至少要跑 2 秒。
  // 断言的上下界都很紧：下界说明确实等到了超时时间，
  // 上界（500ms）说明调度延迟没把超时拖太久。
  auto t0 = std::chrono::steady_clock::now();
  auto r  = kw::exec_cmd("ping -n 3 127.0.0.1", 50);
  auto el = elapsed_ms(t0);

  check(r.timed_out,  "timed_out == true");
  check(el >= 45,     "waited at least ~50ms",
        "elapsed = " + std::to_string(el) + " ms");
  check(el < 500,     "did not overshoot the timeout",
        "elapsed = " + std::to_string(el) + " ms");
  std::cout << "          elapsed = " << el << " ms (target ~50ms)\n";
}

// ============================================================
// 追加测试 2: 大超时值
// ============================================================
void test_large_timeout_value() {
  header("test_large_timeout_value");
  // 10 分钟的超时值，命令瞬间完成。
  // 验证大 DWORD 值不会溢出 / 被截断成负值或小值。
  // 命令必须在 ms 级返回，否则说明超时计算有问题。
  DWORD bigTimeout = 10u * 60u * 1000u;   // 600000

  auto t0 = std::chrono::steady_clock::now();
  auto r  = kw::exec_cmd("echo done", bigTimeout);
  auto el = elapsed_ms(t0);

  check(!r.timed_out,     "not timed out");
  check(r.exit_code == 0, "exit_code == 0",
        "got " + std::to_string(r.exit_code));
  check(contains(r.std_out, "done"), "stdout contains done");
  check(el < 3000,        "returned immediately, not blocked",
        "elapsed = " + std::to_string(el) + " ms");

          // 再验证一个接近 INFINITE-1 的极大值不会把等待变成立即超时
          // 注意：INFINITE 是 0xFFFFFFFF，我们传 0xFFFFFFFE，语义上是“等很久”，
          // 命令瞬间完成，所以同样应立即返回且 timed_out == false。
  auto t1 = std::chrono::steady_clock::now();
  auto r2 = kw::exec_cmd("echo huge", 0xFFFFFFFEu);
  auto el2 = elapsed_ms(t1);

  check(!r2.timed_out,     "huge timeout: not timed out");
  check(r2.exit_code == 0, "huge timeout: exit_code == 0");
  check(el2 < 3000,        "huge timeout: returned immediately",
        "elapsed = " + std::to_string(el2) + " ms");
}

// ============================================================
// 追加测试 3: 从外部验证进程树残留
// ============================================================
void test_no_orphan_process() {
  header("test_no_orphan_process");

          // 前置检查：确认当前系统里没有我们待会要检测的同名进程，
          // 否则测试结果不可信。
  {
    auto pre = kw::exec_cmd("tasklist /FI \"IMAGENAME eq PING.EXE\" /NH");
    if (contains(pre.std_out, "PING.EXE")) {
      check(false, "precondition: no pre-existing PING.EXE",
            "found PING.EXE before starting, test result unreliable");
      return;
    }
  }

          // 制造一次超时。ping -n 30 会跑 ~29 秒，我们 800ms 就掐掉。
  auto r = kw::exec_cmd("ping -n 30 127.0.0.1", 800);
  check(r.timed_out, "timed_out == true");

          // 立即用 tasklist 从外部查证 PING.EXE 是否已被清掉。
          // 这里只等很短的时间，因为 Job Object 是同步终止的。
          // 但如果 cmd.exe 已经开始派生子进程而 ping 尚未完全创建，
          // tasklist 可能恰好看到中间态，因此给一点点缓冲。
  Sleep(200);

  auto post = kw::exec_cmd("tasklist /FI \"IMAGENAME eq PING.EXE\" /NH");
  bool stillThere = contains(post.std_out, "PING.EXE");

  check(!stillThere,
        "no PING.EXE remains after timeout",
        stillThere ? "post-check stdout: " + post.std_out : "");

          // 顺带确认 cmd.exe 也是干净退出的，反复执行几次进一步压实。
  for (int i = 0; i < 3; ++i) {
    auto rr = kw::exec_cmd("ping -n 30 127.0.0.1", 500);
    if (!rr.timed_out) {
      check(false, "iteration " + std::to_string(i) + " timed out");
      return;
    }
    Sleep(100);
    auto chk = kw::exec_cmd("tasklist /FI \"IMAGENAME eq PING.EXE\" /NH");
    if (contains(chk.std_out, "PING.EXE")) {
      check(false, "iteration " + std::to_string(i) + ": no orphan PING.EXE",
            "post-check stdout: " + chk.std_out);
      return;
    }
  }
  check(true, "3 more rounds: no orphan PING.EXE after each timeout");
}

// ============================================================
// 追加测试 4: 并发调用
// ============================================================
void test_concurrent_calls() {
  header("test_concurrent_calls");
  // 同时发起 4 个 kw::exec_cmd。实现里只用了局部变量和各自的 Job，
  // 没有任何共享可变状态，应该完全线程安全。
  // 每个命令的输出用独特的标记串，方便验证没有互相串数据。

  const int N = 4;

  auto worker = [](int id) {
    std::string tag = "MARKER_" + std::to_string(id) + "_" + std::string(60, 'A' + id);
    std::string cmd = "for /L %i in (1,1,200) do @echo " + tag;
    auto r = kw::exec_cmd(cmd, 30000);
    return std::make_pair(id, std::move(r));
  };

  std::vector<std::future<std::pair<int, kw::command_result>>> futs;
  futs.reserve(N);
  for (int i = 0; i < N; ++i) {
    futs.push_back(std::async(std::launch::async, worker, i));
  }

  for (auto &f : futs) {
    auto [id, r] = f.get();
    std::string tag = "MARKER_" + std::to_string(id) + "_" +
                      std::string(60, 'A' + id);

    check(r.exit_code == 0, "worker " + std::to_string(id) + " exit_code == 0",
          "got " + std::to_string(r.exit_code));
    check(!r.timed_out,     "worker " + std::to_string(id) + " not timed out");
    check(contains(r.std_out, tag),
          "worker " + std::to_string(id) + " stdout has its own marker");

            // 其它三个 worker 的 marker 不应出现在此输出里
    for (int j = 0; j < N; ++j) {
      if (j == id) continue;
      std::string otherTag = "MARKER_" + std::to_string(j) + "_" +
                             std::string(60, 'A' + j);
      if (contains(r.std_out, otherTag)) {
        check(false,
              "worker " + std::to_string(id) + " stdout has no worker " +
                  std::to_string(j) + " data",
              "cross-contamination detected");
      }
    }
  }
  check(true, "4 concurrent workers completed without cross-contamination");
}

// ============================================================
// 追加测试 4b: 并发 + 超时混合
// ============================================================
void test_concurrent_with_timeout() {
  header("test_concurrent_with_timeout");
  // 一半的 worker 会超时，一半会正常完成。
  // 验证一个 worker 超时不影响其它 worker 的结果。
  auto fastWorker = []() {
    return kw::exec_cmd("echo fast_done");
  };
  auto slowWorker = []() {
    return kw::exec_cmd("ping -n 30 127.0.0.1", 700);
  };

  auto f1 = std::async(std::launch::async, fastWorker);
  auto f2 = std::async(std::launch::async, slowWorker);
  auto f3 = std::async(std::launch::async, fastWorker);
  auto f4 = std::async(std::launch::async, slowWorker);

  auto r1 = f1.get();
  auto r2 = f2.get();
  auto r3 = f3.get();
  auto r4 = f4.get();

  check(!r1.timed_out && r1.exit_code == 0, "fast worker 1 completed");
  check(!r3.timed_out && r3.exit_code == 0, "fast worker 2 completed");
  check(r2.timed_out, "slow worker 1 timed out");
  check(r4.timed_out, "slow worker 2 timed out");
  check(contains(r1.std_out, "fast_done") && contains(r3.std_out, "fast_done"),
        "both fast workers produced their output");
}

// ============================================================
// main
// ============================================================
int main() {
  std::cout << "========================================\n"
            << " kw::kw::exec_cmd test suite\n"
            << "========================================\n";

          // 正常路径
  test_echo_basic();
  test_exit_code();
  test_stderr_capture();
  test_internal_command();
  test_unknown_command();

          // 超时逻辑
  test_no_timeout_when_fast();
  test_zero_means_infinite();
  test_simple_timeout();
  test_process_tree_timeout();
  test_timeout_with_output();

          // 大输出 / 双流并发
  test_large_stdout();
  test_large_mixed_streams();

          // 超时后的状态恢复
  test_state_clean_after_timeout();
  test_repeated_timeouts();

  // ---- 追加的 4 项 ----
  test_timeout_precision();       // 1
  test_large_timeout_value();     // 2
  test_no_orphan_process();       // 3
  test_concurrent_calls();        // 4
  test_concurrent_with_timeout(); // 4b

  // 基础
  RUN(test_echo_hello);
  RUN(test_nonzero_exit);
  RUN(test_stderr_capture_new);

          // 边界
  RUN(test_empty_command);
  RUN(test_nul_in_command);
  RUN(test_invalid_utf8_command);
  RUN(test_special_chars);
  RUN(test_pipe);
  RUN(test_empty_output);
  RUN(test_command_too_long);

          // 编码
  RUN(test_utf8_command_accepted);
  RUN(test_output_is_valid_utf8);
  RUN(test_oem_to_utf8_conversion);

          // 超时与进程管理
  RUN(test_timeout);
  RUN(test_timeout_precision_new);
  RUN(test_timeout_kills_child);
  RUN(test_background_grandchild);

          // 压力与并发
  RUN(test_large_output);
  RUN(test_stdout_stderr_concurrent);
  RUN(test_concurrent);
  RUN(test_stress_repeat);
  RUN(test_comspec_fallback);

  std::printf("\n===== Summary: %d checks, %d failed =====\n",
              g_total_checks, g_total_fails);
  return g_total_fails == 0 ? 0 : 1;

  std::cout << "\n========================================\n"
            << " SUMMARY\n"
            << "   PASS: " << g_pass << "\n"
            << "   FAIL: " << g_fail << "\n"
            << "========================================\n";
  return g_fail == 0 ? 0 : 1;
}