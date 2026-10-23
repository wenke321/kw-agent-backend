#include "function.h"

#include "helpers/log.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <process.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#endif

namespace kw {

namespace fs = std::filesystem;

/*-store_jsonstr-------------------------------------------------------*/

void sleep_backoff(int attempt) {
  std::this_thread::sleep_for(kRetryBackoff * (attempt + 1));
}

std::string make_temp_suffix() {
  static std::atomic<uint64_t> s_seq{0};
  const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
#ifdef _WIN32
  const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  const auto ts  = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto seq = s_seq.fetch_add(1, std::memory_order_relaxed);
  return ".tmp." + std::to_string(pid) + "." + std::to_string(tid) + "."
         + std::to_string(ts) + "." + std::to_string(seq);
}

// 对已 fileno 的 fd 做 fsync，处理 EINTR
bool fsync_fd(int fd) {
#ifdef _WIN32
  const intptr_t osfh = _get_osfhandle(fd);
  if (osfh == -1) return false;
  HANDLE h = reinterpret_cast<HANDLE>(osfh);
  if (h == INVALID_HANDLE_VALUE) return false;
  return ::FlushFileBuffers(h) != 0;
#else
  for (;;) {
    if (::fsync(fd) == 0) return true;
    if (errno != EINTR) return false;
  }
#endif
}

// 关键修复：先 fflush 把 stdio 缓冲推进内核，再 fsync 落盘
bool flush_and_sync(std::FILE* fp) {
  if (std::fflush(fp) != 0) return false;
  const int fd = ::fileno(fp);
  if (fd < 0) return false;
  return fsync_fd(fd);
}

bool sync_directory(const fs::path& dir) {
#ifdef _WIN32
  (void)dir;
  return true;   // Windows 无对应稳定接口
#else
  int flags = O_RDONLY;
#  ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#  endif
  int fd;
  do { fd = ::open(dir.c_str(), flags); } while (fd < 0 && errno == EINTR);
  if (fd < 0) return false;

  int saved = 0;
  bool ok = false;
  for (;;) {
    if (::fsync(fd) == 0) { ok = true; break; }
    if (errno != EINTR) { saved = errno; break; }
  }
  ::close(fd);

          // 部分 FS（tmpfs / 某些网络 FS / 容器 overlay）不支持目录 fsync
  if (!ok && saved == EINVAL) return true;
  if (!ok) errno = saved;
  return ok;
#endif
}

bool atomic_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
  for (int i = 0; i < kRetryCount; ++i) {
    if (::MoveFileExW(from.wstring().c_str(),
                      to.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
    const DWORD err = ::GetLastError();
    if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) {
      return false;
    }
    sleep_backoff(i);
  }
  return false;
#else
  for (int i = 0; i < kRetryCount; ++i) {
    if (::rename(from.c_str(), to.c_str()) == 0) return true;
    if (errno != EINTR && errno != EBUSY) return false;
    sleep_backoff(i);
  }
  return false;
#endif
}

bool is_valid_file_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  if (name.size() > kMaxFileNameLen) return false;
  if (name.find('/') != std::string::npos) return false;
#ifdef _WIN32
  if (name.find('\\') != std::string::npos) return false;
  if (name.find(':')  != std::string::npos) return false;
#endif
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool store_jsonstr(const std::string& _file_name,
                          const std::string& _jsonstr,
                          const std::string& _prefix)
{
  // ---------- 参数校验 ----------
  if (!is_valid_file_name(_file_name)) {
    LOG_ERROR("store_jsonstr: invalid file name: '{}'", _file_name);
    return false;
  }
  if (_prefix.empty()) {
    LOG_ERROR("store_jsonstr: empty prefix, file_name={}", _file_name);
    return false;
  }

          // ---------- 准备目录 ----------
  std::error_code ec;
  const fs::path dir = fs::path(_prefix);
  fs::create_directories(dir, ec);
  if (ec) {
    LOG_ERROR("store_jsonstr: create_directories failed, prefix={}, err={}",
              _prefix, ec.message());
    return false;
  }

  const fs::path target = dir / (_file_name + ".json");
  const fs::path tmp    = dir / (_file_name + ".json" + make_temp_suffix());

          // ---------- 写临时文件 ----------
  std::FILE* fp = nullptr;
#ifdef _WIN32
  fp = ::_wfopen(tmp.wstring().c_str(), L"wb");
#else
  fp = std::fopen(tmp.c_str(), "wb");
#endif
  if (!fp) {
    const int e = errno;
    LOG_ERROR("store_jsonstr: fopen temp failed, path={}, errno={} ({})",
              tmp.string(), e, std::strerror(e));
    return false;
  }

  bool ok = true;

  if (!_jsonstr.empty()) {
    std::size_t written = 0;
    while (written < _jsonstr.size()) {
      const std::size_t n = std::fwrite(_jsonstr.data() + written, 1,
                                        _jsonstr.size() - written, fp);
      if (n == 0) {
        const int e = errno;
        LOG_ERROR("store_jsonstr: fwrite failed, path={}, wrote={}/{}, errno={}",
                  tmp.string(), written, _jsonstr.size(), e);
        ok = false;
        break;
      }
      written += n;
    }
  }

  if (ok && !flush_and_sync(fp)) {
    const int e = errno;
    LOG_ERROR("store_jsonstr: fflush/fsync failed, path={}, errno={} ({})",
              tmp.string(), e, std::strerror(e));
    ok = false;
  }

  if (std::fclose(fp) != 0) {
    const int e = errno;
    LOG_ERROR("store_jsonstr: fclose failed, path={}, errno={} ({})",
              tmp.string(), e, std::strerror(e));
    ok = false;
  }

  if (!ok) {
    std::error_code rmec;
    fs::remove(tmp, rmec);
    return false;
  }

          // ---------- 原子替换 ----------
  if (!atomic_replace(tmp, target)) {
#ifdef _WIN32
    const long long e = static_cast<long long>(::GetLastError());
#else
    const long long e = errno;
#endif
    LOG_ERROR("store_jsonstr: atomic replace failed, tmp={}, target={}, err={}",
              tmp.string(), target.string(), e);
    std::error_code rmec;
    fs::remove(tmp, rmec);
    return false;
  }

          // ---------- 持久化目录项 ----------
  if (!sync_directory(dir)) {
    const int e = errno;
    LOG_ERROR("store_jsonstr: directory fsync failed (rename may not be durable), "
        "dir={}, errno={}", dir.string(), e);
    // 数据本身已落盘；此处仅目录项持久性打折，不算失败
  }

  return true;
}
/*-store_jsonstr-------------------------------------------------------*/

/*-load_jsonstr-------------------------------------------------------*/

void strip_utf8_bom(std::string& s) noexcept
{
  if (s.size() >= 3
      && static_cast<unsigned char>(s[0]) == 0xEF
      && static_cast<unsigned char>(s[1]) == 0xBB
      && static_cast<unsigned char>(s[2]) == 0xBF)
  {
    s.erase(0, 3);
  }
}

