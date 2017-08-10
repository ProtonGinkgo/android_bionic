/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <locale.h>

#include <string>
#include <vector>

#include "BionicDeathTest.h"
#include "TemporaryFile.h"
#include "utils.h"

#if defined(NOFORTIFY)
#define STDIO_TEST stdio_nofortify
#define STDIO_DEATHTEST stdio_nofortify_DeathTest
#else
#define STDIO_TEST stdio
#define STDIO_DEATHTEST stdio_DeathTest
#endif

using namespace std::string_literals;

class stdio_DeathTest : public BionicDeathTest {};
class stdio_nofortify_DeathTest : public BionicDeathTest {};

static void SetFileTo(const char* path, const char* content) {
  FILE* fp;
  ASSERT_NE(nullptr, fp = fopen(path, "w"));
  ASSERT_NE(EOF, fputs(content, fp));
  ASSERT_EQ(0, fclose(fp));
}

static void AssertFileIs(const char* path, const char* expected) {
  FILE* fp;
  ASSERT_NE(nullptr, fp = fopen(path, "r"));
  char* line = nullptr;
  size_t length;
  ASSERT_NE(EOF, getline(&line, &length, fp));
  ASSERT_EQ(0, fclose(fp));
  ASSERT_STREQ(expected, line);
  free(line);
}

static void AssertFileIs(FILE* fp, const char* expected, bool is_fmemopen = false) {
  rewind(fp);

  char line[1024];
  memset(line, 0xff, sizeof(line));
  ASSERT_EQ(line, fgets(line, sizeof(line), fp));
  ASSERT_STREQ(expected, line);

  if (is_fmemopen) {
    // fmemopen appends a trailing NUL byte, which probably shouldn't show up as an
    // extra empty line, but does on every C library I tested...
    ASSERT_EQ(line, fgets(line, sizeof(line), fp));
    ASSERT_STREQ("", line);
  }

  // Make sure there isn't anything else in the file.
  ASSERT_EQ(nullptr, fgets(line, sizeof(line), fp)) << "junk at end of file: " << line;
}

TEST(STDIO_TEST, flockfile_18208568_stderr) {
  // Check that we have a _recursive_ mutex for flockfile.
  flockfile(stderr);
  feof(stderr); // We don't care about the result, but this needs to take the lock.
  funlockfile(stderr);
}

TEST(STDIO_TEST, flockfile_18208568_regular) {
  // We never had a bug for streams other than stdin/stdout/stderr, but test anyway.
  FILE* fp = fopen("/dev/null", "w");
  ASSERT_TRUE(fp != NULL);
  flockfile(fp);
  feof(fp);
  funlockfile(fp);
  fclose(fp);
}

TEST(STDIO_TEST, tmpfile_fileno_fprintf_rewind_fgets) {
  FILE* fp = tmpfile();
  ASSERT_TRUE(fp != NULL);

  int fd = fileno(fp);
  ASSERT_NE(fd, -1);

  struct stat sb;
  int rc = fstat(fd, &sb);
  ASSERT_NE(rc, -1);
  ASSERT_EQ(sb.st_mode & 0777, 0600U);

  rc = fprintf(fp, "hello\n");
  ASSERT_EQ(rc, 6);

  AssertFileIs(fp, "hello\n");
  fclose(fp);
}

TEST(STDIO_TEST, tmpfile64) {
  FILE* fp = tmpfile64();
  ASSERT_TRUE(fp != nullptr);
  fclose(fp);
}

TEST(STDIO_TEST, dprintf) {
  TemporaryFile tf;

  int rc = dprintf(tf.fd, "hello\n");
  ASSERT_EQ(rc, 6);

  lseek(tf.fd, 0, SEEK_SET);
  FILE* tfile = fdopen(tf.fd, "r");
  ASSERT_TRUE(tfile != NULL);

  AssertFileIs(tfile, "hello\n");
  fclose(tfile);
}

TEST(STDIO_TEST, getdelim) {
  FILE* fp = tmpfile();
  ASSERT_TRUE(fp != NULL);

  const char* line_written = "This  is a test";
  int rc = fprintf(fp, "%s", line_written);
  ASSERT_EQ(rc, static_cast<int>(strlen(line_written)));

  rewind(fp);

  char* word_read = NULL;
  size_t allocated_length = 0;

  const char* expected[] = { "This ", " ", "is ", "a ", "test" };
  for (size_t i = 0; i < 5; ++i) {
    ASSERT_FALSE(feof(fp));
    ASSERT_EQ(getdelim(&word_read, &allocated_length, ' ', fp), static_cast<int>(strlen(expected[i])));
    ASSERT_GE(allocated_length, strlen(expected[i]));
    ASSERT_STREQ(expected[i], word_read);
  }
  // The last read should have set the end-of-file indicator for the stream.
  ASSERT_TRUE(feof(fp));
  clearerr(fp);

  // getdelim returns -1 but doesn't set errno if we're already at EOF.
  // It should set the end-of-file indicator for the stream, though.
  errno = 0;
  ASSERT_EQ(getdelim(&word_read, &allocated_length, ' ', fp), -1);
  ASSERT_EQ(0, errno);
  ASSERT_TRUE(feof(fp));

  free(word_read);
  fclose(fp);
}

TEST(STDIO_TEST, getdelim_invalid) {
  FILE* fp = tmpfile();
  ASSERT_TRUE(fp != NULL);

  char* buffer = NULL;
  size_t buffer_length = 0;

  // The first argument can't be NULL.
  errno = 0;
  ASSERT_EQ(getdelim(NULL, &buffer_length, ' ', fp), -1);
  ASSERT_EQ(EINVAL, errno);

  // The second argument can't be NULL.
  errno = 0;
  ASSERT_EQ(getdelim(&buffer, NULL, ' ', fp), -1);
  ASSERT_EQ(EINVAL, errno);

  // The underlying fd can't be closed.
  ASSERT_EQ(0, close(fileno(fp)));
  errno = 0;
  ASSERT_EQ(getdelim(&buffer, &buffer_length, ' ', fp), -1);
  ASSERT_EQ(EBADF, errno);
  fclose(fp);
}

TEST(STDIO_TEST, getdelim_directory) {
  FILE* fp = fopen("/proc", "r");
  ASSERT_TRUE(fp != NULL);
  char* word_read;
  size_t allocated_length;
  ASSERT_EQ(-1, getdelim(&word_read, &allocated_length, ' ', fp));
  fclose(fp);
}

TEST(STDIO_TEST, getline) {
  FILE* fp = tmpfile();
  ASSERT_TRUE(fp != NULL);

  const char* line_written = "This is a test for getline\n";
  const size_t line_count = 5;

  for (size_t i = 0; i < line_count; ++i) {
    int rc = fprintf(fp, "%s", line_written);
    ASSERT_EQ(rc, static_cast<int>(strlen(line_written)));
  }

  rewind(fp);

  char* line_read = NULL;
  size_t allocated_length = 0;

  size_t read_line_count = 0;
  ssize_t read_char_count;
  while ((read_char_count = getline(&line_read, &allocated_length, fp)) != -1) {
    ASSERT_EQ(read_char_count, static_cast<int>(strlen(line_written)));
    ASSERT_GE(allocated_length, strlen(line_written));
    ASSERT_STREQ(line_written, line_read);
    ++read_line_count;
  }
  ASSERT_EQ(read_line_count, line_count);

  // The last read should have set the end-of-file indicator for the stream.
  ASSERT_TRUE(feof(fp));
  clearerr(fp);

  // getline returns -1 but doesn't set errno if we're already at EOF.
  // It should set the end-of-file indicator for the stream, though.
  errno = 0;
  ASSERT_EQ(getline(&line_read, &allocated_length, fp), -1);
  ASSERT_EQ(0, errno);
  ASSERT_TRUE(feof(fp));

  free(line_read);
  fclose(fp);
}

TEST(STDIO_TEST, getline_invalid) {
  FILE* fp = tmpfile();
  ASSERT_TRUE(fp != NULL);

  char* buffer = NULL;
  size_t buffer_length = 0;

  // The first argument can't be NULL.
  errno = 0;
  ASSERT_EQ(getline(NULL, &buffer_length, fp), -1);
  ASSERT_EQ(EINVAL, errno);

  // The second argument can't be NULL.
  errno = 0;
  ASSERT_EQ(getline(&buffer, NULL, fp), -1);
  ASSERT_EQ(EINVAL, errno);

  // The underlying fd can't be closed.
  ASSERT_EQ(0, close(fileno(fp)));
  errno = 0;
  ASSERT_EQ(getline(&buffer, &buffer_length, fp), -1);
  ASSERT_EQ(EBADF, errno);
  fclose(fp);
}

TEST(STDIO_TEST, printf_ssize_t) {
  // http://b/8253769
  ASSERT_EQ(sizeof(ssize_t), sizeof(long int));
  ASSERT_EQ(sizeof(ssize_t), sizeof(size_t));
  // For our 32-bit ABI, we had a ssize_t definition that confuses GCC into saying:
  // error: format '%zd' expects argument of type 'signed size_t',
  //     but argument 4 has type 'ssize_t {aka long int}' [-Werror=format]
  ssize_t v = 1;
  char buf[32];
  snprintf(buf, sizeof(buf), "%zd", v);
}

// https://code.google.com/p/android/issues/detail?id=64886
TEST(STDIO_TEST, snprintf_a) {
  char buf[BUFSIZ];
  EXPECT_EQ(23, snprintf(buf, sizeof(buf), "<%a>", 9990.235));
  EXPECT_STREQ("<0x1.3831e147ae148p+13>", buf);
}

