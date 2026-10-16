// Opt-in first-chance exception tracer for the game process (env RBRRTX_TRACE_EXCEPTIONS=1, RBRRTX_LOG_DIR set).
//
// For each C++ exception (0xE06D7363) or hard fault it appends to <RBRRTX_LOG_DIR>\rbrrtx_exceptions.log:
//   code, thread, C++ type name and std::exception::what(), the faulting address, and the likely
//   return addresses on the stack (dwords pointing into executable module code right after a CALL),
//   all as module+offset. The first RBRRTX_TRACE_DUMPS (default 0) of them also get a small minidump.
// Exceptions that are thrown and caught normally show up too; the log is capped at 300 entries.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr DWORD kCppException = 0xE06D7363;
constexpr DWORD kHeapCorruption = 0xC0000374;
constexpr DWORD kStackBufferOverrun = 0xC0000409;
constexpr LONG kMaxEntries = 300;

volatile LONG g_entries = 0;
volatile LONG g_dumps = 0;
LONG g_max_dumps = 0;
wchar_t g_dir[MAX_PATH] = {};
CRITICAL_SECTION g_lock;
__declspec(thread) bool t_busy = false;

using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

bool module_of(const void* addr, char* out, size_t cap) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(addr), &m) || !m) {
    sprintf_s(out, cap, "0x%08X", reinterpret_cast<UINT_PTR>(addr));
    return false;
  }
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
  const char* base = path;
  for (DWORD i = 0; i < n; ++i) if (path[i] == '\\') base = path + i + 1;
  sprintf_s(out, cap, "%s+0x%X", base, static_cast<unsigned>(static_cast<const char*>(addr) - reinterpret_cast<const char*>(m)));
  return true;
}