        // JSON 标准要求 UTF-8；遇到 UTF-16/UTF-32 BOM 提前拒绝，避免解析器报无意义的错误
bool has_utf16_or_utf32_bom(const std::string& s) noexcept
{
  const auto b0 = s.size() > 0 ? static_cast<unsigned char>(s[0]) : 0;
  const auto b1 = s.size() > 1 ? static_cast<unsigned char>(s[1]) : 0;
  const auto b2 = s.size() > 2 ? static_cast<unsigned char>(s[2]) : 0;
  const auto b3 = s.size() > 3 ? static_cast<unsigned char>(s[3]) : 0;

          // UTF-16 BE / LE
  if ((b0 == 0xFE && b1 == 0xFF) || (b0 == 0xFF && b1 == 0xFE))
    return true;

          // UTF-32 BE / LE
  if ((b0 == 0x00 && b1 == 0x00 && b2 == 0xFE && b3 == 0xFF) ||
      (b0 == 0xFF && b1 == 0xFE && b2 == 0x00 && b3 == 0x00))
    return true;

  return false;
}

// 读取 JSON 文本文件。
// 成功：返回 true，_out_json 为剥离 UTF-8 BOM 后的内容，_err 为空。
// 失败：返回 false，_out_json 清空，_err 为错误原因短句。
bool load_jsonstr(const std::string& _file_path,
                         std::string&       _out_json,
                         std::string&       _err)
{
  _err.clear();
  _out_json.clear();

  try
  {
    if (_file_path.empty())
    {
      _err = "empty file path";
      LOG_ERROR("load_jsonstr: {}", _err);
      return false;
    }

    std::error_code ec;
    const fs::path path(_file_path);

            // ---- 1. 文件状态检查 ----
    const fs::file_status st = fs::status(path, ec);
    if (ec)
    {
      _err = "stat failed: " + ec.message();
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }
    if (!fs::exists(st))
    {
      _err = "file not found";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }
    if (!fs::is_regular_file(st))
    {
      _err = "not a regular file";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

            // ---- 2. 初始大小检查（仅作快速失败，真正上限在读取循环里再兜底） ----
    const std::uintmax_t file_size = fs::file_size(path, ec);
    if (ec)
    {
      _err = "get size failed: " + ec.message();
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }
    if (file_size == 0)
    {
      _err = "empty file";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }
    if (file_size > kMaxJsonFileSize)
    {
      _err = "file too large";
      LOG_ERROR("load_jsonstr: {} ('{}', {} bytes, limit {} bytes)",
                _err, _file_path, file_size, kMaxJsonFileSize);
      return false;
    }

            // ---- 3. 二进制打开，避免 Windows CRLF 转换 ----
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs)
    {
      _err = "open failed";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

            // ---- 4. 分块读取；即使文件在 stat 之后变大，也不会突破大小上限 ----
    std::string content;
    content.reserve(static_cast<std::size_t>(file_size));

    std::array<char, kReadChunkSize> buf{};
    for (;;)
    {
      ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const std::streamsize got = ifs.gcount();

      if (got > 0)
      {
        const auto uGot = static_cast<std::uintmax_t>(got);
        if (static_cast<std::uintmax_t>(content.size()) + uGot > kMaxJsonFileSize)
        {
          _err = "file too large while reading";
          LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
          return false;
        }
        content.append(buf.data(), static_cast<std::size_t>(got));
      }

      if (ifs.bad())
      {
        _err = "io error while reading";
        LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
        return false;
      }
      if (ifs.eof())
        break;
      if (ifs.fail())
      {
        _err = "read failed";
        LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
        return false;
      }
    }

    if (content.empty())
    {
      _err = "empty content";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

            // ---- 5. 拒绝 UTF-16/UTF-32，剥离 UTF-8 BOM ----
    if (has_utf16_or_utf32_bom(content))
    {
      _err = "unsupported encoding (utf-16/utf-32 bom)";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

    strip_utf8_bom(content);

    if (content.empty())
    {
      _err = "empty content after bom strip";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

    _out_json = std::move(content);
    return true;
  }
  catch (const std::exception& e)
  {
    _err = std::string("exception: ") + e.what();
    LOG_ERROR("load_jsonstr: exception while loading '{}': {}", _file_path, e.what());
    _out_json.clear();
    return false;
  }
  catch (...)
  {
    _err = "unknown exception";
    LOG_ERROR("load_jsonstr: unknown exception while loading '{}'", _file_path);
    _out_json.clear();
    return false;
  }
}
/*-load_jsonstr-------------------------------------------------------*/

/*-exec_cmd-------------------------------------------------------*/

#if defined(__linux__)
command_result exec_cmd(const std::string &cmd) {
  int pipe_out[2], pipe_err[2];
  if (::pipe(pipe_out) == -1 || ::pipe(pipe_err) == -1) {
    //
  }

  pid_t child = ::fork();

  if (child == -1) {
    ::close(pipe_out[0]);
    ::close(pipe_out[1]);
    ::close(pipe_err[0]);
    ::close(pipe_err[1]);
    //
  }

  if (child == 0) {
    ::close(pipe_out[0]);
    ::close(pipe_err[0]);
    ::dup2(pipe_out[1], STDOUT_FILENO);
    ::close(pipe_out[1]);
    ::dup2(pipe_err[1], STDERR_FILENO);
    ::close(pipe_err[1]);

    ::execl("/bin/sh", "sh", "-c", cmd.c_str(), (char *)nullptr);
    ::_exit(127);
  }

  ::close(pipe_out[1]);
  ::close(pipe_err[1]);

  std::string out, err;
  char buf[4096];
  bool out_eof = false, err_eof = false;
  fd_set fds;
  int maxfd = std::max(pipe_out[0], pipe_err[0]) + 1;
  while (!out_eof || !err_eof) {
    FD_ZERO(&fds);
    if (!out_eof)
      FD_SET(pipe_out[0], &fds);
    if (!err_eof)
      FD_SET(pipe_err[0], &fds);

    int ret = ::select(maxfd, &fds, nullptr, nullptr, nullptr);
    if (ret == -1) {
      if (errno == EINTR)
        continue;
      //
    }

    if (FD_ISSET(pipe_out[0], &fds)) {
      ssize_t n = ::read(pipe_out[0], buf, sizeof(buf));
      if (n > 0) {
        out.append(buf, n);
      } else if (n == 0) {
        out_eof = true;
      } else {
        if (errno != EINTR && errno != EAGAIN) {
          //
        }
      }
    }

    if (FD_ISSET(pipe_err[0], &fds)) {
      ssize_t n = ::read(pipe_err[0], buf, sizeof(buf));
      if (n > 0) {
        err.append(buf, n);
      } else if (n == 0) {
        err_eof = true;
      } else {
        if (errno != EINTR && errno != EAGAIN) {
          //
        }
      }
    }
  }

  int status;
  if (::waitpid(child, &status, 0) == -1) {
    //
  }

  if (WIFEXITED(status))
    return {WEXITSTATUS(status), std::move(out), std::move(err)};
  return {-1, std::move(out), std::move(err)};
}
#elif defined(_WIN32)

constexpr size_t kMaxCaptureBytes   = 32u * 1024u * 1024u;
constexpr size_t kMaxCmdLineChars   = 8191;   // cmd.exe /c 参数上限
constexpr DWORD  kReaderJoinGraceMs = 5000;   // Job 生效时的宽限期（EOF 即返回）
constexpr DWORD  kReaderJoinGraceNoJobMs = 500; // Job 未生效时缩短宽限期
constexpr int    kCancelRetries     = 200;

// ---------- 编码 ----------

// 失败时返回空字符串（签名不允许 optional，调用方需自行区分“空输入”与“转换失败”）。
inline std::wstring utf8_to_wide_strict(const std::string& s) {
  if (s.empty()) return {};
  if (s.size() > static_cast<size_t>(INT_MAX)) return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  int r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              s.data(), static_cast<int>(s.size()), &w[0], n);
  if (r != n) return {};
  return w;
}

// 失败时返回空字符串（不再返回原串，避免产生非 UTF-8 结果）。
inline std::string cp_to_utf8(const std::string& s, UINT cp) {
  if (s.empty()) return {};
  if (s.size() > static_cast<size_t>(INT_MAX)) return {};
  // MB_ERR_INVALID_CHARS 仅对 CP_UTF8 有效；其他代码页传 0。
  DWORD mbFlags = (cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
  int wlen = MultiByteToWideChar(cp, mbFlags, s.data(),
                                 static_cast<int>(s.size()), nullptr, 0);
  if (wlen <= 0) return {};
  std::wstring w(static_cast<size_t>(wlen), L'\0');
  int rw = MultiByteToWideChar(cp, mbFlags, s.data(),
                               static_cast<int>(s.size()), &w[0], wlen);
  if (rw != wlen) return {};
  // WC_ERR_INVALID_CHARS 仅对 CP_UTF8 有效（Vista+）。
  int u8len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                  w.data(), wlen, nullptr, 0, nullptr, nullptr);
  if (u8len <= 0) return {};
  std::string u8(static_cast<size_t>(u8len), '\0');
  int ru = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                               w.data(), wlen, &u8[0], u8len, nullptr, nullptr);
  if (ru != u8len) return {};
  return u8;
}

// 失败时返回空字符串（签名限制；调用方若需区分，建议改用 optional 版本）。
inline std::string ensure_utf8(std::string s) {
  if (s.empty()) return s;
  if (s.size() > static_cast<size_t>(INT_MAX)) return {};

          // 0) UTF-32 BOM：本函数不支持 UTF-32，显式拒绝，避免被误判为 UTF-16。
  if (s.size() >= 4) {
    unsigned char b0 = static_cast<unsigned char>(s[0]);
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    unsigned char b2 = static_cast<unsigned char>(s[2]);
    unsigned char b3 = static_cast<unsigned char>(s[3]);
    if ((b0 == 0xFF && b1 == 0xFE && b2 == 0x00 && b3 == 0x00) ||  // UTF-32 LE
        (b0 == 0x00 && b1 == 0x00 && b2 == 0xFE && b3 == 0xFF)) {  // UTF-32 BE
      return {};
    }
  }

          // 1) UTF-16 BOM（至少 2 字节；载荷必须为偶数长度）
  if (s.size() >= 2) {
    unsigned char b0 = static_cast<unsigned char>(s[0]);
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    bool le = (b0 == 0xFF && b1 == 0xFE);
    bool be = (b0 == 0xFE && b1 == 0xFF);
    if (le || be) {
      if ((s.size() - 2) % 2 != 0) return {};  // 截断的 UTF-16
      int wlen = static_cast<int>((s.size() - 2) / 2);
      if (wlen == 0) return {};
      std::wstring w(static_cast<size_t>(wlen), L'\0');
      // 逐字节组装，避免 reinterpret_cast<wchar_t*> 的对齐/严格别名问题。
      for (int i = 0; i < wlen; ++i) {
        unsigned char a = static_cast<unsigned char>(s[2 + 2 * i]);
        unsigned char b = static_cast<unsigned char>(s[3 + 2 * i]);
        w[static_cast<size_t>(i)] =
            le ? static_cast<wchar_t>(a | (static_cast<unsigned>(b) << 8))
               : static_cast<wchar_t>((static_cast<unsigned>(a) << 8) | b);
      }
      int u8len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      w.data(), wlen, nullptr, 0, nullptr, nullptr);
      if (u8len <= 0) return {};
      std::string u8(static_cast<size_t>(u8len), '\0');
      int ru = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                   w.data(), wlen, &u8[0], u8len, nullptr, nullptr);
      if (ru != u8len) return {};
      return u8;
    }
  }

          // 2) 严格校验 UTF-8；合法则直接返回（UTF-8 BOM 也合法，保留）
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                          s.data(), static_cast<int>(s.size()),
                          nullptr, 0) > 0) {
    return s;
  }

          // 3) 回退 OEM。注意 GetOEMCP() 不一定等于控制台输出代码页；
          //    若来源是控制台，调用方可能更适合用 GetConsoleOutputCP()。
  return cp_to_utf8(s, GetOEMCP());
}

// ---------- 环境变量（TOCTOU 重试） ----------

std::wstring get_env_dynamic(const wchar_t* name) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    SetLastError(0);
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) {
      // ERROR_ENVVAR_NOT_FOUND(不存在) 或 ERROR_SUCCESS(存在但为空)
      return {};
    }
    std::wstring v(n, L'\0');
    SetLastError(0);
    DWORD r = GetEnvironmentVariableW(name, v.data(), n);
    if (r == 0) return {};
    if (r < n) { v.resize(r); return v; }
    // r >= n：探测后值变长，重试
  }
  return {};
}