TEST(STDIO_TEST, snprintf_lc) {
  char buf[BUFSIZ];
  wint_t wc = L'a';
  EXPECT_EQ(3, snprintf(buf, sizeof(buf), "<%lc>", wc));
  EXPECT_STREQ("<a>", buf);
}

TEST(STDIO_TEST, snprintf_ls) {
  char buf[BUFSIZ];
  wchar_t* ws = NULL;
  EXPECT_EQ(8, snprintf(buf, sizeof(buf), "<%ls>", ws));
  EXPECT_STREQ("<(null)>", buf);

  wchar_t chars[] = { L'h', L'i', 0 };
  ws = chars;
  EXPECT_EQ(4, snprintf(buf, sizeof(buf), "<%ls>", ws));
  EXPECT_STREQ("<hi>", buf);
}

TEST(STDIO_TEST, snprintf_n) {
#if defined(__BIONIC__)
  // http://b/14492135
  char buf[32];
  int i = 1234;
  EXPECT_EQ(5, snprintf(buf, sizeof(buf), "a %n b", &i));
  EXPECT_EQ(1234, i);
  EXPECT_STREQ("a n b", buf);
#else
  GTEST_LOG_(INFO) << "This test does nothing on glibc.\n";
#endif
}

TEST(STDIO_TEST, snprintf_smoke) {
  char buf[BUFSIZ];

  snprintf(buf, sizeof(buf), "a");
  EXPECT_STREQ("a", buf);

  snprintf(buf, sizeof(buf), "%%");
  EXPECT_STREQ("%", buf);

  snprintf(buf, sizeof(buf), "01234");
  EXPECT_STREQ("01234", buf);

  snprintf(buf, sizeof(buf), "a%sb", "01234");
  EXPECT_STREQ("a01234b", buf);

  char* s = NULL;
  snprintf(buf, sizeof(buf), "a%sb", s);
  EXPECT_STREQ("a(null)b", buf);

  snprintf(buf, sizeof(buf), "aa%scc", "bb");
  EXPECT_STREQ("aabbcc", buf);

  snprintf(buf, sizeof(buf), "a%cc", 'b');
  EXPECT_STREQ("abc", buf);

  snprintf(buf, sizeof(buf), "a%db", 1234);
  EXPECT_STREQ("a1234b", buf);

  snprintf(buf, sizeof(buf), "a%db", -8123);
  EXPECT_STREQ("a-8123b", buf);

  snprintf(buf, sizeof(buf), "a%hdb", static_cast<short>(0x7fff0010));
  EXPECT_STREQ("a16b", buf);

  snprintf(buf, sizeof(buf), "a%hhdb", static_cast<char>(0x7fffff10));
  EXPECT_STREQ("a16b", buf);

  snprintf(buf, sizeof(buf), "a%lldb", 0x1000000000LL);
  EXPECT_STREQ("a68719476736b", buf);

  snprintf(buf, sizeof(buf), "a%ldb", 70000L);
  EXPECT_STREQ("a70000b", buf);

  snprintf(buf, sizeof(buf), "a%pb", reinterpret_cast<void*>(0xb0001234));
  EXPECT_STREQ("a0xb0001234b", buf);

  snprintf(buf, sizeof(buf), "a%xz", 0x12ab);
  EXPECT_STREQ("a12abz", buf);

  snprintf(buf, sizeof(buf), "a%Xz", 0x12ab);
  EXPECT_STREQ("a12ABz", buf);

  snprintf(buf, sizeof(buf), "a%08xz", 0x123456);
  EXPECT_STREQ("a00123456z", buf);

  snprintf(buf, sizeof(buf), "a%5dz", 1234);
  EXPECT_STREQ("a 1234z", buf);

  snprintf(buf, sizeof(buf), "a%05dz", 1234);
  EXPECT_STREQ("a01234z", buf);

  snprintf(buf, sizeof(buf), "a%8dz", 1234);
  EXPECT_STREQ("a    1234z", buf);

  snprintf(buf, sizeof(buf), "a%-8dz", 1234);
  EXPECT_STREQ("a1234    z", buf);

  snprintf(buf, sizeof(buf), "A%-11sZ", "abcdef");
  EXPECT_STREQ("Aabcdef     Z", buf);

  snprintf(buf, sizeof(buf), "A%s:%dZ", "hello", 1234);
  EXPECT_STREQ("Ahello:1234Z", buf);

  snprintf(buf, sizeof(buf), "a%03d:%d:%02dz", 5, 5, 5);
  EXPECT_STREQ("a005:5:05z", buf);

  void* p = NULL;
  snprintf(buf, sizeof(buf), "a%d,%pz", 5, p);
#if defined(__BIONIC__)
  EXPECT_STREQ("a5,0x0z", buf);
#else // __BIONIC__
  EXPECT_STREQ("a5,(nil)z", buf);
#endif // __BIONIC__

  snprintf(buf, sizeof(buf), "a%lld,%d,%d,%dz", 0x1000000000LL, 6, 7, 8);
  EXPECT_STREQ("a68719476736,6,7,8z", buf);

  snprintf(buf, sizeof(buf), "a_%f_b", 1.23f);
  EXPECT_STREQ("a_1.230000_b", buf);

  snprintf(buf, sizeof(buf), "a_%g_b", 3.14);
  EXPECT_STREQ("a_3.14_b", buf);

  snprintf(buf, sizeof(buf), "%1$s %1$s", "print_me_twice");
  EXPECT_STREQ("print_me_twice print_me_twice", buf);
}

template <typename T>
static void CheckInfNan(int snprintf_fn(T*, size_t, const T*, ...),
                        int sscanf_fn(const T*, const T*, ...),
                        const T* fmt_string, const T* fmt, const T* fmt_plus,
                        const T* minus_inf, const T* inf_, const T* plus_inf,
                        const T* minus_nan, const T* nan_, const T* plus_nan) {
  T buf[BUFSIZ];
  float f;

  // NaN.

  snprintf_fn(buf, sizeof(buf), fmt, nanf(""));
  EXPECT_STREQ(nan_, buf) << fmt;
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f));
  EXPECT_TRUE(isnan(f));

  snprintf_fn(buf, sizeof(buf), fmt, -nanf(""));
  EXPECT_STREQ(minus_nan, buf) << fmt;
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f));
  EXPECT_TRUE(isnan(f));

  snprintf_fn(buf, sizeof(buf), fmt_plus, nanf(""));
  EXPECT_STREQ(plus_nan, buf) << fmt_plus;
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f));
  EXPECT_TRUE(isnan(f));

  snprintf_fn(buf, sizeof(buf), fmt_plus, -nanf(""));
  EXPECT_STREQ(minus_nan, buf) << fmt_plus;
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f));
  EXPECT_TRUE(isnan(f));

  // Inf.

  snprintf_fn(buf, sizeof(buf), fmt, HUGE_VALF);
  EXPECT_STREQ(inf_, buf) << fmt;
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f));
  EXPECT_EQ(HUGE_VALF, f);

  snprintf_fn(buf, sizeof(buf), fmt, -HUGE_VALF);
  EXPECT_STREQ(minus_inf, buf) << fmt;
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f));
  EXPECT_EQ(-HUGE_VALF, f);

  snprintf_fn(buf, sizeof(buf), fmt_plus, HUGE_VALF);
  EXPECT_STREQ(plus_inf, buf) << fmt_plus;
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f));
  EXPECT_EQ(HUGE_VALF, f);

  snprintf_fn(buf, sizeof(buf), fmt_plus, -HUGE_VALF);
  EXPECT_STREQ(minus_inf, buf) << fmt_plus;
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f));
  EXPECT_EQ(-HUGE_VALF, f);

  // Check case-insensitivity.
  snprintf_fn(buf, sizeof(buf), fmt_string, "[InFiNiTy]");
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f)) << buf;
  EXPECT_EQ(HUGE_VALF, f);
  snprintf_fn(buf, sizeof(buf), fmt_string, "[NaN]");
  EXPECT_EQ(1, sscanf_fn(buf, fmt, &f)) << buf;
  EXPECT_TRUE(isnan(f));
}

TEST(STDIO_TEST, snprintf_sscanf_inf_nan) {
  CheckInfNan(snprintf, sscanf, "%s",
              "[%a]", "[%+a]",
              "[-inf]", "[inf]", "[+inf]",
              "[-nan]", "[nan]", "[+nan]");
  CheckInfNan(snprintf, sscanf, "%s",
              "[%A]", "[%+A]",
              "[-INF]", "[INF]", "[+INF]",
              "[-NAN]", "[NAN]", "[+NAN]");
  CheckInfNan(snprintf, sscanf, "%s",
              "[%e]", "[%+e]",
              "[-inf]", "[inf]", "[+inf]",
              "[-nan]", "[nan]", "[+nan]");
  CheckInfNan(snprintf, sscanf, "%s",
              "[%E]", "[%+E]",
              "[-INF]", "[INF]", "[+INF]",
              "[-NAN]", "[NAN]", "[+NAN]");
  CheckInfNan(snprintf, sscanf, "%s",
              "[%f]", "[%+f]",
              "[-inf]", "[inf]", "[+inf]",
              "[-nan]", "[nan]", "[+nan]");
  CheckInfNan(snprintf, sscanf, "%s",
              "[%F]", "[%+F]",
              "[-INF]", "[INF]", "[+INF]",
              "[-NAN]", "[NAN]", "[+NAN]");
  CheckInfNan(snprintf, sscanf, "%s",
              "[%g]", "[%+g]",
              "[-inf]", "[inf]", "[+inf]",
              "[-nan]", "[nan]", "[+nan]");
  CheckInfNan(snprintf, sscanf, "%s",
              "[%G]", "[%+G]",
              "[-INF]", "[INF]", "[+INF]",
              "[-NAN]", "[NAN]", "[+NAN]");
}

