#ifndef FUNCTION_H
#define FUNCTION_H

//{
//    "name":{
//                "type":"directory/file",
//                "discription":"",
//                "name":{...}
//           },
//    "name":{...}
//}

namespace kw {

namespace fs = std::filesystem;

/*-store_jsonstr-------------------------------------------------------*/
constexpr int kRetryCount = 4;
constexpr auto kRetryBackoff = std::chrono::milliseconds(15);
constexpr std::size_t kMaxFileNameLen = 200;

void sleep_backoff(int attempt);

std::string make_temp_suffix();

// 对已 fileno 的 fd 做 fsync，处理 EINTR
bool fsync_fd(int fd);

// 关键修复：先 fflush 把 stdio 缓冲推进内核，再 fsync 落盘
bool flush_and_sync(std::FILE* fp);

bool sync_directory(const fs::path& dir);

bool atomic_replace(const fs::path& from, const fs::path& to);

bool is_valid_file_name(const std::string& name);

bool store_jsonstr(const std::string& _file_name, const std::string& _jsonstr,
                   const std::string& _prefix);
/*-store_jsonstr-------------------------------------------------------*/

/*-load_jsonstr-------------------------------------------------------*/
constexpr std::uintmax_t kMaxJsonFileSize =
    64ULL * 1024ULL * 1024ULL;  // 64 MiB
constexpr std::size_t kReadChunkSize = 64 * 1024;

void strip_utf8_bom(std::string& s) noexcept;

// JSON 标准要求 UTF-8；遇到 UTF-16/UTF-32 BOM
// 提前拒绝，避免解析器报无意义的错误
bool has_utf16_or_utf32_bom(const std::string& s) noexcept;

// 读取 JSON 文本文件。
// 成功：返回 true，_out_json 为剥离 UTF-8 BOM 后的内容，_err 为空。
// 失败：返回 false，_out_json 清空，_err 为错误原因短句。
bool load_jsonstr(const std::string& _file_path, std::string& _out_json,
                  std::string& _err);
/*-load_jsonstr-------------------------------------------------------*/

/*-exec_cmd-------------------------------------------------------*/

#if defined(__linux__)

struct command_result {
  bool ok = false;
  int exit_code = -1;
  int term_signal = 0;
  bool timed_out = false;
  bool output_truncated = false;
  std::string out, err, sys_error;
};

command_result exec_cmd(const std::string& cmd, int timeout_ms);
#elif defined(_WIN32)
struct command_result {
  int exit_code = -1;
  std::string std_out;
  std::string std_err;
  bool timed_out = false;
  bool truncated = false;  // 输出超过 kMaxCaptureBytes 被截断
};

std::string ensure_utf8(std::string s);

command_result exec_cmd(const std::string& cmd, DWORD timeout_ms = 0);
#endif
/*-exec_cmd-------------------------------------------------------*/

/*-file insert-------------------------------------------------------*/
#if defined(__linux__)

std::string file_insert_line(const char* file_path, int line, const char* data,
                             size_t len);
#elif defined(_WIN32)
constexpr size_t kCopyBufSize = 64 * 1024;
constexpr size_t kScanBufSize = 64 * 1024;

// ---------- RAII 句柄 ----------
class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
  ~UniqueHandle() { close(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& o) noexcept : h_(o.h_) {
    o.h_ = INVALID_HANDLE_VALUE;
  }
  UniqueHandle& operator=(UniqueHandle&& o) noexcept {
    if (this != &o) {
      close();
      h_ = o.h_;
      o.h_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }

  HANDLE get() const noexcept { return h_; }
  bool valid() const noexcept {
    return h_ != INVALID_HANDLE_VALUE && h_ != nullptr;
  }
  void close() noexcept {
    if (valid()) {
      ::CloseHandle(h_);
      h_ = INVALID_HANDLE_VALUE;
    }
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

// ---------- UTF-8 -> UTF-16（严格模式）----------
bool utf8_to_wide(const char* s, std::wstring& out);

// ---------- 完整写入 ----------
bool write_all(HANDLE h, const char* data, size_t len);

// ---------- 从 in 精确拷贝 count 字节到 out ----------
bool copy_exact(HANDLE in, HANDLE out, uint64_t count);
// ---------- 目录 ----------
std::wstring dir_of(const std::wstring& wpath);

// ---------- 临时文件 RAII 守卫 ----------
class TempFileGuard {
 public:
  explicit TempFileGuard(const wchar_t* p) : p_(p) {}
  ~TempFileGuard() {
    if (!dismissed_) ::DeleteFileW(p_);
  }
  void dismiss() { dismissed_ = true; }

 private:
  const wchar_t* p_;
  bool dismissed_ = false;
};

// 将 data 插入到 file_path 的"第 line 行之后"；
// line == 0 表示插到文件头；line 超出总行数则追加到文件尾。
std::string file_insert_line(const char* file_path, size_t line,
                             const char* data, size_t len);

#endif
/*-file insert-------------------------------------------------------*/

/*-file read-------------------------------------------------------*/
#if defined(__linux__)

constexpr int64_t kMaxLineSpan = 100000;
constexpr std::size_t kMaxLineLen = 1u << 20;  // 1 MiB
constexpr std::size_t kReadBufSize = 64 * 1024;

[[nodiscard]] bool fail(std::string& out, std::string msg);

// 线程安全，可移植。等价于 strerror_r，但避免了 GNU/XSI 版本二义性。
std::string errno_str(int e);

void append_padded_int(std::string& dst, int64_t n, int width);

class ScopedFd {
  int fd_{-1};

 public:
  explicit ScopedFd(int fd) noexcept : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) ::close(fd_);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  ScopedFd& operator=(ScopedFd&& o) noexcept {
    if (this != &o) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = o.fd_;
      o.fd_ = -1;
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
};

// 返回 true 表示成功（out 为内容）；false 表示失败（error 为原因）。
// 若 end 超过文件总行数，则返回 true 且内容为 [start, 实际最后行]，
// 实际最后行通过 actual_last 返回。
bool file_read_line(const std::string& file_path, int start, int end,
                    std::string& out, std::string& error);

#elif defined(_WIN32)

constexpr DWORD kBufferSize = 64u * 1024;
constexpr size_t kMaxLineBytes = 1024u * 1024;
constexpr size_t kMaxOutputBytes = 64ull * 1024 * 1024;

std::string WideToUtf8(const wchar_t* w, int len);

bool PathToWide(const std::string& path, std::wstring& out);

std::string Win32ErrorString(DWORD err);

void AppendLine(std::string& out, int lineNo, const std::string& content);

bool file_read_line(const std::string& file_path, int start, int end,
                    std::string& out);

#endif
/*-file read-------------------------------------------------------*/

/*-file delete-------------------------------------------------------*/
#if defined(__linux__)

std::string file_delete_line(const std::string& file_path, int start, int end);

#elif defined(_WIN32)
// UTF-8 std::string -> UTF-16 std::wstring
std::wstring utf8_to_wide(const std::string& s);

struct FileGuard {
  FILE* fp = nullptr;
  explicit FileGuard(FILE* f = nullptr) : fp(f) {}
  ~FileGuard() { reset(); }
  FileGuard(const FileGuard&) = delete;
  FileGuard& operator=(const FileGuard&) = delete;

  FILE* get() const { return fp; }
  void reset(FILE* f = nullptr) {
    if (fp) ::fclose(fp);
    fp = f;
  }
};

struct TempGuard {
  std::wstring path;
  bool committed = false;
  ~TempGuard() {
    if (!committed) ::DeleteFileW(path.c_str());
  }
};

// 若打开时清掉了 READONLY，作用域结束时恢复
struct ReadonlyRestore {
  const std::wstring* path = nullptr;
  bool active = false;
  ~ReadonlyRestore() {
    if (!active || !path) return;
    const DWORD a = ::GetFileAttributesW(path->c_str());
    if (a != INVALID_FILE_ATTRIBUTES) {
      ::SetFileAttributesW(path->c_str(), a | FILE_ATTRIBUTE_READONLY);
    }
  }
};

std::string file_delete_line(const std::string& file_path, int start, int end);
#endif
/*-file delete-------------------------------------------------------*/

/*-ripgrep-------------------------------------------------------*/
#if defined(__linux__)

// extern char** environ;

#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
#define GREPTOOL_HAS_SPAWN_CHDIR 1
#else
#define GREPTOOL_HAS_SPAWN_CHDIR 0
#endif

// ---------------- 对外数据结构 ----------------

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
  std::chrono::milliseconds timeout{30'000};  // 0 => 不设超时；负数视为非法
  std::size_t max_spill_bytes = 256ull * 1024 * 1024;  // 溢出文件字节上限

  // 内联返回的**总字节**上限（行数上限由 GrepTool ctor 的 inline_limit 控制）。
  // 防止 max_line_bytes 配置过大时，250 行 * 大行把调用方内存打爆。
  std::size_t inline_bytes_limit = 8ull * 1024 * 1024;

  // true : --sort=path 保证严格字典序，但 rg 会在内存里缓冲全部命中。
  // false: -j1 单线程按遍历顺序逐文件输出，内存恒定，仍保证按文件分组。
  // 生产环境默认 false，避免大搜索 OOM。
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

// ============================================================
//  greptool.cpp
// ============================================================

constexpr std::size_t kErrCap = 64 * 1024;
constexpr std::size_t kMaxPartialExtra = 4 * 1024;

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
    // 进程组 + 单进程双重 SIGKILL，覆盖 rg 启动的孙进程。
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    pid = -1;
  }
};

struct TempFile {
  int fd = -1;
  std::string path;
  std::size_t bytes = 0;
  ~TempFile() {
    if (fd >= 0) ::close(fd);
  }  // 不 unlink；成功路径由调用方通过 saved_path 接管

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

#elif defined(_WIN32)

// ------------------------------------------------------------------
// 返回结构
// ------------------------------------------------------------------
struct GrepResult {
  bool ok = false;           // 调用成功（rg 退出码 0 或 1）
  std::string content;       // 供 LLM/调用方直接使用的文本
  size_t totalMatches = 0;   // 匹配行总数（不含文件标题行）
  bool truncated = false;    // 是否被 250 行截断
  std::string fullListPath;  // 被截断时，完整匹配列表保存路径
  int exitCode = 0;
  std::string error;  // 出错时的信息
};

// ---------------- UTF-8 <-> UTF-16 ----------------
std::wstring utf8ToWide(const std::string& s);

std::string wideToUtf8(const std::wstring& w);

// ---------------- Windows 命令行参数转义（MSVC CRT 规则） ----------------
// 规则：包含空格/引号才需要加引号；反斜杠只在引号前才需要翻倍。
std::wstring quoteArg(const std::wstring& arg);

// ---------------- 判断一行是否为 “行号:内容” 的匹配行 ----------------
// rg --heading 输出：
//   path/to/file            <-- 文件标题行
//   12:match content        <-- 匹配行
//   15:another match
//   (空行)
bool isMatchLine(const std::string& line);

// ---------------- RAII 句柄 ----------------
struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE x) : h(x) {}
  ~Handle() {
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }
  Handle& operator=(Handle&& o) noexcept {
    if (this != &o) {
      if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
      h = o.h;
      o.h = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  operator HANDLE() const { return h; }
  HANDLE* addr() { return &h; }
  bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

GrepResult grep(const std::string& pattern, const std::string& path = "",
                const std::string& include = "", size_t max = 250);

#endif
/*-ripgrep-------------------------------------------------------*/

};  // namespace kw

#endif