// 从 System32 解析 cmd.exe 绝对路径，避免相对路径劫持
std::wstring system_cmd_path() {
  wchar_t sysdir[MAX_PATH];
  UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    std::wstring p(sysdir, n);
    if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
    p += L"cmd.exe";
    return p;
  }
  return L"cmd.exe";   // 极低概率，保留兜底
}

// ---------- 取消令牌 ----------

struct cancel_token {
  std::atomic<HANDLE> thread_handle{ nullptr };
  std::atomic<bool>   stopped{ false };
};

// 关键修复：重试直到线程真正退出，消除“Cancel 早于 ReadFile”的竞态。
void cancel_read(cancel_token& tok) noexcept {
  tok.stopped.store(true, std::memory_order_release);
  HANDLE h = tok.thread_handle.load(std::memory_order_acquire);
  if (!h) return;
  for (int i = 0; i < kCancelRetries; ++i) {
    CancelSynchronousIo(h);
    if (WaitForSingleObject(h, 10) == WAIT_OBJECT_0) return;
  }
}

// noexcept：bad_alloc 不得逃逸
void read_all(HANDLE h, std::string& out, bool& truncated, cancel_token& tok) noexcept {
  char buf[8192];
  DWORD n = 0;
  bool capped = false;
  while (!tok.stopped.load(std::memory_order_relaxed)) {
    if (!ReadFile(h, buf, static_cast<DWORD>(sizeof(buf)), &n, nullptr)) break;
    if (n == 0) break;
    if (capped) continue;                       // 已封顶：只排空不存储
    size_t room = kMaxCaptureBytes - out.size();
    try {
      if (n > room) { out.append(buf, room); capped = true; }
      else          { out.append(buf, n); }
    } catch (...) {
      capped = true;                          // 分配失败：排空剩余输出
    }
  }
  truncated = capped;
}

// ---------- RAII ----------

struct handle_guard {
  HANDLE h = nullptr;
  handle_guard() = default;
  explicit handle_guard(HANDLE x) : h(x) {}
  ~handle_guard() { reset(); }
  handle_guard(const handle_guard&) = delete;
  handle_guard& operator=(const handle_guard&) = delete;
  void reset(HANDLE x = nullptr) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = x;
  }
  HANDLE release() { HANDLE x = h; h = nullptr; return x; }
};

struct attr_list_guard {
  LPPROC_THREAD_ATTRIBUTE_LIST a = nullptr;
  ~attr_list_guard() { if (a) DeleteProcThreadAttributeList(a); }
};

// 带宽限期的 join。正常 EOF 时立即返回；否则主动取消，保证 join 一定返回。
void join_reader(std::thread& t, cancel_token& tok, DWORD grace_ms) {
  if (!t.joinable()) return;
  if (WaitForSingleObject(t.native_handle(), grace_ms) == WAIT_TIMEOUT) {
    cancel_read(tok);
  }
  t.join();
}