TEST(STDIO_TEST, swprintf_swscanf_inf_nan) {
  CheckInfNan(swprintf, swscanf, L"%s",
              L"[%a]", L"[%+a]",
              L"[-inf]", L"[inf]", L"[+inf]",
              L"[-nan]", L"[nan]", L"[+nan]");
  CheckInfNan(swprintf, swscanf, L"%s",
              L"[%A]", L"[%+A]",
              L"[-INF]", L"[INF]", L"[+INF]",
              L"[-NAN]", L"[NAN]", L"[+NAN]");
  CheckInfNan(swprintf, swscanf, L"%s",
              L"[%e]", L"[%+e]",
              L"[-inf]", L"[inf]", L"[+inf]",
              L"[-nan]", L"[nan]", L"[+nan]");
  CheckInfNan(swprintf, swscanf, L"%s",
              L"[%E]", L"[%+E]",
              L"[-INF]", L"[INF]", L"[+INF]",
              L"[-NAN]", L"[NAN]", L"[+NAN]");
  CheckInfNan(swprintf, swscanf, L"%s",
              L"[%f]", L"[%+f]",
              L"[-inf]", L"[inf]", L"[+inf]",
              L"[-nan]", L"[nan]", L"[+nan]");
  CheckInfNan(swprintf, swscanf, L"%s",
              L"[%F]", L"[%+F]",
              L"[-INF]", L"[INF]", L"[+INF]",
              L"[-NAN]", L"[NAN]", L"[+NAN]");
  CheckInfNan(swprintf, swscanf, L"%s",
              L"[%g]", L"[%+g]",
              L"[-inf]", L"[inf]", L"[+inf]",
              L"[-nan]", L"[nan]", L"[+nan]");
  CheckInfNan(swprintf, swscanf, L"%s",
              L"[%G]", L"[%+G]",
              L"[-INF]", L"[INF]", L"[+INF]",
              L"[-NAN]", L"[NAN]", L"[+NAN]");
}

TEST(STDIO_TEST, swprintf) {
  constexpr size_t nchars = 32;
  wchar_t buf[nchars];

  ASSERT_EQ(2, swprintf(buf, nchars, L"ab")) << strerror(errno);
  ASSERT_EQ(std::wstring(L"ab"), buf);
  ASSERT_EQ(5, swprintf(buf, nchars, L"%s", "abcde"));
  ASSERT_EQ(std::wstring(L"abcde"), buf);

  // Unlike swprintf(), swprintf() returns -1 in case of truncation
  // and doesn't necessarily zero-terminate the output!
  ASSERT_EQ(-1, swprintf(buf, 4, L"%s", "abcde"));

  const char kString[] = "Hello, World";
  ASSERT_EQ(12, swprintf(buf, nchars, L"%s", kString));
  ASSERT_EQ(std::wstring(L"Hello, World"), buf);
  ASSERT_EQ(12, swprintf(buf, 13, L"%s", kString));
  ASSERT_EQ(std::wstring(L"Hello, World"), buf);
}

TEST(STDIO_TEST, swprintf_a) {
  constexpr size_t nchars = 32;
  wchar_t buf[nchars];

  ASSERT_EQ(20, swprintf(buf, nchars, L"%a", 3.1415926535));
  ASSERT_EQ(std::wstring(L"0x1.921fb54411744p+1"), buf);
}

TEST(STDIO_TEST, swprintf_ls) {
  constexpr size_t nchars = 32;
  wchar_t buf[nchars];

  static const wchar_t kWideString[] = L"Hello\uff41 World";
  ASSERT_EQ(12, swprintf(buf, nchars, L"%ls", kWideString));
  ASSERT_EQ(std::wstring(kWideString), buf);
  ASSERT_EQ(12, swprintf(buf, 13, L"%ls", kWideString));
  ASSERT_EQ(std::wstring(kWideString), buf);
}

TEST(STDIO_TEST, snprintf_d_INT_MAX) {
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), "%d", INT_MAX);
  EXPECT_STREQ("2147483647", buf);
}

TEST(STDIO_TEST, snprintf_d_INT_MIN) {
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), "%d", INT_MIN);
  EXPECT_STREQ("-2147483648", buf);
}

TEST(STDIO_TEST, snprintf_ld_LONG_MAX) {
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), "%ld", LONG_MAX);
#if defined(__LP64__)
  EXPECT_STREQ("9223372036854775807", buf);
#else
  EXPECT_STREQ("2147483647", buf);
#endif
}

TEST(STDIO_TEST, snprintf_ld_LONG_MIN) {
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), "%ld", LONG_MIN);
#if defined(__LP64__)
  EXPECT_STREQ("-9223372036854775808", buf);
#else
  EXPECT_STREQ("-2147483648", buf);
#endif
}

TEST(STDIO_TEST, snprintf_lld_LLONG_MAX) {
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), "%lld", LLONG_MAX);
  EXPECT_STREQ("9223372036854775807", buf);
}

TEST(STDIO_TEST, snprintf_lld_LLONG_MIN) {
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), "%lld", LLONG_MIN);
  EXPECT_STREQ("-9223372036854775808", buf);
}

TEST(STDIO_TEST, snprintf_e) {
  char buf[BUFSIZ];

  snprintf(buf, sizeof(buf), "%e", 1.5);
  EXPECT_STREQ("1.500000e+00", buf);

  snprintf(buf, sizeof(buf), "%Le", 1.5l);
  EXPECT_STREQ("1.500000e+00", buf);
}

TEST(STDIO_TEST, snprintf_negative_zero_5084292) {
  char buf[BUFSIZ];

  snprintf(buf, sizeof(buf), "%e", -0.0);
  EXPECT_STREQ("-0.000000e+00", buf);
  snprintf(buf, sizeof(buf), "%E", -0.0);
  EXPECT_STREQ("-0.000000E+00", buf);
  snprintf(buf, sizeof(buf), "%f", -0.0);
  EXPECT_STREQ("-0.000000", buf);
  snprintf(buf, sizeof(buf), "%F", -0.0);
  EXPECT_STREQ("-0.000000", buf);
  snprintf(buf, sizeof(buf), "%g", -0.0);
  EXPECT_STREQ("-0", buf);
  snprintf(buf, sizeof(buf), "%G", -0.0);
  EXPECT_STREQ("-0", buf);
  snprintf(buf, sizeof(buf), "%a", -0.0);
  EXPECT_STREQ("-0x0p+0", buf);
  snprintf(buf, sizeof(buf), "%A", -0.0);
  EXPECT_STREQ("-0X0P+0", buf);
}

TEST(STDIO_TEST, snprintf_utf8_15439554) {
  locale_t cloc = newlocale(LC_ALL, "C.UTF-8", 0);
  locale_t old_locale = uselocale(cloc);

  // http://b/15439554
  char buf[BUFSIZ];

  // 1-byte character.
  snprintf(buf, sizeof(buf), "%dx%d", 1, 2);
  EXPECT_STREQ("1x2", buf);
  // 2-byte character.
  snprintf(buf, sizeof(buf), "%d\xc2\xa2%d", 1, 2);
  EXPECT_STREQ("1¢2", buf);
  // 3-byte character.
  snprintf(buf, sizeof(buf), "%d\xe2\x82\xac%d", 1, 2);
  EXPECT_STREQ("1€2", buf);
  // 4-byte character.
  snprintf(buf, sizeof(buf), "%d\xf0\xa4\xad\xa2%d", 1, 2);
  EXPECT_STREQ("1𤭢2", buf);

  uselocale(old_locale);
  freelocale(cloc);
}

static void* snprintf_small_stack_fn(void*) {
  // Make life (realistically) hard for ourselves by allocating our own buffer for the result.
  char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "/proc/%d", getpid());
  return nullptr;
}

TEST(STDIO_TEST, snprintf_small_stack) {
  // Is it safe to call snprintf on a thread with a small stack?
  // (The snprintf implementation puts some pretty large buffers on the stack.)
  pthread_attr_t a;
  ASSERT_EQ(0, pthread_attr_init(&a));
  ASSERT_EQ(0, pthread_attr_setstacksize(&a, PTHREAD_STACK_MIN));

  pthread_t t;
  ASSERT_EQ(0, pthread_create(&t, &a, snprintf_small_stack_fn, nullptr));
  ASSERT_EQ(0, pthread_join(t, nullptr));
}

TEST(STDIO_TEST, snprintf_asterisk_overflow) {
  char buf[128];
  ASSERT_EQ(5, snprintf(buf, sizeof(buf), "%.*s%c", 4, "hello world", '!'));
  ASSERT_EQ(12, snprintf(buf, sizeof(buf), "%.*s%c", INT_MAX/2, "hello world", '!'));
  ASSERT_EQ(12, snprintf(buf, sizeof(buf), "%.*s%c", INT_MAX-1, "hello world", '!'));
  ASSERT_EQ(12, snprintf(buf, sizeof(buf), "%.*s%c", INT_MAX, "hello world", '!'));
  ASSERT_EQ(12, snprintf(buf, sizeof(buf), "%.*s%c", -1, "hello world", '!'));

  // INT_MAX-1, INT_MAX, INT_MAX+1.
  ASSERT_EQ(12, snprintf(buf, sizeof(buf), "%.2147483646s%c", "hello world", '!'));
  ASSERT_EQ(12, snprintf(buf, sizeof(buf), "%.2147483647s%c", "hello world", '!'));
  ASSERT_EQ(-1, snprintf(buf, sizeof(buf), "%.2147483648s%c", "hello world", '!'));
  ASSERT_EQ(ENOMEM, errno);
}

