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
constexpr int  kRetryCount   = 4;
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

bool store_jsonstr(const std::string& _file_name,
                   const std::string& _jsonstr,
                          const std::string& _prefix);
/*-store_jsonstr-------------------------------------------------------*/

/*-load_jsonstr-------------------------------------------------------*/
constexpr std::uintmax_t kMaxJsonFileSize = 64ULL * 1024ULL * 1024ULL; // 64 MiB
constexpr std::size_t    kReadChunkSize    = 64 * 1024;

void strip_utf8_bom(std::string& s) noexcept;

        // JSON 标准要求 UTF-8；遇到 UTF-16/UTF-32 BOM 提前拒绝，避免解析器报无意义的错误
bool has_utf16_or_utf32_bom(const std::string& s) noexcept;

// 读取 JSON 文本文件。
// 成功：返回 true，_out_json 为剥离 UTF-8 BOM 后的内容，_err 为空。
// 失败：返回 false，_out_json 清空，_err 为错误原因短句。
bool load_jsonstr(const std::string& _file_path,
                  std::string&       _out_json,
                         std::string&       _err);
/*-load_jsonstr-------------------------------------------------------*/

/*-exec_cmd-------------------------------------------------------*/
struct command_result {
  int exit_code = -1;
  std::string std_out;
  std::string std_err;
  bool timed_out = false;
  bool truncated = false;   // 输出超过 kMaxCaptureBytes 被截断
};

#if defined(__linux__)
command_result exec_cmd(const std::string &cmd);
#elif defined(_WIN32)

std::string ensure_utf8(std::string s);

command_result exec_cmd(const std::string &cmd, DWORD timeout_ms=0);
#endif
/*-exec_cmd-------------------------------------------------------*/

/*-file insert-------------------------------------------------------*/
#if defined(__linux__)
// ---------------- RAII: fd ----------------
class Fd {
 public:
  Fd() noexcept = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}
  ~Fd() { close(); }

  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  Fd &operator=(Fd &&o) noexcept {
    if (this != &o) { close(); fd_ = o.fd_; o.fd_ = -1; }
    return *this;
  }

  int  get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }
  void close() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

 private:
  int fd_ = -1;
};

// ---------------- RAII: 未 commit 则 unlink ----------------
class TempFileGuard {
 public:
  explicit TempFileGuard(std::string p) noexcept : path_(std::move(p)) {}
  ~TempFileGuard() {
    if (!committed_ && !path_.empty()) ::unlink(path_.c_str());
  }
  TempFileGuard(const TempFileGuard &) = delete;
  TempFileGuard &operator=(const TempFileGuard &) = delete;
  void commit() noexcept { committed_ = true; }

 private:
  std::string path_;
  bool        committed_ = false;
};

// ---------------- 错误信息 ----------------
// 线程安全、跨语言：用 std::generic_category() 替代 strerror
std::string errno_str(int err);

std::string err_str(const char *what, std::string_view path, int err);

// ---------------- 完整写 ----------------
bool write_full(int fd, const void *buf, size_t len, int *out_errno);

// ---------------- 范围拷贝 ----------------
bool copy_range(int src_fd, int dst_fd, off_t off, off_t len,
                std::vector<char> &buf, int *out_errno);

// ---------------- fsync 带 EINTR 重试 ----------------
bool fsync_retry(int fd, int *out_errno);

std::string dir_of(std::string_view path);

// 用 memchr 快速统计换行并定位插入点
// 返回是否找到（line==0 时直接返回 true）
bool scan_insert_point(int fd, off_t file_size, size_t line,
                       std::vector<char> &buf,
                       off_t &insert_pos, size_t &total_newlines,
                       std::string &err);

std::string file_insert_line(const char *file_path,
                             size_t line,
                             const char *data,
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

  UniqueHandle(UniqueHandle&& o) noexcept : h_(o.h_) { o.h_ = INVALID_HANDLE_VALUE; }
  UniqueHandle& operator=(UniqueHandle&& o) noexcept {
    if (this != &o) { close(); h_ = o.h_; o.h_ = INVALID_HANDLE_VALUE; }
    return *this;
  }

  HANDLE get() const noexcept { return h_; }
  bool   valid() const noexcept { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
  void   close() noexcept {
    if (valid()) { ::CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
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
  ~TempFileGuard() { if (!dismissed_) ::DeleteFileW(p_); }
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

constexpr int64_t kMaxLineSpan   = 100000;
constexpr std::size_t kMaxLineLen = 1u << 20;   // 1 MiB
constexpr std::size_t kReadBufSize = 64 * 1024;

[[nodiscard]] bool fail(std::string &out, std::string msg);

// 线程安全，可移植。等价于 strerror_r，但避免了 GNU/XSI 版本二义性。
std::string errno_str(int e);

void append_padded_int(std::string &dst, int64_t n, int width);

class ScopedFd {
  int fd_{-1};
 public:
  explicit ScopedFd(int fd) noexcept : fd_(fd) {}
  ~ScopedFd() { if (fd_ >= 0) ::close(fd_); }
  ScopedFd(const ScopedFd &)            = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
  ScopedFd(ScopedFd &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  ScopedFd &operator=(ScopedFd &&o) noexcept {
    if (this != &o) { if (fd_ >= 0) ::close(fd_); fd_ = o.fd_; o.fd_ = -1; }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
};

// 读取 [start, end]（1-based，闭区间）。
// 成功：out = "行号: 内容\n" 拼接（行号按 end 的宽度右对齐补空格）。
// 失败：out = 错误信息。
// 仅用于 Linux。
bool file_read_line(const std::string &file_path,
                    int64_t start, int64_t end,
                    std::string &out);

#elif defined(_WIN32)

constexpr DWORD  kBufferSize      = 64u * 1024;
constexpr size_t kMaxLineBytes    = 1024u * 1024;
constexpr size_t kMaxOutputBytes  = 64ull * 1024 * 1024;

std::string WideToUtf8(const wchar_t* w, int len);

bool PathToWide(const std::string& path, std::wstring& out);

std::string Win32ErrorString(DWORD err);

void AppendLine(std::string& out, int lineNo, const std::string& content);

bool file_read_line(const std::string& file_path, int start, int end,
                           std::string& out);

#endif
/*-file read-------------------------------------------------------*/

/*-file delete-------------------------------------------------------*/
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
  ~TempGuard() { if (!committed) ::DeleteFileW(path.c_str()); }
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

/*-file delete-------------------------------------------------------*/

/*-ripgrep-------------------------------------------------------*/
#if defined(__linux__)

#elif defined(_WIN32)

// ------------------------------------------------------------------
// 返回结构
// ------------------------------------------------------------------
struct GrepResult {
  bool        ok           = false;   // 调用成功（rg 退出码 0 或 1）
  std::string content;                // 供 LLM/调用方直接使用的文本
  size_t      totalMatches = 0;       // 匹配行总数（不含文件标题行）
  bool        truncated    = false;   // 是否被 250 行截断
  std::string fullListPath;           // 被截断时，完整匹配列表保存路径
  int         exitCode     = 0;
  std::string error;                  // 出错时的信息
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
  ~Handle() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }
  Handle& operator=(Handle&& o) noexcept {
    if (this != &o) { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
      h = o.h; o.h = INVALID_HANDLE_VALUE; }
    return *this;
  }
  operator HANDLE() const { return h; }
  HANDLE* addr() { return &h; }
  bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

GrepResult grep(const std::string& pattern,
                const std::string& path    = "",
                const std::string& include = "",
                       size_t             max= 250);

#endif
/*-ripgrep-------------------------------------------------------*/

}; // namespace kw

#endif