// timeout_ms == 0 表示不超时（无限等待）
command_result exec_cmd(const std::string &cmd, DWORD timeout_ms) {// 注：timeout_ms == 0 表示无限等待；INFINITE 亦等价无限等待。
  command_result result;

  if (cmd.empty()) { result.std_err = "exec_cmd: empty command"; return result; }
  if (cmd.find('\0') != std::string::npos) {
    result.std_err = "exec_cmd: command contains NUL byte";
    return result;
  }

  std::wstring wcmd = utf8_to_wide_strict(cmd);
  if (wcmd.empty()) {
    result.std_err = "exec_cmd: command is not valid UTF-8";
    return result;
  }

  std::wstring exe = get_env_dynamic(L"COMSPEC");
  if (exe.empty()) exe = system_cmd_path();   // 绝对路径兜底

          // 给 argv[0] 加引号，防止 exe 路径含空格时 argv[0] 被截断
  std::wstring cmdline = L"\"" + exe + L"\" /s /c \"" + wcmd + L"\"";
  if (cmdline.size() > kMaxCmdLineChars) {
    result.std_err = "exec_cmd: command line too long";
    return result;
  }
  std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
  cmdline_buf.push_back(L'\0');

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  handle_guard stdout_r, stdout_w, stderr_r, stderr_w, nul_h, job;

  if (!CreatePipe(&stdout_r.h, &stdout_w.h, &sa, 64 * 1024)) {
    result.std_err = "exec_cmd: CreatePipe(stdout) failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }
  if (!CreatePipe(&stderr_r.h, &stderr_w.h, &sa, 64 * 1024)) {
    result.std_err = "exec_cmd: CreatePipe(stderr) failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }
  if (!SetHandleInformation(stdout_r.h, HANDLE_FLAG_INHERIT, 0)) {
    result.std_err = "exec_cmd: SetHandleInformation(stdout) failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }
  if (!SetHandleInformation(stderr_r.h, HANDLE_FLAG_INHERIT, 0)) {
    result.std_err = "exec_cmd: SetHandleInformation(stderr) failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }

  nul_h.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &sa, OPEN_EXISTING, 0, nullptr);
  if (nul_h.h == INVALID_HANDLE_VALUE) {
    nul_h.h = nullptr;
    result.std_err = "exec_cmd: open NUL failed, err=" + std::to_string(GetLastError());
    return result;
  }

  job.h = CreateJobObjectW(nullptr, nullptr);
  if (job.h) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
  }

  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  if (attr_size == 0) {
    result.std_err = "exec_cmd: attr list sizing failed";
    return result;
  }
  std::vector<BYTE> attr_buf(attr_size);
  attr_list_guard attr_guard;
  attr_guard.a = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  if (!InitializeProcThreadAttributeList(attr_guard.a, 1, 0, &attr_size)) {
    attr_guard.a = nullptr;
    result.std_err = "exec_cmd: InitializeProcThreadAttributeList failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }

  HANDLE inherit_handles[] = { stdout_w.h, stderr_w.h, nul_h.h };
  if (!UpdateProcThreadAttribute(attr_guard.a, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 inherit_handles, sizeof(inherit_handles),
                                 nullptr, nullptr)) {
    result.std_err = "exec_cmd: UpdateProcThreadAttribute failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb         = sizeof(si);
  si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = stdout_w.h;
  si.StartupInfo.hStdError  = stderr_w.h;
  si.StartupInfo.hStdInput  = nul_h.h;
  si.lpAttributeList        = attr_guard.a;

  DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                CREATE_BREAKAWAY_FROM_JOB;

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(exe.c_str(), cmdline_buf.data(),
                           nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                           &si.StartupInfo, &pi);
  if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
    flags &= ~CREATE_BREAKAWAY_FROM_JOB;
    ok = CreateProcessW(exe.c_str(), cmdline_buf.data(),
                        nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                        &si.StartupInfo, &pi);
  }
  if (!ok) {
    result.std_err = "exec_cmd: CreateProcess failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }

  handle_guard process_guard(pi.hProcess);
  handle_guard thread_guard(pi.hThread);

  stdout_w.reset();
  stderr_w.reset();
  nul_h.reset();

  bool job_assigned = false;
  if (job.h) {
    if (AssignProcessToJobObject(job.h, pi.hProcess)) {
      job_assigned = true;
    } else {
      DWORD e = GetLastError();
      if (e != ERROR_ACCESS_DENIED) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        result.std_err = "exec_cmd: AssignProcessToJobObject failed, err=" +
                         std::to_string(e);
        return result;
      }
      // ERROR_ACCESS_DENIED：父进程 Job 不允许嵌套，降级继续
    }
  }

  if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
    if (job_assigned) TerminateJobObject(job.h, 1);
    else              TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    result.std_err = "exec_cmd: ResumeThread failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }

  std::string out_buf, err_buf;
  bool trunc_out = false, trunc_err = false;
  cancel_token tok_out, tok_err;
  HANDLE h_out = stdout_r.h;
  HANDLE h_err = stderr_r.h;
  std::thread t_out, t_err;

  try {
    t_out = std::thread([h_out, &out_buf, &trunc_out, &tok_out] {
      read_all(h_out, out_buf, trunc_out, tok_out);
    });
    tok_out.thread_handle.store(t_out.native_handle(), std::memory_order_release);

    t_err = std::thread([h_err, &err_buf, &trunc_err, &tok_err] {
      read_all(h_err, err_buf, trunc_err, tok_err);
    });
    tok_err.thread_handle.store(t_err.native_handle(), std::memory_order_release);
  } catch (const std::system_error&) {
    // 统一走 join_reader，保证即便线程卡在 append 也能被取消兜底
    cancel_read(tok_out);
    cancel_read(tok_err);
    if (job_assigned) TerminateJobObject(job.h, 1);
    else              TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    job.reset();
    join_reader(t_out, tok_out, kReaderJoinGraceNoJobMs);
    join_reader(t_err, tok_err, kReaderJoinGraceNoJobMs);
    result.std_err = "exec_cmd: failed to start capture threads";
    return result;
  }

  DWORD wait_rc = WaitForSingleObject(pi.hProcess,
                                      timeout_ms == 0 ? INFINITE : timeout_ms);
  std::string wait_error;
  if (wait_rc == WAIT_TIMEOUT) {
    result.timed_out = true;
    if (job_assigned) TerminateJobObject(job.h, 1);
    else              TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  } else if (wait_rc == WAIT_FAILED) {
    wait_error = "exec_cmd: WaitForSingleObject failed, err=" +
                 std::to_string(GetLastError());
    if (job_assigned) TerminateJobObject(job.h, 1);
    else              TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }

  DWORD ec = 0;
  if (GetExitCodeProcess(pi.hProcess, &ec))
    result.exit_code = static_cast<int>(ec);

          // 关闭 Job：KILL_ON_JOB_CLOSE 确保后台孙进程一并终止、管道写端真正关闭
  job.reset();

          // Job 生效时 EOF 即刻到达，join_reader 立即返回；
          // Job 未生效时用较短宽限期，避免对带后台子进程的命令产生固定 5s 延迟。
  DWORD grace = job_assigned ? kReaderJoinGraceMs : kReaderJoinGraceNoJobMs;
  join_reader(t_out, tok_out, grace);
  join_reader(t_err, tok_err, grace);

  result.truncated  = trunc_out || trunc_err;
  result.std_out    = ensure_utf8(std::move(out_buf));
  result.std_err    = ensure_utf8(std::move(err_buf));
  // result.std_out=std::move(out_buf);
  // result.std_err=std::move(err_buf);
  if (!wait_error.empty()) {
    if (!result.std_err.empty()) result.std_err += "\n";
    result.std_err += wait_error;
  }
  return result;
}
#endif
/*-exec_cmd-------------------------------------------------------*/

/*-file insert-------------------------------------------------------*/
#if defined(__linux__)

// ---------------- 错误信息 ----------------
// 线程安全、跨语言：用 std::generic_category() 替代 strerror
std::string errno_str(int err) {
  return std::generic_category().message(err);
}

std::string err_str(const char *what, std::string_view path, int err) {
  std::string s;
  s.reserve(192);
  s += what;
  s += " '";
  s.append(path.data(), path.size());
  s += "' failed: ";
  s += errno_str(err);
  s += " (errno=";
  s += std::to_string(err);
  s += ")";
  return s;
}

// ---------------- 完整写 ----------------
bool write_full(int fd, const void *buf, size_t len, int *out_errno) {
  const char *p = static_cast<const char *>(buf);
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::write(fd, p, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (out_errno) *out_errno = errno;
      return false;
    }
    if (n == 0) {                       // 普通文件不该发生
      if (out_errno) *out_errno = EIO;
      return false;
    }
    p         += n;
    remaining -= static_cast<size_t>(n);
  }
  return true;
}

// ---------------- 范围拷贝 ----------------
bool copy_range(int src_fd, int dst_fd, off_t off, off_t len,
                std::vector<char> &buf, int *out_errno) {
  if (len <= 0) return true;
  off_t cur = off;
  const off_t end = off + len;
  while (cur < end) {
    const size_t want = std::min<size_t>(buf.size(),
                                         static_cast<size_t>(end - cur));
    ssize_t n = ::pread(src_fd, buf.data(), want, cur);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (out_errno) *out_errno = errno;
      return false;
    }
    if (n == 0) {                       // 源被截断
      if (out_errno) *out_errno = EIO;
      return false;
    }
    if (!write_full(dst_fd, buf.data(), static_cast<size_t>(n), out_errno))
      return false;
    cur += n;
  }
  return true;
}

// ---------------- fsync 带 EINTR 重试 ----------------
bool fsync_retry(int fd, int *out_errno) {
  for (;;) {
    if (::fsync(fd) == 0) return true;
    if (errno == EINTR) continue;
    if (out_errno) *out_errno = errno;
    return false;
  }
}

std::string dir_of(std::string_view path) {
  auto slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return ".";
  if (slash == 0)                      return "/";
  return std::string(path.substr(0, slash));
}

// 用 memchr 快速统计换行并定位插入点
// 返回是否找到（line==0 时直接返回 true）
bool scan_insert_point(int fd, off_t file_size, size_t line,
                       std::vector<char> &buf,
                       off_t &insert_pos, size_t &total_newlines,
                       std::string &err) {
  insert_pos     = 0;
  total_newlines = 0;
  if (line == 0) return true;

  bool  found = false;
  off_t off   = 0;
  while (off < file_size) {
    const size_t want = std::min<size_t>(buf.size(),
                                         static_cast<size_t>(file_size - off));
    ssize_t n = ::pread(fd, buf.data(), want, off);
    if (n < 0) {
      if (errno == EINTR) continue;
      err = std::string("pread failed: ") + errno_str(errno);
      return false;
    }
    if (n == 0) break;

    const char *p   = buf.data();
    const char *end = p + n;
    while (p < end) {
      const void *hit = ::memchr(p, '\n', static_cast<size_t>(end - p));
      if (!hit) break;
      const char *nl = static_cast<const char *>(hit);
      ++total_newlines;
      if (!found && total_newlines == line) {
        insert_pos = off + (nl - buf.data()) + 1;
        found      = true;
      }
      p = nl + 1;
    }
    off += n;
  }
  return found;
}

std::string file_insert_line(const char *file_path,
                             size_t line,
                             const char *data,
                             size_t len) {
  // ---------- 入参校验 ----------
  if (file_path == nullptr || *file_path == '\0') {
    LOG_ERROR("file_insert_line: file_path is null or empty");
    return "file_path is null or empty";
  }
  if (data == nullptr && len != 0) {
    LOG_ERROR("file_insert_line: data is null but len={}", len);
    return "data is null";
  }

          // ---------- 解析真实路径 ----------
  char resolved[PATH_MAX];
  if (::realpath(file_path, resolved) == nullptr) {
    std::string msg = err_str("realpath", file_path, errno);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }
  const std::string path(resolved);

          // ---------- 打开源 ----------
  Fd src(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!src) {
    std::string msg = err_str("open", path, errno);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }

  struct stat st{};
  if (::fstat(src.get(), &st) != 0) {
    std::string msg = err_str("fstat", path, errno);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }
  if (!S_ISREG(st.st_mode)) {
    std::string msg = "'" + path + "' is not a regular file";
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }
  const off_t file_size = st.st_size;

          // ---------- 扫描定位 ----------
  constexpr size_t kCopyBuf = 64 * 1024;
  std::vector<char> buf(kCopyBuf);

  off_t  insert_pos     = 0;
  size_t total_newlines = 0;
  bool   need_leading_newline = false;
  {
    std::string scan_err;
    bool found = scan_insert_point(src.get(), file_size, line, buf,
                                   insert_pos, total_newlines, scan_err);
    if (!scan_err.empty()) {
      std::string msg = scan_err + " in '" + path + "'";
      LOG_ERROR("file_insert_line: {}", msg);
      return msg;
    }
    // 空文件 / line==0：found=true, insert_pos=0
    // 其它情况 found=false 表示 file 中换行数 < line
    if (!found && line != 0) {
      // 判定总行数：newlines + (末字节非 \n ? 1 : 0)
      size_t total_lines = total_newlines;
      if (file_size > 0) {
        char    last = 0;
        ssize_t n    = ::pread(src.get(), &last, 1, file_size - 1);
        if (n != 1) {
          std::string msg = err_str("pread(last byte)", path, errno);
          LOG_ERROR("file_insert_line: {}", msg);
          return msg;
        }
        if (last != '\n') {
          ++total_lines;
          // 目标行是最后一行且无换行结尾：插入前补一个 '\n'
          need_leading_newline = true;
        }
      }
      if (line > total_lines) {
        std::string msg = "line " + std::to_string(line) +
                          " exceeds total lines " +
                          std::to_string(total_lines) +
                          " in '" + path + "'";
        LOG_ERROR("file_insert_line: {}", msg);
        return msg;
      }
      // 目标行存在但无 \n 结尾：追加到文件末尾
      insert_pos = file_size;
    }
  }

          // ---------- 同目录临时文件 ----------
  const std::string dir = dir_of(path);
  std::string tmpl = dir + "/.file_insert_line_XXXXXX";
  const int raw_tmp = ::mkstemp(tmpl.data());   // C++17 起 data() 可写
  if (raw_tmp < 0) {
    std::string msg = err_str("mkstemp", dir, errno);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }
  Fd              tmp(raw_tmp);
  TempFileGuard   tmp_guard(tmpl);

  if (::fchmod(tmp.get(), st.st_mode & 07777) != 0) {
    LOG_WARN("file_insert_line: fchmod('{}') failed: {}", tmpl,
             errno_str(errno));
  }

  int werr = 0;

          // 前缀
  if (!copy_range(src.get(), tmp.get(), 0, insert_pos, buf, &werr)) {
    std::string msg = "copy prefix failed: " + errno_str(werr);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }

          // 插入
  if (need_leading_newline) {
    if (!write_full(tmp.get(), "\n", 1, &werr)) {
      std::string msg = "write leading newline failed: " + errno_str(werr);
      LOG_ERROR("file_insert_line: {}", msg);
      return msg;
    }
  }
  if (len > 0 && !write_full(tmp.get(), data, len, &werr)) {
    std::string msg = "write data failed: " + errno_str(werr);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }

          // 后缀
  if (!copy_range(src.get(), tmp.get(), insert_pos,
                  file_size - insert_pos, buf, &werr)) {
    std::string msg = "copy suffix failed: " + errno_str(werr);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }

          // ---------- 并发检测（同 inode 原地修改）----------
  struct stat st_now{};
  if (::fstat(src.get(), &st_now) != 0) {
    std::string msg = err_str("fstat(recheck)", path, errno);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }
  if (st_now.st_size != st.st_size ||
      st_now.st_mtim.tv_sec  != st.st_mtim.tv_sec ||
      st_now.st_mtim.tv_nsec != st.st_mtim.tv_nsec) {
    std::string msg = "'" + path + "' was modified concurrently; aborting";
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }

          // ---------- 并发检测（路径被替换）----------
          // 关键补充：fstat(src_fd) 永远返回同一 inode，必须对 path 做 stat
  struct stat st_path{};
  if (::stat(path.c_str(), &st_path) != 0) {
    std::string msg = err_str("stat(recheck)", path, errno);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }
  if (st_path.st_dev != st.st_dev || st_path.st_ino != st.st_ino) {
    std::string msg = "'" + path + "' was replaced concurrently; aborting";
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;
  }

          // ---------- 落盘 ----------
  {
    int ferr = 0;
    if (!fsync_retry(tmp.get(), &ferr)) {
      std::string msg = err_str("fsync", tmpl, ferr);
      LOG_ERROR("file_insert_line: {}", msg);
      return msg;
    }
  }

  src.close();
  tmp.close();

          // ---------- 原子替换 ----------
  if (::rename(tmpl.c_str(), path.c_str()) != 0) {
    std::string msg = err_str("rename", tmpl, errno);
    LOG_ERROR("file_insert_line: {}", msg);
    return msg;             // tmp_guard 会 unlink 临时文件
  }
  tmp_guard.commit();

          // ---------- 目录 fsync ----------
  if (Fd dir_fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))) {
    int derr = 0;
    if (!fsync_retry(dir_fd.get(), &derr)) {
      LOG_WARN("file_insert_line: fsync dir '{}' failed: {}",
               dir, errno_str(derr));
    }
  }

  return "success";
}
#elif defined(_WIN32)