TEST(STDIO_TEST, fprintf) {
  TemporaryFile tf;

  FILE* tfile = fdopen(tf.fd, "r+");
  ASSERT_TRUE(tfile != nullptr);

  ASSERT_EQ(7, fprintf(tfile, "%d %s", 123, "abc"));
  AssertFileIs(tfile, "123 abc");
  fclose(tfile);
}

TEST(STDIO_TEST, fprintf_failures_7229520) {
  // http://b/7229520
  FILE* fp;

  // Unbuffered case where the fprintf(3) itself fails.
  ASSERT_NE(nullptr, fp = tmpfile());
  setbuf(fp, NULL);
  ASSERT_EQ(4, fprintf(fp, "epic"));
  ASSERT_EQ(0, close(fileno(fp)));
  ASSERT_EQ(-1, fprintf(fp, "fail"));
  ASSERT_EQ(-1, fclose(fp));

  // Buffered case where we won't notice until the fclose(3).
  // It's likely this is what was actually seen in http://b/7229520,
  // and that expecting fprintf to fail is setting yourself up for
  // disappointment. Remember to check fclose(3)'s return value, kids!
  ASSERT_NE(nullptr, fp = tmpfile());
  ASSERT_EQ(4, fprintf(fp, "epic"));
  ASSERT_EQ(0, close(fileno(fp)));
  ASSERT_EQ(4, fprintf(fp, "fail"));
  ASSERT_EQ(-1, fclose(fp));
}

TEST(STDIO_TEST, popen) {
  FILE* fp = popen("cat /proc/version", "r");
  ASSERT_TRUE(fp != NULL);

  char buf[16];
  char* s = fgets(buf, sizeof(buf), fp);
  buf[13] = '\0';
  ASSERT_STREQ("Linux version", s);

  ASSERT_EQ(0, pclose(fp));
}

TEST(STDIO_TEST, getc) {
  FILE* fp = fopen("/proc/version", "r");
  ASSERT_TRUE(fp != NULL);
  ASSERT_EQ('L', getc(fp));
  ASSERT_EQ('i', getc(fp));
  ASSERT_EQ('n', getc(fp));
  ASSERT_EQ('u', getc(fp));
  ASSERT_EQ('x', getc(fp));
  fclose(fp);
}

TEST(STDIO_TEST, putc) {
  FILE* fp = fopen("/proc/version", "r");
  ASSERT_TRUE(fp != NULL);
  ASSERT_EQ(EOF, putc('x', fp));
  fclose(fp);
}

TEST(STDIO_TEST, sscanf_swscanf) {
  struct stuff {
    char s1[123];
    int i1;
    double d1;
    float f1;
    char s2[123];

    void Check() {
      ASSERT_STREQ("hello", s1);
      ASSERT_EQ(123, i1);
      ASSERT_DOUBLE_EQ(1.23, d1);
      ASSERT_FLOAT_EQ(9.0f, f1);
      ASSERT_STREQ("world", s2);
    }
  } s;

  memset(&s, 0, sizeof(s));
  ASSERT_EQ(5, sscanf("  hello 123 1.23 0x1.2p3 world",
                      "%s %i %lf %f %s",
                      s.s1, &s.i1, &s.d1, &s.f1, s.s2));
  s.Check();

  memset(&s, 0, sizeof(s));
  ASSERT_EQ(5, swscanf(L"  hello 123 1.23 0x1.2p3 world",
                       L"%s %i %lf %f %s",
                       s.s1, &s.i1, &s.d1, &s.f1, s.s2));
  s.Check();
}

TEST(STDIO_TEST, cantwrite_EBADF) {
  // If we open a file read-only...
  FILE* fp = fopen("/proc/version", "r");

  // ...all attempts to write to that file should return failure.

  // They should also set errno to EBADF. This isn't POSIX, but it's traditional.
  // glibc gets the wide-character functions wrong.

  errno = 0;
  EXPECT_EQ(EOF, putc('x', fp));
  EXPECT_EQ(EBADF, errno);

  errno = 0;
  EXPECT_EQ(EOF, fprintf(fp, "hello"));
  EXPECT_EQ(EBADF, errno);

  errno = 0;
  EXPECT_EQ(EOF, fwprintf(fp, L"hello"));
#if defined(__BIONIC__)
  EXPECT_EQ(EBADF, errno);
#endif

  errno = 0;
  EXPECT_EQ(0U, fwrite("hello", 1, 2, fp));
  EXPECT_EQ(EBADF, errno);

  errno = 0;
  EXPECT_EQ(EOF, fputs("hello", fp));
  EXPECT_EQ(EBADF, errno);

  errno = 0;
  EXPECT_EQ(WEOF, fputwc(L'x', fp));
#if defined(__BIONIC__)
  EXPECT_EQ(EBADF, errno);
#endif
}

// Tests that we can only have a consistent and correct fpos_t when using
// f*pos functions (i.e. fpos doesn't get inside a multi byte character).
TEST(STDIO_TEST, consistent_fpos_t) {
  ASSERT_STREQ("C.UTF-8", setlocale(LC_CTYPE, "C.UTF-8"));
  uselocale(LC_GLOBAL_LOCALE);

  FILE* fp = tmpfile();
  ASSERT_TRUE(fp != NULL);

  wchar_t mb_one_bytes = L'h';
  wchar_t mb_two_bytes = 0x00a2;
  wchar_t mb_three_bytes = 0x20ac;
  wchar_t mb_four_bytes = 0x24b62;

  // Write to file.
  ASSERT_EQ(mb_one_bytes, static_cast<wchar_t>(fputwc(mb_one_bytes, fp)));
  ASSERT_EQ(mb_two_bytes, static_cast<wchar_t>(fputwc(mb_two_bytes, fp)));
  ASSERT_EQ(mb_three_bytes, static_cast<wchar_t>(fputwc(mb_three_bytes, fp)));
  ASSERT_EQ(mb_four_bytes, static_cast<wchar_t>(fputwc(mb_four_bytes, fp)));

  rewind(fp);

  // Record each character position.
  fpos_t pos1;
  fpos_t pos2;
  fpos_t pos3;
  fpos_t pos4;
  fpos_t pos5;
  EXPECT_EQ(0, fgetpos(fp, &pos1));
  ASSERT_EQ(mb_one_bytes, static_cast<wchar_t>(fgetwc(fp)));
  EXPECT_EQ(0, fgetpos(fp, &pos2));
  ASSERT_EQ(mb_two_bytes, static_cast<wchar_t>(fgetwc(fp)));
  EXPECT_EQ(0, fgetpos(fp, &pos3));
  ASSERT_EQ(mb_three_bytes, static_cast<wchar_t>(fgetwc(fp)));
  EXPECT_EQ(0, fgetpos(fp, &pos4));
  ASSERT_EQ(mb_four_bytes, static_cast<wchar_t>(fgetwc(fp)));
  EXPECT_EQ(0, fgetpos(fp, &pos5));

#if defined(__BIONIC__)
  // Bionic's fpos_t is just an alias for off_t. This is inherited from OpenBSD
  // upstream. Glibc differs by storing the mbstate_t inside its fpos_t. In
  // Bionic (and upstream OpenBSD) the mbstate_t is stored inside the FILE
  // structure.
  ASSERT_EQ(0, static_cast<off_t>(pos1));
  ASSERT_EQ(1, static_cast<off_t>(pos2));
  ASSERT_EQ(3, static_cast<off_t>(pos3));
  ASSERT_EQ(6, static_cast<off_t>(pos4));
  ASSERT_EQ(10, static_cast<off_t>(pos5));
#endif

  // Exercise back and forth movements of the position.
  ASSERT_EQ(0, fsetpos(fp, &pos2));
  ASSERT_EQ(mb_two_bytes, static_cast<wchar_t>(fgetwc(fp)));
  ASSERT_EQ(0, fsetpos(fp, &pos1));
  ASSERT_EQ(mb_one_bytes, static_cast<wchar_t>(fgetwc(fp)));
  ASSERT_EQ(0, fsetpos(fp, &pos4));
  ASSERT_EQ(mb_four_bytes, static_cast<wchar_t>(fgetwc(fp)));
  ASSERT_EQ(0, fsetpos(fp, &pos3));
  ASSERT_EQ(mb_three_bytes, static_cast<wchar_t>(fgetwc(fp)));
  ASSERT_EQ(0, fsetpos(fp, &pos5));
  ASSERT_EQ(WEOF, fgetwc(fp));

  fclose(fp);
}