bool is_executable(const void* p) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// True when the bytes before `ret` decode as a CALL, i.e. `ret` is a plausible return address.
bool preceded_by_call(const unsigned char* ret) {
  __try {
    if (ret[-5] == 0xE8) return true;                                  // call rel32
    if (ret[-6] == 0xFF && (ret[-5] & 0x38) == 0x10) return true;      // call [disp32] / [reg+disp32]
    if (ret[-7] == 0xFF && (ret[-6] & 0x38) == 0x10) return true;      // call [sib+disp32]
    if (ret[-3] == 0xFF && (ret[-2] & 0x38) == 0x10) return true;      // call [reg+disp8]
    if (ret[-4] == 0xFF && (ret[-3] & 0x38) == 0x10) return true;      // call [sib+disp8]
    if (ret[-2] == 0xFF && (ret[-1] & 0x38) == 0x10) return true;      // call reg / [reg]
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return false;
}

void cpp_type(const EXCEPTION_RECORD* er, char* type, size_t tcap, char* what, size_t wcap) {
  type[0] = what[0] = 0;
  if (er->NumberParameters < 3) return;
  __try {
    // x86 MSVC: params = { magic, object, ThrowInfo* }; ThrowInfo[3] = CatchableTypeArray*;
    // CatchableTypeArray = { count, CatchableType*[] }; CatchableType[1] = TypeDescriptor*; name at +8.
    const unsigned char* obj = reinterpret_cast<const unsigned char*>(er->ExceptionInformation[1]);
    const DWORD* throw_info = reinterpret_cast<const DWORD*>(er->ExceptionInformation[2]);
    const DWORD* types = reinterpret_cast<const DWORD*>(throw_info[3]);
    bool is_std = false;
    for (DWORD i = 0; i < types[0] && i < 8; ++i) {
      const DWORD* ct = reinterpret_cast<const DWORD*>(types[1 + i]);
      const char* name = reinterpret_cast<const char*>(ct[1] + 8);
      if (i == 0) strncpy_s(type, tcap, name, _TRUNCATE);
      if (strstr(name, "exception@std@@")) is_std = true;
    }
    if (is_std && obj) {
      const char* w = *reinterpret_cast<const char* const*>(obj + 4);   // __std_exception_data::_What
      if (w) strncpy_s(what, wcap, w, _TRUNCATE);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void write_dump(PEXCEPTION_POINTERS ep, int index) {
  HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
  auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
  if (!write) return;
  wchar_t path[MAX_PATH * 2];
  swprintf_s(path, L"%s\\rbrrtx_exception_%d_%lu.dmp", g_dir, index, GetCurrentProcessId());
  HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
  write(GetCurrentProcess(), GetCurrentProcessId(), f,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo),
        &mei, nullptr, nullptr);
  CloseHandle(f);
}

LONG CALLBACK on_exception(PEXCEPTION_POINTERS ep) {
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  const DWORD code = er->ExceptionCode;
  if (code != kCppException && code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != kHeapCorruption && code != kStackBufferOverrun)
    return EXCEPTION_CONTINUE_SEARCH;
  if (t_busy) return EXCEPTION_CONTINUE_SEARCH;
  const LONG entry = InterlockedIncrement(&g_entries);
  if (entry > kMaxEntries) return EXCEPTION_CONTINUE_SEARCH;
  t_busy = true;

  char text[4096];
  int len = 0;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char where[160];
  module_of(er->ExceptionAddress, where, sizeof(where));
  len += sprintf_s(text + len, sizeof(text) - len, "#%ld %02u:%02u:%02u.%03u thread %lu code 0x%08lX at %s",
                   entry, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), code, where);
  if (code == kCppException) {
    char type[256], what[512];
    cpp_type(er, type, sizeof(type), what, sizeof(what));
    len += sprintf_s(text + len, sizeof(text) - len, "\n  type %s%s%s", type[0] ? type : "?", what[0] ? "  what: " : "", what);
  } else if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    len += sprintf_s(text + len, sizeof(text) - len, "\n  %s 0x%08X", er->ExceptionInformation[0] ? "write to" : "read from",
                     static_cast<unsigned>(er->ExceptionInformation[1]));
  }

  // Scan the stack from ESP upward for plausible return addresses.
  const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const DWORD* sp = reinterpret_cast<const DWORD*>(ep->ContextRecord->Esp);
  const DWORD* top = reinterpret_cast<const DWORD*>(tib->StackBase);
  int frames = 0;
  DWORD last = 0;
  for (const DWORD* p = sp; p < top && p < sp + 0x1000 && frames < 24; ++p) {
    DWORD v = 0;
    __try { v = *p; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    if (v < 0x10000 || v == last || !is_executable(reinterpret_cast<const void*>(v))) continue;
    if (!preceded_by_call(reinterpret_cast<const unsigned char*>(v))) continue;
    char m[160];
    if (!module_of(reinterpret_cast<const void*>(v), m, sizeof(m))) continue;
    len += sprintf_s(text + len, sizeof(text) - len, "\n  [sp+0x%03X] %s", static_cast<unsigned>((p - sp) * 4), m);
    last = v;
    ++frames;
  }
  text[sizeof(text) - 1] = 0;

  EnterCriticalSection(&g_lock);
  wchar_t path[MAX_PATH * 2];
  swprintf_s(path, L"%s\\rbrrtx_exceptions.log", g_dir);
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"a") == 0 && f) { fprintf(f, "%s\n", text); fclose(f); }
  if (InterlockedIncrement(&g_dumps) <= g_max_dumps) write_dump(ep, static_cast<int>(g_dumps));
  LeaveCriticalSection(&g_lock);

  t_busy = false;
  return EXCEPTION_CONTINUE_SEARCH;   // observe only; the game's own handling continues
}

}  // namespace

void rbrrtx_exception_trace_install() {
  wchar_t flag[8];
  if (GetEnvironmentVariableW(L"RBRRTX_TRACE_EXCEPTIONS", flag, 8) == 0 || flag[0] != L'1') return;
  DWORD n = GetEnvironmentVariableW(L"RBRRTX_LOG_DIR", g_dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  wchar_t dumps[8];
  if (GetEnvironmentVariableW(L"RBRRTX_TRACE_DUMPS", dumps, 8)) g_max_dumps = _wtoi(dumps);
  InitializeCriticalSection(&g_lock);
  AddVectoredExceptionHandler(1, on_exception);
}