// ---------- UTF-8 -> UTF-16（严格模式）----------
bool utf8_to_wide(const char* s, std::wstring& out) {
  out.clear();
  if (!s) return true;
  const int wlen = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
  if (wlen <= 0) return false;
  out.resize(static_cast<size_t>(wlen - 1));
  if (wlen > 1) {
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out.data(), wlen) == 0) {
      out.clear();
      return false;
    }
  }
  return true;
}

// ---------- 完整写入 ----------
bool write_all(HANDLE h, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - off, 1u << 20));
    DWORD written = 0;
    if (!::WriteFile(h, data + off, chunk, &written, nullptr)) return false;
    if (written == 0) { ::SetLastError(ERROR_WRITE_FAULT); return false; }
    off += written;
  }
  return true;
}

// ---------- 从 in 精确拷贝 count 字节到 out ----------
bool copy_exact(HANDLE in, HANDLE out, uint64_t count) {
  if (count == 0) return true;
  std::vector<char> buf(kCopyBufSize);
  uint64_t remaining = count;
  while (remaining > 0) {
    const DWORD to_read = static_cast<DWORD>(std::min<uint64_t>(remaining, kCopyBufSize));
    DWORD read_bytes = 0;
    if (!::ReadFile(in, buf.data(), to_read, &read_bytes, nullptr)) return false;
    if (read_bytes == 0) { ::SetLastError(ERROR_HANDLE_EOF); return false; }
    if (!write_all(out, buf.data(), read_bytes)) return false;
    remaining -= read_bytes;
  }
  return true;
}

// ---------- 目录 ----------
std::wstring dir_of(const std::wstring& wpath) {
  if (wpath.empty()) return L".";
  const auto slash = wpath.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L".";
  // 盘符根： "C:\"
  if (slash == 2 && wpath.size() >= 3 && wpath[1] == L':') {
    return wpath.substr(0, 3);
  }
  // UNC： "\\server\share\..." -> "\\server\share\..."
  if (wpath.size() >= 2 && wpath[0] == L'\\' && wpath[1] == L'\\') {
    if (slash <= 1) return L".";  // "\\" 本身
    return wpath.substr(0, slash);
  }
  return wpath.substr(0, slash);
}