// Exercise the interaction between fpos and seek.
TEST(STDIO_TEST, fpos_t_and_seek) {
  ASSERT_STREQ("C.UTF-8", setlocale(LC_CTYPE, "C.UTF-8"));
  uselocale(LC_GLOBAL_LOCALE);

  // In glibc-2.16 fseek doesn't work properly in wide mode
  // (https://sourceware.org/bugzilla/show_bug.cgi?id=14543). One workaround is
  // to close and re-open the file. We do it in order to make the test pass
  // with all glibcs.

  TemporaryFile tf;
  FILE* fp = fdopen(tf.fd, "w+");
  ASSERT_TRUE(fp != NULL);

  wchar_t mb_two_bytes = 0x00a2;
  wchar_t mb_three_bytes = 0x20ac;
  wchar_t mb_four_bytes = 0x24b62;

  // Write to file.
  ASSERT_EQ(mb_two_bytes, static_cast<wchar_t>(fputwc(mb_two_bytes, fp)));
  ASSERT_EQ(mb_three_bytes, static_cast<wchar_t>(fputwc(mb_three_bytes, fp)));
  ASSERT_EQ(mb_four_bytes, static_cast<wchar_t>(fputwc(mb_four_bytes, fp)));

  fflush(fp);
  fclose(fp);

  fp = fopen(tf.filename, "r");
  ASSERT_TRUE(fp != NULL);

  // Store a valid position.
  fpos_t mb_two_bytes_pos;
  ASSERT_EQ(0, fgetpos(fp, &mb_two_bytes_pos));

  // Move inside mb_four_bytes with fseek.
  long offset_inside_mb = 6;
  ASSERT_EQ(0, fseek(fp, offset_inside_mb, SEEK_SET));

  // Store the "inside multi byte" position.
  fpos_t pos_inside_mb;
  ASSERT_EQ(0, fgetpos(fp, &pos_inside_mb));
#if defined(__BIONIC__)
  ASSERT_EQ(offset_inside_mb, static_cast<off_t>(pos_inside_mb));
#endif

  // Reading from within a byte should produce an error.
  ASSERT_EQ(WEOF, fgetwc(fp));
  ASSERT_EQ(EILSEQ, errno);

  // Reverting to a valid position should work.
  ASSERT_EQ(0, fsetpos(fp, &mb_two_bytes_pos));
  ASSERT_EQ(mb_two_bytes, static_cast<wchar_t>(fgetwc(fp)));

  // Moving withing a multi byte with fsetpos should work but reading should
  // produce an error.
  ASSERT_EQ(0, fsetpos(fp, &pos_inside_mb));
  ASSERT_EQ(WEOF, fgetwc(fp));
  ASSERT_EQ(EILSEQ, errno);

  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen) {
  char buf[16];
  memset(buf, 0, sizeof(buf));
  FILE* fp = fmemopen(buf, sizeof(buf), "r+");
  ASSERT_EQ('<', fputc('<', fp));
  ASSERT_NE(EOF, fputs("abc>\n", fp));
  fflush(fp);

  // We wrote to the buffer...
  ASSERT_STREQ("<abc>\n", buf);

  // And can read back from the file.
  AssertFileIs(fp, "<abc>\n", true);
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_nullptr) {
  FILE* fp = fmemopen(nullptr, 128, "r+");
  ASSERT_NE(EOF, fputs("xyz\n", fp));

  AssertFileIs(fp, "xyz\n", true);
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_trailing_NUL_byte) {
  FILE* fp;
  char buf[8];

  // POSIX: "When a stream open for writing is flushed or closed, a null byte
  // shall be written at the current position or at the end of the buffer,
  // depending on the size of the contents."
  memset(buf, 'x', sizeof(buf));
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "w"));
  // Even with nothing written (and not in truncate mode), we'll flush a NUL...
  ASSERT_EQ(0, fflush(fp));
  EXPECT_EQ("\0xxxxxxx"s, std::string(buf, buf + sizeof(buf)));
  // Now write and check that the NUL moves along with our writes...
  ASSERT_NE(EOF, fputs("hello", fp));
  ASSERT_EQ(0, fflush(fp));
  EXPECT_EQ("hello\0xx"s, std::string(buf, buf + sizeof(buf)));
  ASSERT_NE(EOF, fputs("wo", fp));
  ASSERT_EQ(0, fflush(fp));
  EXPECT_EQ("hellowo\0"s, std::string(buf, buf + sizeof(buf)));
  ASSERT_EQ(0, fclose(fp));

  // "If a stream open for update is flushed or closed and the last write has
  // advanced the current buffer size, a null byte shall be written at the end
  // of the buffer if it fits."
  memset(buf, 'x', sizeof(buf));
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "r+"));
  // Nothing written yet, so no advance...
  ASSERT_EQ(0, fflush(fp));
  EXPECT_EQ("xxxxxxxx"s, std::string(buf, buf + sizeof(buf)));
  ASSERT_NE(EOF, fputs("hello", fp));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_size) {
  FILE* fp;
  char buf[16];
  memset(buf, 'x', sizeof(buf));

  // POSIX: "The stream shall also maintain the size of the current buffer
  // contents; use of fseek() or fseeko() on the stream with SEEK_END shall
  // seek relative to this size."

  // "For modes r and r+ the size shall be set to the value given by the size
  // argument."
  ASSERT_NE(nullptr, fp = fmemopen(buf, 16, "r"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(16, ftell(fp));
  EXPECT_EQ(16, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(16, ftell(fp));
  EXPECT_EQ(16, ftello(fp));
  ASSERT_EQ(0, fclose(fp));
  ASSERT_NE(nullptr, fp = fmemopen(buf, 16, "r+"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(16, ftell(fp));
  EXPECT_EQ(16, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(16, ftell(fp));
  EXPECT_EQ(16, ftello(fp));
  ASSERT_EQ(0, fclose(fp));

  // "For modes w and w+ the initial size shall be zero..."
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 16, "w"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  EXPECT_EQ(0, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  EXPECT_EQ(0, ftello(fp));
  ASSERT_EQ(0, fclose(fp));
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 16, "w+"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  EXPECT_EQ(0, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  EXPECT_EQ(0, ftello(fp));
  ASSERT_EQ(0, fclose(fp));

  // "...and for modes a and a+ the initial size shall be:
  // 1. Zero, if buf is a null pointer
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 16, "a"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  EXPECT_EQ(0, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  EXPECT_EQ(0, ftello(fp));
  ASSERT_EQ(0, fclose(fp));
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 16, "a+"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  EXPECT_EQ(0, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  EXPECT_EQ(0, ftello(fp));
  ASSERT_EQ(0, fclose(fp));

  // 2. The position of the first null byte in the buffer, if one is found
  memset(buf, 'x', sizeof(buf));
  buf[3] = '\0';
  ASSERT_NE(nullptr, fp = fmemopen(buf, 16, "a"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(3, ftell(fp));
  EXPECT_EQ(3, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(3, ftell(fp));
  EXPECT_EQ(3, ftello(fp));
  ASSERT_EQ(0, fclose(fp));
  memset(buf, 'x', sizeof(buf));
  buf[3] = '\0';
  ASSERT_NE(nullptr, fp = fmemopen(buf, 16, "a+"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(3, ftell(fp));
  EXPECT_EQ(3, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(3, ftell(fp));
  EXPECT_EQ(3, ftello(fp));
  ASSERT_EQ(0, fclose(fp));

  // 3. The value of the size argument, if buf is not a null pointer and no
  // null byte is found.
  memset(buf, 'x', sizeof(buf));
  ASSERT_NE(nullptr, fp = fmemopen(buf, 16, "a"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(16, ftell(fp));
  EXPECT_EQ(16, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(16, ftell(fp));
  EXPECT_EQ(16, ftello(fp));
  ASSERT_EQ(0, fclose(fp));
  memset(buf, 'x', sizeof(buf));
  ASSERT_NE(nullptr, fp = fmemopen(buf, 16, "a+"));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(16, ftell(fp));
  EXPECT_EQ(16, ftello(fp));
  ASSERT_EQ(0, fseeko(fp, 0, SEEK_END));
  EXPECT_EQ(16, ftell(fp));
  EXPECT_EQ(16, ftello(fp));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_SEEK_END) {
  // fseek SEEK_END is relative to the current string length, not the buffer size.
  FILE* fp;
  char buf[8];
  memset(buf, 'x', sizeof(buf));
  strcpy(buf, "str");
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "w+"));
  ASSERT_NE(EOF, fputs("string", fp));
  EXPECT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(static_cast<long>(strlen("string")), ftell(fp));
  EXPECT_EQ(static_cast<off_t>(strlen("string")), ftello(fp));
  EXPECT_EQ(0, fclose(fp));

  // glibc < 2.22 interpreted SEEK_END the wrong way round (subtracting rather
  // than adding).
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "w+"));
  ASSERT_NE(EOF, fputs("54321", fp));
  EXPECT_EQ(0, fseek(fp, -2, SEEK_END));
  EXPECT_EQ('2', fgetc(fp));
  EXPECT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_seek_invalid) {
  char buf[8];
  memset(buf, 'x', sizeof(buf));
  FILE* fp = fmemopen(buf, sizeof(buf), "w");
  ASSERT_TRUE(fp != nullptr);

  // POSIX: "An attempt to seek ... to a negative position or to a position
  // larger than the buffer size given in the size argument shall fail."
  // (There's no mention of what errno should be set to, and glibc doesn't
  // set errno in any of these cases.)
  EXPECT_EQ(-1, fseek(fp, -2, SEEK_SET));
  EXPECT_EQ(-1, fseeko(fp, -2, SEEK_SET));
  EXPECT_EQ(-1, fseek(fp, sizeof(buf) + 1, SEEK_SET));
  EXPECT_EQ(-1, fseeko(fp, sizeof(buf) + 1, SEEK_SET));
}

TEST(STDIO_TEST, fmemopen_read_EOF) {
  // POSIX: "A read operation on the stream shall not advance the current
  // buffer position beyond the current buffer size."
  char buf[8];
  memset(buf, 'x', sizeof(buf));
  FILE* fp = fmemopen(buf, sizeof(buf), "r");
  ASSERT_TRUE(fp != nullptr);
  char buf2[BUFSIZ];
  ASSERT_EQ(8U, fread(buf2, 1, sizeof(buf2), fp));
  // POSIX: "Reaching the buffer size in a read operation shall count as
  // end-of-file.
  ASSERT_TRUE(feof(fp));
  ASSERT_EQ(EOF, fgetc(fp));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_read_null_bytes) {
  // POSIX: "Null bytes in the buffer shall have no special meaning for reads."
  char buf[] = "h\0e\0l\0l\0o";
  FILE* fp = fmemopen(buf, sizeof(buf), "r");
  ASSERT_TRUE(fp != nullptr);
  ASSERT_EQ('h', fgetc(fp));
  ASSERT_EQ(0, fgetc(fp));
  ASSERT_EQ('e', fgetc(fp));
  ASSERT_EQ(0, fgetc(fp));
  ASSERT_EQ('l', fgetc(fp));
  ASSERT_EQ(0, fgetc(fp));
  // POSIX: "The read operation shall start at the current buffer position of
  // the stream."
  char buf2[8];
  memset(buf2, 'x', sizeof(buf2));
  ASSERT_EQ(4U, fread(buf2, 1, sizeof(buf2), fp));
  ASSERT_EQ('l', buf2[0]);
  ASSERT_EQ(0, buf2[1]);
  ASSERT_EQ('o', buf2[2]);
  ASSERT_EQ(0, buf2[3]);
  for (size_t i = 4; i < sizeof(buf2); ++i) ASSERT_EQ('x', buf2[i]) << i;
  ASSERT_TRUE(feof(fp));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_write) {
  FILE* fp;
  char buf[8];

  // POSIX: "A write operation shall start either at the current position of
  // the stream (if mode has not specified 'a' as the first character)..."
  memset(buf, 'x', sizeof(buf));
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "r+"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ(0, fseek(fp, 2, SEEK_SET));
  ASSERT_EQ(' ', fputc(' ', fp));
  EXPECT_EQ("xx xxxxx", std::string(buf, buf + sizeof(buf)));
  ASSERT_EQ(0, fclose(fp));

  // "...or at the current size of the stream (if mode had 'a' as the first
  // character)." (See the fmemopen_size test for what "size" means, but for
  // mode "a", it's the first NUL byte.)
  memset(buf, 'x', sizeof(buf));
  buf[3] = '\0';
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "a+"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ(' ', fputc(' ', fp));
  EXPECT_EQ("xxx \0xxx"s, std::string(buf, buf + sizeof(buf)));
  ASSERT_EQ(0, fclose(fp));

  // "If the current position at the end of the write is larger than the
  // current buffer size, the current buffer size shall be set to the current
  // position." (See the fmemopen_size test for what "size" means, but to
  // query it we SEEK_END with offset 0, and then ftell.)
  memset(buf, 'x', sizeof(buf));
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "w+"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(0, ftell(fp));
  ASSERT_EQ(' ', fputc(' ', fp));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(1, ftell(fp));
  ASSERT_NE(EOF, fputs("123", fp));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(4, ftell(fp));
  EXPECT_EQ(" 123\0xxx"s, std::string(buf, buf + sizeof(buf)));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_write_EOF) {
  // POSIX: "A write operation on the stream shall not advance the current
  // buffer size beyond the size given in the size argument."
  FILE* fp;

  // Scalar writes...
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 4, "w"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ('x', fputc('x', fp));
  ASSERT_EQ('x', fputc('x', fp));
  ASSERT_EQ('x', fputc('x', fp));
  ASSERT_EQ(EOF, fputc('x', fp)); // Only 3 fit because of the implicit NUL.
  ASSERT_EQ(0, fclose(fp));

  // Vector writes...
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 4, "w"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ(3U, fwrite("xxxx", 1, 4, fp));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_initial_position) {
  // POSIX: "The ... current position in the buffer ... shall be initially
  // set to either the beginning of the buffer (for r and w modes) ..."
  char buf[] = "hello\0world";
  FILE* fp;
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "r"));
  EXPECT_EQ(0L, ftell(fp));
  EXPECT_EQ(0, fclose(fp));
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "w"));
  EXPECT_EQ(0L, ftell(fp));
  EXPECT_EQ(0, fclose(fp));
  buf[0] = 'h'; // (Undo the effects of the above.)

  // POSIX: "...or to the first null byte in the buffer (for a modes)."
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "a"));
  EXPECT_EQ(5L, ftell(fp));
  EXPECT_EQ(0, fclose(fp));

  // POSIX: "If no null byte is found in append mode, the initial position
  // shall be set to one byte after the end of the buffer."
  memset(buf, 'x', sizeof(buf));
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "a"));
  EXPECT_EQ(static_cast<long>(sizeof(buf)), ftell(fp));
  EXPECT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_initial_position_allocated) {
  // POSIX: "If buf is a null pointer, the initial position shall always be
  // set to the beginning of the buffer."
  FILE* fp = fmemopen(nullptr, 128, "a+");
  ASSERT_TRUE(fp != nullptr);
  EXPECT_EQ(0L, ftell(fp));
  EXPECT_EQ(0L, fseek(fp, 0, SEEK_SET));
  EXPECT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_zero_length) {
  // POSIX says it's up to the implementation whether or not you can have a
  // zero-length buffer (but "A future version of this standard may require
  // support of zero-length buffer streams explicitly"). BSD and glibc < 2.22
  // agreed that you couldn't, but glibc >= 2.22 allows it for consistency.
  FILE* fp;
  char buf[16];
  ASSERT_NE(nullptr, fp = fmemopen(buf, 0, "r+"));
  ASSERT_EQ(EOF, fgetc(fp));
  ASSERT_TRUE(feof(fp));
  ASSERT_EQ(0, fclose(fp));
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 0, "r+"));
  ASSERT_EQ(EOF, fgetc(fp));
  ASSERT_TRUE(feof(fp));
  ASSERT_EQ(0, fclose(fp));

  ASSERT_NE(nullptr, fp = fmemopen(buf, 0, "w+"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ(EOF, fputc('x', fp));
  ASSERT_EQ(0, fclose(fp));
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 0, "w+"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ(EOF, fputc('x', fp));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_write_only_allocated) {
  // POSIX says fmemopen "may fail if the mode argument does not include a '+'".
  // BSD fails, glibc doesn't. We side with the more lenient.
  FILE* fp;
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 16, "r"));
  ASSERT_EQ(0, fclose(fp));
  ASSERT_NE(nullptr, fp = fmemopen(nullptr, 16, "w"));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_fileno) {
  // There's no fd backing an fmemopen FILE*.
  FILE* fp = fmemopen(nullptr, 16, "r");
  ASSERT_TRUE(fp != nullptr);
  errno = 0;
  ASSERT_EQ(-1, fileno(fp));
  ASSERT_EQ(EBADF, errno);
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, fmemopen_append_after_seek) {
  // In BSD and glibc < 2.22, append mode didn't force writes to append if
  // there had been an intervening seek.

  FILE* fp;
  char buf[] = "hello\0world";
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "a"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ(0, fseek(fp, 0, SEEK_SET));
  ASSERT_NE(EOF, fputc('!', fp));
  EXPECT_EQ("hello!\0orld\0"s, std::string(buf, buf + sizeof(buf)));
  ASSERT_EQ(0, fclose(fp));

  memcpy(buf, "hello\0world", sizeof(buf));
  ASSERT_NE(nullptr, fp = fmemopen(buf, sizeof(buf), "a+"));
  setbuf(fp, nullptr); // Turn off buffering so we can see what's happening as it happens.
  ASSERT_EQ(0, fseek(fp, 0, SEEK_SET));
  ASSERT_NE(EOF, fputc('!', fp));
  EXPECT_EQ("hello!\0orld\0"s, std::string(buf, buf + sizeof(buf)));
  ASSERT_EQ(0, fclose(fp));
}

TEST(STDIO_TEST, open_memstream) {
  char* p = nullptr;
  size_t size = 0;
  FILE* fp = open_memstream(&p, &size);
  ASSERT_NE(EOF, fputs("hello, world!", fp));
  fclose(fp);

  ASSERT_STREQ("hello, world!", p);
  ASSERT_EQ(strlen("hello, world!"), size);
  free(p);
}

TEST(STDIO_TEST, open_memstream_EINVAL) {
#if defined(__BIONIC__)
  char* p;
  size_t size;

  // Invalid buffer.
  errno = 0;
  ASSERT_EQ(nullptr, open_memstream(nullptr, &size));
  ASSERT_EQ(EINVAL, errno);

  // Invalid size.
  errno = 0;
  ASSERT_EQ(nullptr, open_memstream(&p, nullptr));
  ASSERT_EQ(EINVAL, errno);
#else
  GTEST_LOG_(INFO) << "This test does nothing on glibc.\n";
#endif
}

TEST(STDIO_TEST, fdopen_CLOEXEC) {
  int fd = open("/proc/version", O_RDONLY);
  ASSERT_TRUE(fd != -1);

  // This fd doesn't have O_CLOEXEC...
  int flags = fcntl(fd, F_GETFD);
  ASSERT_TRUE(flags != -1);
  ASSERT_EQ(0, flags & FD_CLOEXEC);

  FILE* fp = fdopen(fd, "re");
  ASSERT_TRUE(fp != NULL);

  // ...but the new one does.
  flags = fcntl(fileno(fp), F_GETFD);
  ASSERT_TRUE(flags != -1);
  ASSERT_EQ(FD_CLOEXEC, flags & FD_CLOEXEC);

  fclose(fp);
  close(fd);
}

TEST(STDIO_TEST, freopen_CLOEXEC) {
  FILE* fp = fopen("/proc/version", "r");
  ASSERT_TRUE(fp != NULL);

  // This FILE* doesn't have O_CLOEXEC...
  int flags = fcntl(fileno(fp), F_GETFD);
  ASSERT_TRUE(flags != -1);
  ASSERT_EQ(0, flags & FD_CLOEXEC);

  fp = freopen("/proc/version", "re", fp);

  // ...but the new one does.
  flags = fcntl(fileno(fp), F_GETFD);
  ASSERT_TRUE(flags != -1);
  ASSERT_EQ(FD_CLOEXEC, flags & FD_CLOEXEC);

  fclose(fp);
}

TEST(STDIO_TEST, fopen64_freopen64) {
  FILE* fp = fopen64("/proc/version", "r");
  ASSERT_TRUE(fp != nullptr);
  fp = freopen64("/proc/version", "re", fp);
  ASSERT_TRUE(fp != nullptr);
  fclose(fp);
}

// https://code.google.com/p/android/issues/detail?id=81155
// http://b/18556607
TEST(STDIO_TEST, fread_unbuffered_pathological_performance) {
  FILE* fp = fopen("/dev/zero", "r");
  ASSERT_TRUE(fp != NULL);

  // Make this stream unbuffered.
  setvbuf(fp, 0, _IONBF, 0);

  char buf[65*1024];
  memset(buf, 0xff, sizeof(buf));

  time_t t0 = time(NULL);
  for (size_t i = 0; i < 1024; ++i) {
    ASSERT_EQ(1U, fread(buf, 64*1024, 1, fp));
  }
  time_t t1 = time(NULL);

  fclose(fp);

  // 1024 64KiB reads should have been very quick.
  ASSERT_LE(t1 - t0, 1);

  for (size_t i = 0; i < 64*1024; ++i) {
    ASSERT_EQ('\0', buf[i]);
  }
  for (size_t i = 64*1024; i < 65*1024; ++i) {
    ASSERT_EQ('\xff', buf[i]);
  }
}

TEST(STDIO_TEST, fread_EOF) {
  std::string digits("0123456789");
  FILE* fp = fmemopen(&digits[0], digits.size(), "r");

  // Try to read too much, but little enough that it still fits in the FILE's internal buffer.
  char buf1[4 * 4];
  memset(buf1, 0, sizeof(buf1));
  ASSERT_EQ(2U, fread(buf1, 4, 4, fp));
  ASSERT_STREQ("0123456789", buf1);
  ASSERT_TRUE(feof(fp));

  rewind(fp);

  // Try to read way too much so stdio tries to read more direct from the stream.
  char buf2[4 * 4096];
  memset(buf2, 0, sizeof(buf2));
  ASSERT_EQ(2U, fread(buf2, 4, 4096, fp));
  ASSERT_STREQ("0123456789", buf2);
  ASSERT_TRUE(feof(fp));

  fclose(fp);
}

static void test_fread_from_write_only_stream(size_t n) {
  FILE* fp = fopen("/dev/null", "w");
  std::vector<char> buf(n, 0);
  errno = 0;
  ASSERT_EQ(0U, fread(&buf[0], n, 1, fp));
  ASSERT_EQ(EBADF, errno);
  ASSERT_TRUE(ferror(fp));
  ASSERT_FALSE(feof(fp));
  fclose(fp);
}

TEST(STDIO_TEST, fread_from_write_only_stream_slow_path) {
  test_fread_from_write_only_stream(1);
}

TEST(STDIO_TEST, fread_from_write_only_stream_fast_path) {
  test_fread_from_write_only_stream(64*1024);
}

static void test_fwrite_after_fread(size_t n) {
  TemporaryFile tf;

  FILE* fp = fdopen(tf.fd, "w+");
  ASSERT_EQ(1U, fwrite("1", 1, 1, fp));
  fflush(fp);

  // We've flushed but not rewound, so there's nothing to read.
  std::vector<char> buf(n, 0);
  ASSERT_EQ(0U, fread(&buf[0], 1, buf.size(), fp));
  ASSERT_TRUE(feof(fp));

  // But hitting EOF doesn't prevent us from writing...
  errno = 0;
  ASSERT_EQ(1U, fwrite("2", 1, 1, fp)) << strerror(errno);

  // And if we rewind, everything's there.
  rewind(fp);
  ASSERT_EQ(2U, fread(&buf[0], 1, buf.size(), fp));
  ASSERT_EQ('1', buf[0]);
  ASSERT_EQ('2', buf[1]);

  fclose(fp);
}

TEST(STDIO_TEST, fwrite_after_fread_slow_path) {
  test_fwrite_after_fread(16);
}

TEST(STDIO_TEST, fwrite_after_fread_fast_path) {
  test_fwrite_after_fread(64*1024);
}

// http://b/19172514
TEST(STDIO_TEST, fread_after_fseek) {
  TemporaryFile tf;

  FILE* fp = fopen(tf.filename, "w+");
  ASSERT_TRUE(fp != nullptr);

  char file_data[12288];
  for (size_t i = 0; i < 12288; i++) {
    file_data[i] = i;
  }
  ASSERT_EQ(12288U, fwrite(file_data, 1, 12288, fp));
  fclose(fp);

  fp = fopen(tf.filename, "r");
  ASSERT_TRUE(fp != nullptr);

  char buffer[8192];
  size_t cur_location = 0;
  // Small read to populate internal buffer.
  ASSERT_EQ(100U, fread(buffer, 1, 100, fp));
  ASSERT_EQ(memcmp(file_data, buffer, 100), 0);

  cur_location = static_cast<size_t>(ftell(fp));
  // Large read to force reading into the user supplied buffer and bypassing
  // the internal buffer.
  ASSERT_EQ(8192U, fread(buffer, 1, 8192, fp));
  ASSERT_EQ(memcmp(file_data+cur_location, buffer, 8192), 0);

  // Small backwards seek to verify fseek does not reuse the internal buffer.
  ASSERT_EQ(0, fseek(fp, -22, SEEK_CUR)) << strerror(errno);
  cur_location = static_cast<size_t>(ftell(fp));
  ASSERT_EQ(22U, fread(buffer, 1, 22, fp));
  ASSERT_EQ(memcmp(file_data+cur_location, buffer, 22), 0);

  fclose(fp);
}

// https://code.google.com/p/android/issues/detail?id=184847
TEST(STDIO_TEST, fread_EOF_184847) {
  TemporaryFile tf;
  char buf[6] = {0};

  FILE* fw = fopen(tf.filename, "w");
  ASSERT_TRUE(fw != nullptr);

  FILE* fr = fopen(tf.filename, "r");
  ASSERT_TRUE(fr != nullptr);

  fwrite("a", 1, 1, fw);
  fflush(fw);
  ASSERT_EQ(1U, fread(buf, 1, 1, fr));
  ASSERT_STREQ("a", buf);

  // 'fr' is now at EOF.
  ASSERT_EQ(0U, fread(buf, 1, 1, fr));
  ASSERT_TRUE(feof(fr));

  // Write some more...
  fwrite("z", 1, 1, fw);
  fflush(fw);

  // ...and check that we can read it back.
  // (BSD thinks that once a stream has hit EOF, it must always return EOF. SysV disagrees.)
  ASSERT_EQ(1U, fread(buf, 1, 1, fr));
  ASSERT_STREQ("z", buf);

  // But now we're done.
  ASSERT_EQ(0U, fread(buf, 1, 1, fr));

  fclose(fr);
  fclose(fw);
}

TEST(STDIO_TEST, fclose_invalidates_fd) {
  // The typical error we're trying to help people catch involves accessing
  // memory after it's been freed. But we know that stdin/stdout/stderr are
  // special and don't get deallocated, so this test uses stdin.
  ASSERT_EQ(0, fclose(stdin));

  // Even though using a FILE* after close is undefined behavior, I've closed
  // this bug as "WAI" too many times. We shouldn't hand out stale fds,
  // especially because they might actually correspond to a real stream.
  errno = 0;
  ASSERT_EQ(-1, fileno(stdin));
  ASSERT_EQ(EBADF, errno);
}

TEST(STDIO_TEST, fseek_ftell_unseekable) {
#if defined(__BIONIC__) // glibc has fopencookie instead.
  auto read_fn = [](void*, char*, int) { return -1; };
  FILE* fp = funopen(nullptr, read_fn, nullptr, nullptr, nullptr);
  ASSERT_TRUE(fp != nullptr);

  // Check that ftell balks on an unseekable FILE*.
  errno = 0;
  ASSERT_EQ(-1, ftell(fp));
  ASSERT_EQ(ESPIPE, errno);

  // SEEK_CUR is rewritten as SEEK_SET internally...
  errno = 0;
  ASSERT_EQ(-1, fseek(fp, 0, SEEK_CUR));
  ASSERT_EQ(ESPIPE, errno);

  // ...so it's worth testing the direct seek path too.
  errno = 0;
  ASSERT_EQ(-1, fseek(fp, 0, SEEK_SET));
  ASSERT_EQ(ESPIPE, errno);

  fclose(fp);
#else
  GTEST_LOG_(INFO) << "glibc uses fopencookie instead.\n";
#endif
}

TEST(STDIO_TEST, funopen_EINVAL) {
#if defined(__BIONIC__)
  errno = 0;
  ASSERT_EQ(nullptr, funopen(nullptr, nullptr, nullptr, nullptr, nullptr));
  ASSERT_EQ(EINVAL, errno);
#else
  GTEST_LOG_(INFO) << "glibc uses fopencookie instead.\n";
#endif
}

TEST(STDIO_TEST, funopen_seek) {
#if defined(__BIONIC__)
  auto read_fn = [](void*, char*, int) { return -1; };

  auto seek_fn = [](void*, fpos_t, int) -> fpos_t { return 0xfedcba12; };
  auto seek64_fn = [](void*, fpos64_t, int) -> fpos64_t { return 0xfedcba12345678; };

  FILE* fp = funopen(nullptr, read_fn, nullptr, seek_fn, nullptr);
  ASSERT_TRUE(fp != nullptr);
  fpos_t pos;
#if defined(__LP64__)
  EXPECT_EQ(0, fgetpos(fp, &pos)) << strerror(errno);
  EXPECT_EQ(0xfedcba12LL, pos);
#else
  EXPECT_EQ(-1, fgetpos(fp, &pos)) << strerror(errno);
  EXPECT_EQ(EOVERFLOW, errno);
#endif

  FILE* fp64 = funopen64(nullptr, read_fn, nullptr, seek64_fn, nullptr);
  ASSERT_TRUE(fp64 != nullptr);
  fpos64_t pos64;
  EXPECT_EQ(0, fgetpos64(fp64, &pos64)) << strerror(errno);
  EXPECT_EQ(0xfedcba12345678, pos64);
#else
  GTEST_LOG_(INFO) << "glibc uses fopencookie instead.\n";
#endif
}

TEST(STDIO_TEST, lots_of_concurrent_files) {
  std::vector<TemporaryFile*> tfs;
  std::vector<FILE*> fps;

  for (size_t i = 0; i < 256; ++i) {
    TemporaryFile* tf = new TemporaryFile;
    tfs.push_back(tf);
    FILE* fp = fopen(tf->filename, "w+");
    fps.push_back(fp);
    fprintf(fp, "hello %zu!\n", i);
    fflush(fp);
  }

  for (size_t i = 0; i < 256; ++i) {
    char expected[BUFSIZ];
    snprintf(expected, sizeof(expected), "hello %zu!\n", i);

    AssertFileIs(fps[i], expected);
    fclose(fps[i]);
    delete tfs[i];
  }
}

static void AssertFileOffsetAt(FILE* fp, off64_t offset) {
  EXPECT_EQ(offset, ftell(fp));
  EXPECT_EQ(offset, ftello(fp));
  EXPECT_EQ(offset, ftello64(fp));
  fpos_t pos;
  fpos64_t pos64;
  EXPECT_EQ(0, fgetpos(fp, &pos));
  EXPECT_EQ(0, fgetpos64(fp, &pos64));
#if defined(__BIONIC__)
  EXPECT_EQ(offset, static_cast<off64_t>(pos));
  EXPECT_EQ(offset, static_cast<off64_t>(pos64));
#else
  GTEST_LOG_(INFO) << "glibc's fpos_t is opaque.\n";
#endif
}

TEST(STDIO_TEST, seek_tell_family_smoke) {
  TemporaryFile tf;
  FILE* fp = fdopen(tf.fd, "w+");

  // Initially we should be at 0.
  AssertFileOffsetAt(fp, 0);

  // Seek to offset 8192.
  ASSERT_EQ(0, fseek(fp, 8192, SEEK_SET));
  AssertFileOffsetAt(fp, 8192);
  fpos_t eight_k_pos;
  ASSERT_EQ(0, fgetpos(fp, &eight_k_pos));

  // Seek forward another 8192...
  ASSERT_EQ(0, fseek(fp, 8192, SEEK_CUR));
  AssertFileOffsetAt(fp, 8192 + 8192);
  fpos64_t sixteen_k_pos64;
  ASSERT_EQ(0, fgetpos64(fp, &sixteen_k_pos64));

  // Seek back 8192...
  ASSERT_EQ(0, fseek(fp, -8192, SEEK_CUR));
  AssertFileOffsetAt(fp, 8192);

  // Since we haven't written anything, the end is also at 0.
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  AssertFileOffsetAt(fp, 0);

  // Check that our fpos64_t from 16KiB works...
  ASSERT_EQ(0, fsetpos64(fp, &sixteen_k_pos64));
  AssertFileOffsetAt(fp, 8192 + 8192);
  // ...as does our fpos_t from 8192.
  ASSERT_EQ(0, fsetpos(fp, &eight_k_pos));
  AssertFileOffsetAt(fp, 8192);

  // Do fseeko and fseeko64 work too?
  ASSERT_EQ(0, fseeko(fp, 1234, SEEK_SET));
  AssertFileOffsetAt(fp, 1234);
  ASSERT_EQ(0, fseeko64(fp, 5678, SEEK_SET));
  AssertFileOffsetAt(fp, 5678);

  fclose(fp);
}

TEST(STDIO_TEST, fseek_fseeko_EINVAL) {
  TemporaryFile tf;
  FILE* fp = fdopen(tf.fd, "w+");

  // Bad whence.
  errno = 0;
  ASSERT_EQ(-1, fseek(fp, 0, 123));
  ASSERT_EQ(EINVAL, errno);
  errno = 0;
  ASSERT_EQ(-1, fseeko(fp, 0, 123));
  ASSERT_EQ(EINVAL, errno);
  errno = 0;
  ASSERT_EQ(-1, fseeko64(fp, 0, 123));
  ASSERT_EQ(EINVAL, errno);

  // Bad offset.
  errno = 0;
  ASSERT_EQ(-1, fseek(fp, -1, SEEK_SET));
  ASSERT_EQ(EINVAL, errno);
  errno = 0;
  ASSERT_EQ(-1, fseeko(fp, -1, SEEK_SET));
  ASSERT_EQ(EINVAL, errno);
  errno = 0;
  ASSERT_EQ(-1, fseeko64(fp, -1, SEEK_SET));
  ASSERT_EQ(EINVAL, errno);

  fclose(fp);
}

TEST(STDIO_TEST, ctermid) {
  ASSERT_STREQ("/dev/tty", ctermid(nullptr));

  char buf[L_ctermid] = {};
  ASSERT_EQ(buf, ctermid(buf));
  ASSERT_STREQ("/dev/tty", buf);
}

TEST(STDIO_TEST, remove) {
  struct stat sb;

  TemporaryFile tf;
  ASSERT_EQ(0, remove(tf.filename));
  ASSERT_EQ(-1, lstat(tf.filename, &sb));
  ASSERT_EQ(ENOENT, errno);

  TemporaryDir td;
  ASSERT_EQ(0, remove(td.dirname));
  ASSERT_EQ(-1, lstat(td.dirname, &sb));
  ASSERT_EQ(ENOENT, errno);

  errno = 0;
  ASSERT_EQ(-1, remove(tf.filename));
  ASSERT_EQ(ENOENT, errno);

  errno = 0;
  ASSERT_EQ(-1, remove(td.dirname));
  ASSERT_EQ(ENOENT, errno);
}

TEST(STDIO_DEATHTEST, snprintf_30445072_known_buffer_size) {
  char buf[16];
  ASSERT_EXIT(snprintf(buf, atol("-1"), "hello"),
              testing::KilledBySignal(SIGABRT),
#if defined(NOFORTIFY)
              "FORTIFY: vsnprintf: size .* > SSIZE_MAX"
#else
              "FORTIFY: vsnprintf: prevented .*-byte write into 16-byte buffer"
#endif
              );
}

TEST(STDIO_DEATHTEST, snprintf_30445072_unknown_buffer_size) {
  std::string buf = "world";
  ASSERT_EXIT(snprintf(&buf[0], atol("-1"), "hello"),
              testing::KilledBySignal(SIGABRT),
              "FORTIFY: vsnprintf: size .* > SSIZE_MAX");
}

TEST(STDIO_TEST, sprintf_30445072) {
  std::string buf = "world";
  sprintf(&buf[0], "hello");
  ASSERT_EQ(buf, "hello");
}

TEST(STDIO_TEST, fopen_append_mode_and_ftell) {
  TemporaryFile tf;
  SetFileTo(tf.filename, "0123456789");
  FILE* fp = fopen(tf.filename, "a");
  EXPECT_EQ(10, ftell(fp));
  ASSERT_EQ(0, fseek(fp, 2, SEEK_SET));
  EXPECT_EQ(2, ftell(fp));
  ASSERT_NE(EOF, fputs("xxx", fp));
  ASSERT_EQ(0, fflush(fp));
  EXPECT_EQ(13, ftell(fp));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(13, ftell(fp));
  ASSERT_EQ(0, fclose(fp));
  AssertFileIs(tf.filename, "0123456789xxx");
}

TEST(STDIO_TEST, fdopen_append_mode_and_ftell) {
  TemporaryFile tf;
  SetFileTo(tf.filename, "0123456789");
  int fd = open(tf.filename, O_RDWR);
  ASSERT_NE(-1, fd);
  // POSIX: "The file position indicator associated with the new stream is set to the position
  // indicated by the file offset associated with the file descriptor."
  ASSERT_EQ(4, lseek(fd, 4, SEEK_SET));
  FILE* fp = fdopen(fd, "a");
  EXPECT_EQ(4, ftell(fp));
  ASSERT_EQ(0, fseek(fp, 2, SEEK_SET));
  EXPECT_EQ(2, ftell(fp));
  ASSERT_NE(EOF, fputs("xxx", fp));
  ASSERT_EQ(0, fflush(fp));
  EXPECT_EQ(13, ftell(fp));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(13, ftell(fp));
  ASSERT_EQ(0, fclose(fp));
  AssertFileIs(tf.filename, "0123456789xxx");
}

TEST(STDIO_TEST, freopen_append_mode_and_ftell) {
  TemporaryFile tf;
  SetFileTo(tf.filename, "0123456789");
  FILE* other_fp = fopen("/proc/version", "r");
  FILE* fp = freopen(tf.filename, "a", other_fp);
  EXPECT_EQ(10, ftell(fp));
  ASSERT_EQ(0, fseek(fp, 2, SEEK_SET));
  EXPECT_EQ(2, ftell(fp));
  ASSERT_NE(EOF, fputs("xxx", fp));
  ASSERT_EQ(0, fflush(fp));
  EXPECT_EQ(13, ftell(fp));
  ASSERT_EQ(0, fseek(fp, 0, SEEK_END));
  EXPECT_EQ(13, ftell(fp));
  ASSERT_EQ(0, fclose(fp));
  AssertFileIs(tf.filename, "0123456789xxx");
}

TEST(STDIO_TEST, constants) {
  ASSERT_LE(FILENAME_MAX, PATH_MAX);
  ASSERT_EQ(L_tmpnam, PATH_MAX);
}