// 将 data 插入到 file_path 的"第 line 行之后"；
// line == 0 表示插到文件头；line 超出总行数则追加到文件尾。
std::string file_insert_line(const char* file_path, size_t line,
                                    const char* data, size_t len) {
  // ---------- 参数校验 ----------
  if (!file_path || file_path[0] == '\0') {
    LOG_ERROR("file_insert_line: invalid file_path (null/empty)");
    return "invalid file_path";
  }
  if (!data && len > 0) {
    LOG_ERROR("file_insert_line: data is null but len={} (file={})", len, file_path);
    return "invalid data";
  }

  std::wstring wpath;
  if (!utf8_to_wide(file_path, wpath) || wpath.empty()) {
    LOG_ERROR("file_insert_line: invalid UTF-8 path, file={}", file_path);
    return "invalid path encoding";
  }

          // ---------- 打开源文件 ----------
  UniqueHandle hIn(::CreateFileW(
      wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!hIn.valid()) {
    const DWORD err = ::GetLastError();
    LOG_ERROR("file_insert_line: open '{}' for read failed, GetLastError={}",
              file_path, err);
    return "open failed: " + std::to_string(err);
  }

  LARGE_INTEGER file_size{};
  if (!::GetFileSizeEx(hIn.get(), &file_size)) {
    const DWORD err = ::GetLastError();
    LOG_ERROR("file_insert_line: GetFileSizeEx failed, file={}, GetLastError={}",
              file_path, err);
    return "get size failed: " + std::to_string(err);
  }

          // ---------- 扫描插入点：定位到第 line 行的下一行开头 ----------
  uint64_t insert_pos = 0;
  bool need_leading_newline = false;  // 是否需要在 data 前补一个换行
  if (line > 0) {
    std::vector<char> buf(kScanBufSize);
    uint64_t buf_start = 0;
    size_t   nl_seen   = 0;
    bool     found     = false;
    bool     scan_ok   = true;
    DWORD    scan_err  = 0;

    for (;;) {
      DWORD read_bytes = 0;
      if (!::ReadFile(hIn.get(), buf.data(), static_cast<DWORD>(kScanBufSize),
                      &read_bytes, nullptr)) {
        scan_ok  = false;
        scan_err = ::GetLastError();
        break;
      }
      if (read_bytes == 0) break;

      const char* p    = buf.data();
      DWORD       left = read_bytes;
      while (left > 0) {
        const void* hit = std::memchr(p, '\n', left);
        if (!hit) break;
        const char* nl = static_cast<const char*>(hit);
        if (++nl_seen == line) {
          // 第 line 个换行之后，就是第 line 行的下一行开头
          insert_pos = buf_start
                       + static_cast<uint64_t>(nl - buf.data()) + 1;
          found = true;
          break;
        }
        left -= static_cast<DWORD>(nl - p) + 1;
        p     = nl + 1;
      }
      if (found) break;
      buf_start += read_bytes;
    }

    if (!scan_ok) {
      LOG_ERROR("file_insert_line: ReadFile failed while scanning lines, "
          "file={}, GetLastError={}", file_path, scan_err);
      return "scan failed: " + std::to_string(scan_err);
    }
    if (!found) insert_pos = static_cast<uint64_t>(file_size.QuadPart);

            // 如果插入点落在文件尾，且原文件末尾不是换行，则先补一个换行，
            // 保证 data 从第 line 行的下一行开始，而不是粘在最后一行后面。
    if (len > 0 &&
        insert_pos == static_cast<uint64_t>(file_size.QuadPart) &&
        file_size.QuadPart > 0) {
      LARGE_INTEGER last_pos{};
      last_pos.QuadPart = file_size.QuadPart - 1;

      if (!::SetFilePointerEx(hIn.get(), last_pos, nullptr, FILE_BEGIN)) {
        const DWORD err = ::GetLastError();
        LOG_ERROR("file_insert_line: seek to last byte failed, file={}, "
            "GetLastError={}", file_path, err);
        return "seek last byte failed: " + std::to_string(err);
      }

      char  last_ch = 0;
      DWORD got     = 0;
      if (!::ReadFile(hIn.get(), &last_ch, 1, &got, nullptr)) {
        const DWORD err = ::GetLastError();
        LOG_ERROR("file_insert_line: read last byte failed, file={}, "
            "GetLastError={}", file_path, err);
        return "read last byte failed: " + std::to_string(err);
      }

      if (got == 1 && last_ch != '\n') {
        need_leading_newline = true;
      }
    }
  }

          // ---------- 同目录创建临时文件 ----------
  const std::wstring dir = dir_of(wpath);
  wchar_t tmp_file[MAX_PATH] = {0};
  if (!::GetTempFileNameW(dir.c_str(), L"ins", 0, tmp_file)) {
    const DWORD err = ::GetLastError();
    LOG_ERROR("file_insert_line: GetTempFileNameW failed, file={}, GetLastError={}",
              file_path, err);
    return "temp file failed: " + std::to_string(err);
  }
  TempFileGuard tmp_guard(tmp_file);   // 出错自动删除

  UniqueHandle hOut(::CreateFileW(
      tmp_file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!hOut.valid()) {
    const DWORD err = ::GetLastError();
    LOG_ERROR("file_insert_line: create temp file failed, file={}, GetLastError={}",
              file_path, err);
    return "create temp failed: " + std::to_string(err);
  }

          // ---------- 写临时文件：[0,insert_pos) + (可选换行) + data + [insert_pos,EOF) ----------
  bool  ok        = true;
  DWORD write_err = 0;
  auto fail = [&](DWORD e) { if (ok) { ok = false; write_err = e; } };

          // 1) 头部
  if (insert_pos > 0) {
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(hIn.get(), zero, nullptr, FILE_BEGIN))
      fail(::GetLastError());
    else if (!copy_exact(hIn.get(), hOut.get(), insert_pos))
      fail(::GetLastError());
  }

          // 2) 新数据：若需要，先补换行，确保从 line 的下一行开始
  if (ok && len > 0) {
    if (need_leading_newline) {
      const char nl = '\n';
      if (!write_all(hOut.get(), &nl, 1)) fail(::GetLastError());
    }
    if (ok && !write_all(hOut.get(), data, len)) fail(::GetLastError());
  }

          // 3) 尾部
  if (ok) {
    const uint64_t remaining =
        static_cast<uint64_t>(file_size.QuadPart) - insert_pos;
    if (remaining > 0) {
      if (insert_pos == 0) {
        LARGE_INTEGER zero{};
        if (!::SetFilePointerEx(hIn.get(), zero, nullptr, FILE_BEGIN))
          fail(::GetLastError());
      }
      if (ok && !copy_exact(hIn.get(), hOut.get(), remaining))
        fail(::GetLastError());
    }
  }
  if (ok && !::FlushFileBuffers(hOut.get()))
    fail(::GetLastError());

          // ---------- 关闭句柄（替换前必须先关）----------
  hIn.close();
  hOut.close();

  if (!ok) {
    LOG_ERROR("file_insert_line: write/copy temp failed, file={}, GetLastError={}",
              file_path, write_err);
    return "write failed: " + std::to_string(write_err);
  }

          // ---------- 原子替换 ----------
          // ReplaceFileW 保留原文件的 ACL / 属性 / 时间戳
  if (!::ReplaceFileW(wpath.c_str(), tmp_file, nullptr,
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
    const DWORD err_replace = ::GetLastError();
    if (!::MoveFileExW(tmp_file, wpath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      const DWORD err_move = ::GetLastError();
      LOG_ERROR("file_insert_line: replace failed, file={}, "
          "ReplaceFileW err={}, MoveFileExW err={}",
          file_path, err_replace, err_move);
      return "replace failed: " + std::to_string(err_move);
    }
    LOG_WARN("file_insert_line: fallback MoveFileExW used "
        "(metadata may be lost), file={}, ReplaceFileW err={}",
        file_path, err_replace);
  }
  tmp_guard.dismiss();   // 替换成功后临时文件已不存在

  LOG_INFO("file_insert_line: inserted {} bytes starting at next line after line {} into '{}'",
           len, line, file_path);
  return "success";
}

#endif
/*-file insert-------------------------------------------------------*/

/*-file read-------------------------------------------------------*/
#if defined(__linux__)

[[nodiscard]] bool fail(std::string &out, std::string msg) {
  out = std::move(msg);
  return false;
}

// 线程安全，可移植。等价于 strerror_r，但避免了 GNU/XSI 版本二义性。
std::string errno_str(int e) {
  return std::generic_category().message(e);
}

void append_padded_int(std::string &dst, int64_t n, int width) {
  char buf[32];
  auto res = std::to_chars(buf, buf + sizeof(buf), n);
  const int len = static_cast<int>(res.ptr - buf);
  if (len < width) dst.append(static_cast<std::size_t>(width - len), ' ');
  dst.append(buf, res.ptr);
}

// 读取 [start, end]（1-based，闭区间）。
// 成功：out = "行号: 内容\n" 拼接（行号按 end 的宽度右对齐补空格）。
// 失败：out = 错误信息。
// 仅用于 Linux。
bool file_read_line(const std::string &file_path,
                    int64_t start, int64_t end,
                    std::string &out) {
  out.clear();

  if (file_path.empty())
    return fail(out, "invalid argument: file_path is empty");
  if (start < 1)
    return fail(out, "invalid argument: start=" + std::to_string(start) +
                         " (must be >= 1)");
  if (end < start)
    return fail(out, "invalid argument: end=" + std::to_string(end) +
                         " < start=" + std::to_string(start));
  if (end - start + 1 > kMaxLineSpan)
    return fail(out, "invalid argument: line span " +
                         std::to_string(end - start + 1) +
                         " exceeds limit " + std::to_string(kMaxLineSpan));

  ScopedFd fd(::open(file_path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0)
    return fail(out, "open failed: '" + file_path + "': " + errno_str(errno));

  const int     width  = static_cast<int>(std::to_string(end).size());
  const int64_t want   = end - start + 1;

  std::string result;
  // 保守预估：每行平均 64 字节，最多预留 16 MiB，避免极端 span 下的巨额 reserve。
  const std::size_t hint = static_cast<std::size_t>(want) * 64;
  result.reserve(hint > (16u << 20) ? (16u << 20) : hint);

  std::string line;
  line.reserve(256);

  int64_t cur     = 0;
  bool    got_any = false;
  bool    eof     = false;

  std::array<char, kReadBufSize> chunk{};

  while (!eof && cur < end) {
    const ssize_t n = ::read(fd.get(), chunk.data(), chunk.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return fail(out, "read failed: '" + file_path + "': " +
                           errno_str(errno));
    }
    if (n == 0) { eof = true; break; }

    const char       *p    = chunk.data();
    const char *const cend = p + n;
    while (p < cend && cur < end) {
      const void *found = std::memchr(p, '\n',
                                      static_cast<std::size_t>(cend - p));
      const char *nl      = static_cast<const char *>(found);
      const char *seg_end = nl ? nl : cend;
      line.append(p, static_cast<std::size_t>(seg_end - p));

              // 关键：每段 append 后立即检查，绝不等到整行读完。
      if (line.size() > kMaxLineLen)
        return fail(out, "line " + std::to_string(cur + 1) +
                             " exceeds max length " +
                             std::to_string(kMaxLineLen));

      if (nl) {
        ++cur;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (cur >= start) {
          append_padded_int(result, cur, width);
          result.append(": ");
          result.append(line);
          result.push_back('\n');
          got_any = true;
        }
        line.clear();
        p = nl + 1;
      } else {
        p = seg_end;
      }
    }
  }

          // 文件末尾无换行符的末行。
  if (eof && !line.empty() && cur < end) {
    ++cur;
    if (line.back() == '\r') line.pop_back();
    if (cur >= start) {
      append_padded_int(result, cur, width);
      result.append(": ");
      result.append(line);
      result.push_back('\n');
      got_any = true;
    }
  }

  if (!got_any) {
    return fail(out, "range start " + std::to_string(start) +
                         " is beyond end of file '" + file_path + "'");
  }

  out = std::move(result);
  return true;
}

#elif defined(_WIN32)

std::string WideToUtf8(const wchar_t* w, int len) {
  if (w == nullptr || len <= 0) return {};
  int need = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  if (need <= 0) return {};
  std::string s(static_cast<size_t>(need), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), need, nullptr, nullptr);
  return s;
}

bool PathToWide(const std::string& path, std::wstring& out) {
  if (path.empty()) return false;

  UINT  cp    = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  int   len   = ::MultiByteToWideChar(cp, flags, path.c_str(),
                                  static_cast<int>(path.size()), nullptr, 0);
  if (len <= 0) {
    cp    = CP_ACP;
    flags = 0;
    len   = ::MultiByteToWideChar(cp, flags, path.c_str(),
                                static_cast<int>(path.size()), nullptr, 0);
    if (len <= 0) return false;
  }
  out.resize(static_cast<size_t>(len));
  ::MultiByteToWideChar(cp, flags, path.c_str(),
                        static_cast<int>(path.size()), out.data(), len);
  return true;
}

std::string Win32ErrorString(DWORD err) {
  LPWSTR buf = nullptr;
  DWORD n = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buf), 0, nullptr);

  std::string msg;
  if (n > 0 && buf != nullptr) {
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' ||
                     buf[n - 1] == L' '  || buf[n - 1] == L'\t')) {
      --n;
    }
    msg = WideToUtf8(buf, static_cast<int>(n));
    ::LocalFree(buf);
  }
  if (msg.empty()) msg = "Win32 error " + std::to_string(err);
  return msg;
}

void AppendLine(std::string& out, int lineNo, const std::string& content) {
  char num[16];
  int n = std::snprintf(num, sizeof(num), "%d: ", lineNo);
  if (n > 0) out.append(num, static_cast<size_t>(n));
  out.append(content);
  out.push_back('\n');
}

bool file_read_line(const std::string& file_path, int start, int end,
                           std::string& out) {
  out.clear();

          // ---------- 1. 参数校验 ----------
  if (file_path.empty()) {
    out = "file_read_line error: file_path is empty";
    return false;
  }
  if (start < 1) {
    out = "file_read_line error: start must be >= 1 (got " +
          std::to_string(start) + ")";
    return false;
  }
  if (end <= 0) {
    end = (start > INT_MAX - 2000) ? INT_MAX : start + 2000;
  }
  if (end < start) {
    out = "file_read_line error: end (" + std::to_string(end) +
          ") must be >= start (" + std::to_string(start) + ")";
    return false;
  }

          // ---------- 2. 路径 ----------
  std::wstring wpath;
  if (!PathToWide(file_path, wpath)) {
    out = "file_read_line error: cannot convert path to UTF-16: " + file_path;
    return false;
  }

          // ---------- 3. 打开 ----------
  HANDLE hFile = ::CreateFileW(
      wpath.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    out = "file_read_line error: cannot open \"" + file_path +
          "\": " + Win32ErrorString(::GetLastError());
    return false;
  }
  struct HandleGuard {
    HANDLE h;
    ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
  } guard{hFile};

          // ---------- 4. 流式读取 ----------
  std::vector<char> buf(kBufferSize);
  std::string line;
  line.reserve(512);
  out.reserve(std::min<size_t>(kMaxOutputBytes,
                               static_cast<size_t>(4) * 1024 * 1024));

  int  curLine      = 1;
  int  linesWritten = 0;
  bool lineTrunc    = false;   // 当前行因 kMaxLineBytes 被截断
  bool outputTrunc  = false;   // 输出因 kMaxOutputBytes 被截断
  bool stopRead     = false;   // 命中 end 或输出满，不再继续读
  bool firstChunk   = true;
  bool fileEnded    = false;   // 读到 EOF

  while (!stopRead) {
    DWORD bytesRead = 0;
    if (!::ReadFile(hFile, buf.data(), kBufferSize, &bytesRead, nullptr)) {
      out = "file_read_line error: ReadFile failed: " +
            Win32ErrorString(::GetLastError());
      return false;
    }
    if (bytesRead == 0) { fileEnded = true; break; }

    size_t idx = 0;

            // ---- BOM 检测（仅第一次） ----
    if (firstChunk) {
      firstChunk = false;
      if (bytesRead >= 3 &&
          static_cast<unsigned char>(buf[0]) == 0xEF &&
          static_cast<unsigned char>(buf[1]) == 0xBB &&
          static_cast<unsigned char>(buf[2]) == 0xBF) {
        idx = 3;
      } else if (bytesRead >= 2) {
        unsigned char b0 = static_cast<unsigned char>(buf[0]);
        unsigned char b1 = static_cast<unsigned char>(buf[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
          out = "file_read_line error: file is UTF-16LE encoded, "
              "only UTF-8/ANSI is supported: " + file_path;
          return false;
        }
        if (b0 == 0xFE && b1 == 0xFF) {
          out = "file_read_line error: file is UTF-16BE encoded, "
              "only UTF-8/ANSI is supported: " + file_path;
          return false;
        }
      }
    }

            // ---- 逐行扫描 ----
    while (idx < bytesRead) {
      const char* base = buf.data();
      const void* nl = std::memchr(base + idx, '\n', bytesRead - idx);
      size_t nlIdx = nl
                         ? static_cast<size_t>(static_cast<const char*>(nl) - base)
                         : static_cast<size_t>(bytesRead);
      size_t segLen = nlIdx - idx;

              // 追加本段（受行上限约束；一旦超限就只保留首个越界位置）
      if (segLen > 0 && !lineTrunc) {
        size_t room = kMaxLineBytes - line.size();
        if (segLen <= room) {
          line.append(base + idx, segLen);
        } else {
          line.append(base + idx, room);
          lineTrunc = true;
        }
      }

      if (nl == nullptr) break;   // 行未结束，等下一 chunk

              // ---- 一行结束 ----
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (lineTrunc) line += "...[line truncated]";

      if (curLine >= start && curLine <= end) {
        // 预留 snprintf 行号、"..." 与 '\n' 的空间
        if (out.size() + line.size() + 32 > kMaxOutputBytes) {
          outputTrunc = true;
          stopRead    = true;
          break;
        }
        AppendLine(out, curLine, line);
        ++linesWritten;
      }

      ++curLine;
      line.clear();
      lineTrunc = false;
      idx = nlIdx + 1;

      if (curLine > end) { stopRead = true; break; }
    }
  }

          // ---------- 5. 末行（无尾换行符） ----------
          // 只有当读到了文件末尾且当前还有累积内容时才算末行；
          // 若因 stopRead（命中 end / 输出满）退出，说明已处理完所需内容。
  if (fileEnded && !line.empty() && curLine >= start && curLine <= end) {
    if (line.back() == '\r') line.pop_back();
    if (lineTrunc) line += "...[line truncated]";
    if (out.size() + line.size() + 32 <= kMaxOutputBytes) {
      AppendLine(out, curLine, line);
      ++linesWritten;
    } else {
      outputTrunc = true;
    }
  }

          // ---------- 6. 结果 ----------
  if (linesWritten == 0) {
    out = "file_read_line error: line range [" + std::to_string(start) +
          ", " + std::to_string(end) +
          "] contains no content";
    if (fileEnded) {
      // 读到 EOF 时 curLine-1 就是总行数
      out += "; file has only " + std::to_string(curLine - 1) +
             " line(s)";
    }
    out += ": " + file_path;
    return false;
  }

  if (outputTrunc) {
    out += "...[output truncated at ";
    out += std::to_string(kMaxOutputBytes);
    out += " bytes]\n";
  }
  return true;
}

#endif
/*-file read-------------------------------------------------------*/

/*-file delete-------------------------------------------------------*/
// UTF-8 std::string -> UTF-16 std::wstring
std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int src_len = static_cast<int>(s.size());
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      s.data(), src_len, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            s.data(), src_len, w.data(), n) != n) {
    return {};
  }
  return w;
}

std::string file_delete_line(const std::string& file_path, int start, int end)
{
  // ---------- 1. 参数校验 ----------
  if (file_path.empty()) {
    const std::string err = "file_delete_line: file_path is empty";
    LOG_ERROR("{}", err);
    return err;
  }
  if (start < 1) {
    const std::string err = "file_delete_line: start must be >= 1, got " +
                            std::to_string(start);
    LOG_ERROR("{}", err);
    return err;
  }
  if (end < start) {
    const std::string err = "file_delete_line: end(" + std::to_string(end) +
                            ") < start(" + std::to_string(start) + ")";
    LOG_ERROR("{}", err);
    return err;
  }

          // ---------- 2. 路径转换 ----------
  const std::wstring wpath = utf8_to_wide(file_path);
  if (wpath.empty()) {
    const std::string err = "file_delete_line: invalid UTF-8 file_path: " + file_path;
    LOG_ERROR("{}", err);
    return err;
  }

          // ---------- 3. 检查文件 ----------
  const DWORD attrs = ::GetFileAttributesW(wpath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    const DWORD e = ::GetLastError();
    const std::string err = "file_delete_line: file not accessible: " + file_path +
                            " (GetLastError=" + std::to_string(e) + ")";
    LOG_ERROR("{}", err);
    return err;
  }
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    const std::string err = "file_delete_line: path is a directory: " + file_path;
    LOG_ERROR("{}", err);
    return err;
  }

          // 只读文件清掉属性，否则替换会失败
  const bool was_readonly = (attrs & FILE_ATTRIBUTE_READONLY) != 0;
  ReadonlyRestore ro_guard{&wpath, was_readonly};
  if (was_readonly) {
    if (!::SetFileAttributesW(wpath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) {
      const DWORD e = ::GetLastError();
      const std::string err = "file_delete_line: cannot clear read-only on '" + file_path +
                              "' (GetLastError=" + std::to_string(e) + ")";
      LOG_ERROR("{}", err);
      return err;
    }
  }

          // ---------- 4. 打开源文件 ----------
  FILE* raw_in = nullptr;
  if (::_wfopen_s(&raw_in, wpath.c_str(), L"rb") != 0 || raw_in == nullptr) {
    const DWORD e = ::GetLastError();
    const std::string err = "file_delete_line: cannot open for reading: " + file_path +
                            " (GetLastError=" + std::to_string(e) + ")";
    LOG_ERROR("{}", err);
    return err;
  }
  FileGuard in{raw_in};

          // ---------- 5. 创建同目录临时文件 ----------
  std::wstring tmp_dir = wpath;
  const auto sep = tmp_dir.find_last_of(L"\\/");
  if (sep != std::wstring::npos) {
    tmp_dir.resize(sep + 1);
  } else {
    tmp_dir = L".\\";
  }

  wchar_t tmp_path[MAX_PATH] = {};
  if (::GetTempFileNameW(tmp_dir.c_str(), L"fdl", 0, tmp_path) == 0) {
    const DWORD e = ::GetLastError();
    const std::string err = "file_delete_line: GetTempFileNameW failed for: " + file_path +
                            " (GetLastError=" + std::to_string(e) + ")";
    LOG_ERROR("{}", err);
    return err;
  }
  TempGuard temp{std::wstring(tmp_path)};

  FILE* raw_out = nullptr;
  if (::_wfopen_s(&raw_out, tmp_path, L"wb") != 0 || raw_out == nullptr) {
    const DWORD e = ::GetLastError();
    const std::string err = "file_delete_line: cannot create temp file for: " + file_path +
                            " (GetLastError=" + std::to_string(e) + ")";
    LOG_ERROR("{}", err);
    return err;
  }
  FileGuard out{raw_out};

          // ---------- 6. 带缓冲逐块拷贝，跳过 [start, end] ----------
          // 保留原始行尾：\r 被当作普通字节收集，遇 \n 结束一行
  constexpr size_t kBufSize = 64 * 1024;
  std::vector<char> buf(kBufSize);

  bool ok = true;
  std::string err_reason;
  int line_no = 1;
  int total_lines = 0;   // 已确认存在的行数

  while (ok) {
    const size_t n = std::fread(buf.data(), 1, kBufSize, in.get());
    if (n == 0) break;

    size_t i = 0;
    while (i < n) {
      const char* nl = static_cast<const char*>(
          std::memchr(buf.data() + i, '\n', n - i));
      const size_t end_idx = nl ? static_cast<size_t>(nl - buf.data()) : n;
      const size_t chunk_len = end_idx - i;

      const bool keep = (line_no < start) || (line_no > end);

      if (chunk_len > 0) {
        // 该片段属于第 line_no 行 => 第 line_no 行确实存在
        if (line_no > total_lines) total_lines = line_no;
        if (keep &&
            std::fwrite(buf.data() + i, 1, chunk_len, out.get()) != chunk_len) {
          ok = false; err_reason = "fwrite failed"; break;
        }
      }

      if (nl) {
        // 遇到 '\n' => 第 line_no 行（可能是空行）收尾
        if (line_no > total_lines) total_lines = line_no;
        if (keep && std::fputc('\n', out.get()) == EOF) {
          ok = false; err_reason = "fputc failed"; break;
        }
        ++line_no;
        i = end_idx + 1;
      } else {
        i = n;   // 行可能跨块，交给下一次 fread
      }
    }
  }

  if (ok && std::ferror(in.get())) {
    ok = false; err_reason = "read error";
  }

          // ---------- 6b. start 超过文件总行数 -> 报错 ----------
          // 注意：end > total_lines 是允许的（删除到末尾）；
          //       仅当 start 越界时视为失败，原文件保持不变。
  if (ok && start > total_lines) {
    // 先关闭句柄，再让 RAII 清理临时文件
    in.reset();
    out.reset();
    const std::string err =
        "file_delete_line: start(" + std::to_string(start) +
        ") exceeds total lines(" + std::to_string(total_lines) +
        ") in '" + file_path + "'";
    LOG_ERROR("{}", err);
    return err;
  }

  if (ok && std::fflush(out.get()) != 0) {
    ok = false; err_reason = "fflush failed";
  }
  if (ok) {
    // 确保数据落到磁盘，然后才能安全替换
    const int fd = ::_fileno(out.get());
    if (fd >= 0) {
      HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
      if (h != INVALID_HANDLE_VALUE && !::FlushFileBuffers(h)) {
        // 落盘失败通常也代表写失败
        ok = false; err_reason = "FlushFileBuffers failed";
      }
    }
  }

          // 关闭句柄（替换前必须关闭）
  in.reset();
  out.reset();

  if (!ok) {
    const std::string err = "file_delete_line: I/O error on '" + file_path + "': " + err_reason;
    LOG_ERROR("{}", err);
    return err; // TempGuard 自动删除临时文件
  }

          // ---------- 7. 原子替换原文件 ----------
          // 优先 ReplaceFileW：保留 ACL / 创建时间 / 属性等
  if (!::ReplaceFileW(wpath.c_str(), tmp_path, nullptr,
                      REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
    const DWORD replace_err = ::GetLastError();
    // 回退：MoveFileExW
    if (!::MoveFileExW(tmp_path, wpath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      const DWORD e = ::GetLastError();
      const std::string err = "file_delete_line: replace failed for '" + file_path +
                              "' (ReplaceFile=" + std::to_string(replace_err) +
                              ", MoveFileEx=" + std::to_string(e) + ")";
      LOG_ERROR("{}", err);
      return err;
    }
  }
  temp.committed = true;

          // 恢复只读（ro_guard 析构也会做，这里显式让流程清楚）
  if (was_readonly) {
    const DWORD a = ::GetFileAttributesW(wpath.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) {
      ::SetFileAttributesW(wpath.c_str(), a | FILE_ATTRIBUTE_READONLY);
    }
  }
  ro_guard.active = false;

  return "success";
}

/*-file delete-------------------------------------------------------*/

/*-ripgrep-------------------------------------------------------*/
#if defined(__linux__)

#elif defined(_WIN32)

// ---------------- UTF-8 <-> UTF-16 ----------------
std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return L"";
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
  return s;
}

// ---------------- Windows 命令行参数转义（MSVC CRT 规则） ----------------
// 规则：包含空格/引号才需要加引号；反斜杠只在引号前才需要翻倍。
std::wstring quoteArg(const std::wstring& arg) {
  if (arg.empty()) return L"\"\"";
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;

  std::wstring out;
  out.reserve(arg.size() + 4);
  out.push_back(L'"');
  size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') { ++backslashes; continue; }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      out.push_back(c);
      backslashes = 0;
    }
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

// ---------------- 判断一行是否为 “行号:内容” 的匹配行 ----------------
// rg --heading 输出：
//   path/to/file            <-- 文件标题行
//   12:match content        <-- 匹配行
//   15:another match
//   (空行)
bool isMatchLine(const std::string& line) {
  if (line.empty()) return false;
  size_t i = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
  return i > 0 && i < line.size() && line[i] == ':';
}

GrepResult grep(const std::string& pattern,
                       const std::string& path,
                       const std::string& include,
                       size_t             maxInline)
{
  GrepResult r;

  if (pattern.empty()) {
    r.error = "pattern must not be empty";
    return r;
  }

          // ---------- 1. 构造 rg 命令行 ----------
  std::wstring cmd = L"rg";
  cmd += L" --line-number --heading --with-filename --color=never";
  cmd += L" --no-config";               // 忽略用户 .ripgreprc，输出可预期
  cmd += L" --sort path";               // 确定性顺序
  cmd += L" --max-columns 500 --max-columns-preview";

  if (!include.empty()) {
    cmd += L" --glob ";
    cmd += quoteArg(utf8ToWide(include));
  }

  cmd += L" -e ";
  cmd += quoteArg(utf8ToWide(pattern));
  cmd += L" ";
  cmd += path.empty() ? L"." : quoteArg(utf8ToWide(path));

          // ---------- 2. 管道 ----------
  SECURITY_ATTRIBUTES sa{};
  sa.nLength        = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;

  Handle hOutR, hOutW, hErrR, hErrW;
  if (!::CreatePipe(hOutR.addr(), hOutW.addr(), &sa, 0)) {
    r.error = "CreatePipe(stdout) failed: " + std::to_string(::GetLastError());
    return r;
  }
  if (!::CreatePipe(hErrR.addr(), hErrW.addr(), &sa, 0)) {
    r.error = "CreatePipe(stderr) failed: " + std::to_string(::GetLastError());
    return r;
  }
  ::SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(hErrR, HANDLE_FLAG_INHERIT, 0);

  Handle hNul(::CreateFileW(L"NUL", GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, nullptr));

          // ---------- 3. 预创建临时文件（严格校验） ----------
  wchar_t tempDir[MAX_PATH]{};
  DWORD tdLen = ::GetTempPathW(MAX_PATH, tempDir);
  if (tdLen == 0 || tdLen >= MAX_PATH) {
    r.error = "GetTempPathW failed: " + std::to_string(::GetLastError());
    return r;
  }
  wchar_t tempFile[MAX_PATH]{};
  if (::GetTempFileNameW(tempDir, L"rg", 0, tempFile) == 0) {
    r.error = "GetTempFileNameW failed: " + std::to_string(::GetLastError());
    return r;
  }
  Handle hTemp(::CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
  if (!hTemp.valid()) {
    DWORD e = ::GetLastError();
    ::DeleteFileW(tempFile);
    r.error = "CreateFile(temp) failed: " + std::to_string(e);
    return r;
  }

  STARTUPINFOW si{};
  si.cb          = sizeof(STARTUPINFOW);
  si.dwFlags     = STARTF_USESTDHANDLES;
  si.hStdOutput  = hOutW;
  si.hStdError   = hErrW;
  si.hStdInput   = hNul.valid() ? (HANDLE)hNul
                              : ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');

  BOOL created = ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                  &si, &pi);

          // 父进程必须立即关闭写端
  hOutW = Handle{};
  hErrW = Handle{};

  if (!created) {
    DWORD err = ::GetLastError();
    hTemp = Handle{};
    ::DeleteFileW(tempFile);
    r.error = "CreateProcess(rg) failed: " + std::to_string(err)
              + " (确认 rg.exe 在 PATH 中)";
    return r;
  }
  Handle hProc(pi.hProcess);
  Handle hThread(pi.hThread);

          // ---------- 4. 并发读 stderr（防死锁） ----------
  std::string errBuf;
  std::thread errThread([&]{
    char eb[4096];
    DWORD en = 0;
    while (::ReadFile(hErrR, eb, sizeof(eb), &en, nullptr) && en > 0) {
      errBuf.append(eb, en);
    }
  });

          // ---------- 5. 主线程流式读 stdout + 落盘 + 解析 ----------
  std::string inlineOut;
  std::string lineBuf;
  std::string pendingHeader;
  size_t      matchCount = 0;
  bool        truncated  = false;

  auto handleLine = [&](const std::string& line) {
    if (isMatchLine(line)) {
      ++matchCount;
      if (matchCount <= maxInline) {
        if (!pendingHeader.empty()) {
          inlineOut += pendingHeader;
          inlineOut += '\n';
          pendingHeader.clear();
        }
        inlineOut += line;
        inlineOut += '\n';
      } else {
        truncated = true;
      }
    } else if (!line.empty()) {
      if (matchCount < maxInline) {
        pendingHeader = line;
      }
    }
  };

  char  buf[16384];
  DWORD n = 0;
  while (::ReadFile(hOutR, buf, sizeof(buf), &n, nullptr) && n > 0) {
    DWORD written = 0;
    ::WriteFile(hTemp, buf, n, &written, nullptr);
    lineBuf.append(buf, n);
    size_t pos;
    while ((pos = lineBuf.find('\n')) != std::string::npos) {
      std::string line = lineBuf.substr(0, pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      handleLine(line);
      lineBuf.erase(0, pos + 1);
    }
  }
  if (!lineBuf.empty()) {
    if (lineBuf.back() == '\r') lineBuf.pop_back();
    handleLine(lineBuf);
  }
  hOutR = Handle{};

  errThread.join();
  hErrR = Handle{};

          // ---------- 6. 收尾 ----------
  ::WaitForSingleObject(hProc, INFINITE);
  DWORD exitCode = 0;
  ::GetExitCodeProcess(hProc, &exitCode);
  r.exitCode = (int)exitCode;

  hTemp = Handle{};   // 关闭写入句柄，调用方才能读

  r.totalMatches = matchCount;
  r.ok = (exitCode == 0 || exitCode == 1);

  if (!r.ok) {
    ::DeleteFileW(tempFile);        // 出错不留垃圾
    r.error = errBuf.empty()
                  ? ("rg exited with code " + std::to_string(exitCode))
                  : errBuf;
    return r;
  }

  if (truncated) {
    r.truncated    = true;
    r.fullListPath = wideToUtf8(tempFile);
  } else {
    ::DeleteFileW(tempFile);
  }

          // ---------- 7. 组装最终文本 ----------
  std::string out;
  out.reserve(inlineOut.size() + 512);
  out += inlineOut;

  if (matchCount == 0) {
    out += "未找到匹配。\n";
  } else {
    out += "\n【共 " + std::to_string(matchCount) + " 处匹配";
    if (truncated) out += "，长行已截断至 500 列";
    out += "】\n";

    if (truncated) {
      out += "仅内联显示前 " + std::to_string(maxInline)
          + " 处匹配；完整匹配列表已保存至：\n";
      out += "  " + r.fullListPath + "\n";
      out += "可使用 read 工具读取该文件获取全部结果，"
          "或对上方任一文件使用 read 查看上下文。\n";
    } else {
      out += "可使用 read 工具读取上方任一文件查看完整上下文。\n";
    }
  }
  r.content = std::move(out);
  return r;
}

#endif
/*-ripgrep-------------------------------------------------------*/

}; // namespace kw












