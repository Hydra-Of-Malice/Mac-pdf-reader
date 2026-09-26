/**
 * macOS build of SumatraPDF.app (cmd/build.ts -mac), plus the source lists, library definitions and compile / link
 * helpers shared with the POSIX core check (cmd/helper/mac-core-build.ts, cmd/build.ts -mac-core).
 *
 * Output: out/mac-<cfg>-<arch>/ with SumatraPDF.app, test_util, test_engines, test_mac_engine,
 * test_mac_thumbnails, lib/*.a. <cfg> is dbg, rel, asan or rel_asan; <arch> is arm64, x64 or universal.
 *
 * Reproducible: fixed flags, source paths mapped to "." (-ffile-prefix-map), sorted sources, ZERO_AR_DATE=1 and
 * SOURCE_DATE_EPOCH (from the HEAD commit) so no build time is baked in.
 */

import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { cpus } from "node:os";
import { join } from "node:path";
import { Glob } from "bun";
import {
  type BuildTools,
  type LibDef,
  buildLibrary,
  compileAll,
  dropX86OnlyCflags,
  invalidateObjsIfBuildChanged,
  objPath,
  spawnCmd,
} from "../deps-build-common";
import {
  aGumbo,
  brotli,
  cmarkGfm,
  extract,
  freetype,
  harfbuzz,
  heicdec,
  jbig2dec,
  jxldec,
  lcms2,
  libjpegTurbo,
  libwebp,
  mujs,
  mupdf as mupdfBase,
  openjpeg,
  unrar,
  zlib,
} from "../deps-build-defs";
import { extractSumatraVersion } from "../util";
import { addBundleResources, packageMacApp } from "./mac-bundle";

export type MacArch = "arm64" | "x64";
export type PosixOs = "mac" | "linux";

// first macOS that runs on Apple Silicon; matches LSMinimumSystemVersion in src/mac/Resources/Info.plist
export const kMacMinVersion = "11.0";

// a compile target: the OS / CPU the code is built for, and the flags that select it
export interface PosixTarget {
  os: PosixOs;
  arch: MacArch;
  // go on every compile and link line: -arch, -mmacosx-version-min, zig's -target
  flags: string[];
  // Apple's SDK with its frameworks is available (not when cross-compiling with zig)
  frameworks: boolean;
}

export interface PosixConfig {
  isRelease: boolean;
  asan: boolean;
}

export function configDirName(cfg: PosixConfig): string {
  if (cfg.asan) return cfg.isRelease ? "rel_asan" : "asan";
  return cfg.isRelease ? "rel" : "dbg";
}

export function hostArch(): MacArch {
  if (process.arch === "arm64") return "arm64";
  return "x64";
}

export function macTarget(arch: MacArch): PosixTarget {
  return {
    os: "mac",
    arch,
    flags: ["-arch", arch === "arm64" ? "arm64" : "x86_64", `-mmacosx-version-min=${kMacMinVersion}`],
    frameworks: true,
  };
}

// C sources that call Apple frameworks (CoreText, CoreFoundation) from base and mupdf: only for a macOS SDK build
export const MAC_FRAMEWORK_SOURCES = ["src/base/StrNormalize_mac.c", "src/mupdf/mupdf_load_system_font_mac.c"];

export function defaultJobs(): number {
  return Math.max(1, cpus().length);
}

function requireDarwin(): void {
  if (process.platform !== "darwin") {
    throw new Error(
      `-mac builds SumatraPDF.app and needs macOS with Xcode (this is ${process.platform}); use -mac-core`,
    );
  }
}

function resolveTool(role: string, candidates: string[]): string {
  for (const name of candidates) {
    if (Bun.which(name)) return name;
  }
  throw new Error(`could not find ${role} (tried: ${candidates.join(", ")}); install Xcode: xcode-select --install`);
}

function resolveMacTools(): BuildTools {
  return {
    cc: resolveTool("C compiler", ["clang"]),
    cxx: resolveTool("C++ compiler", ["clang++"]),
    ar: resolveTool("archiver", ["ar"]),
  };
}

//--- reproducibility -----------------------------------------------------------

// Environment for reproducible output: ar / ld64 write no timestamps (ZERO_AR_DATE), and __DATE__ / __TIME__ come
// from the HEAD commit instead of the clock (SOURCE_DATE_EPOCH, honored by gcc and clang).
export function setReproducibleEnv(): void {
  process.env.ZERO_AR_DATE = "1";
  if (process.env.SOURCE_DATE_EPOCH) return;
  const p = Bun.spawnSync(["git", "log", "-1", "--format=%ct"], { stdout: "pipe", stderr: "ignore" });
  const t = p.exitCode === 0 ? p.stdout.toString().trim() : "";
  process.env.SOURCE_DATE_EPOCH = /^\d+$/.test(t) ? t : "0";
}

// flags on every compile: debug info, source paths relative to the repo root, ASan
export function commonCompileFlags(t: PosixTarget, cfg: PosixConfig): string[] {
  const root = process.cwd();
  return [
    ...t.flags,
    "-g",
    `-ffile-prefix-map=${root}=.`,
    `-fdebug-prefix-map=${root}=.`,
    ...(cfg.asan ? ["-fsanitize=address", "-fno-omit-frame-pointer"] : []),
  ];
}

//--- third-party libraries --------------------------------------------------------------------------------------

// Writes a generated source only when it changed, so its mtime doesn't force a rebuild of what includes it
function writeGenerated(path: string, data: string, encoding: BufferEncoding = "utf8"): void {
  if (existsSync(path) && readFileSync(path, encoding) === data) return;
  writeFileSync(path, data, encoding);
}

// ext/a-libarchive was generated for Windows only: the build supplies a POSIX config header and the few functions
// whose POSIX sources the amalgamation left out.
const libarchiveConfigPosix = `/* generated by cmd/helper/mac-build.ts: POSIX config for ext/a-libarchive (read-only subset) */
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1

#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_FCNTL 1
#define HAVE_ICONV 1
#define HAVE_ICONV_H 1
#define HAVE_LIMITS_H 1
#define HAVE_FCHDIR 1
#define HAVE_DIRFD 1
#define HAVE_READLINK 1
#define HAVE_LSTAT 1
#define HAVE_LOCALE_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define HAVE_DIRENT_H 1
#define HAVE_DLFCN_H 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1
#define HAVE_POLL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_FSTAT 1
#define HAVE_STAT 1
#define HAVE_MEMSET 1
#define HAVE_MEMMOVE 1
#define HAVE_SETLOCALE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRFTIME 1
#define HAVE_STRNLEN 1
#define HAVE_STRRCHR 1
#define HAVE_VPRINTF 1
#define HAVE_MBRTOWC 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCMP 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_INTTYPES_H 1
#define HAVE_INTMAX_T 1
#define HAVE_UINTMAX_T 1
#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define HAVE_WCHAR_T 1
#define HAVE_SSIZE_T 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define SIZEOF_WCHAR_T 4
#define HAVE_EILSEQ 1
#define ICONV_CONST

#if defined(__APPLE__)
#define HAVE_ARC4RANDOM_BUF 1
#define HAVE_STRUCT_STAT_ST_MTIMESPEC 1
#define HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC 1
/* CommonCrypto, part of libSystem */
#define ARCHIVE_CRYPTO_MD5_LIBSYSTEM 1
#define ARCHIVE_CRYPTO_SHA1_LIBSYSTEM 1
#define ARCHIVE_CRYPTO_SHA256_LIBSYSTEM 1
#define ARCHIVE_CRYPTO_SHA384_LIBSYSTEM 1
#define ARCHIVE_CRYPTO_SHA512_LIBSYSTEM 1
#else
#define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1
#endif

/* bundled codecs */
#define HAVE_LIBZ 1
#define HAVE_ZLIB_H 1
#define HAVE_BZLIB_H 1
#define HAVE_LZMA_H 1
#define HAVE_LIBLZMA 1

/* archive_platform_stat.h's POSIX branch, dropped by the Windows-only amalgamation */
#include <sys/types.h>
#include <sys/stat.h>
typedef off_t la_seek_t;
typedef struct stat la_seek_stat_t;
#define la_seek_fstat(fd, st) fstat((fd), (st))
#define la_seek_stat(fd, st) stat((fd), (st))
`;

const libarchiveStubsPosix = `/* generated by cmd/helper/mac-build.ts: libarchive functions whose POSIX sources ext/a-libarchive leaves out.
   SumatraPDF only reads archives from files and memory: no external filter programs, no disk reading. */
#include <stdint.h>
#include <sys/types.h>

struct archive;
#define ARCHIVE_FAILED (-25)
#define ARCHIVE_FATAL (-30)

int __archive_create_child(const char* cmd, int* child_stdin, int* child_stdout, pid_t* out_child) {
    (void)cmd;
    (void)child_stdin;
    (void)child_stdout;
    (void)out_child;
    return ARCHIVE_FAILED;
}

void __archive_check_child(int in, int out) {
    (void)in;
    (void)out;
}

int archive_read_disk_set_gname_lookup(struct archive* a, void* data, const char* (*lookup)(void*, int64_t),
                                       void (*cleanup)(void*)) {
    (void)a;
    (void)data;
    (void)lookup;
    (void)cleanup;
    return ARCHIVE_FATAL;
}

int archive_read_disk_set_uname_lookup(struct archive* a, void* data, const char* (*lookup)(void*, int64_t),
                                       void (*cleanup)(void*)) {
    (void)a;
    (void)data;
    (void)lookup;
    (void)cleanup;
    return ARCHIVE_FATAL;
}
`;

// libarchive amalgamation + bzip2 + liblzma decoder, like cmd/deps-build-defs.ts's libarchive for Windows
function makeLibarchive(t: PosixTarget, genDir: string): LibDef {
  const dir = join(genDir, "libarchive");
  mkdirSync(dir, { recursive: true });
  writeGenerated(join(dir, "config_posix.h"), libarchiveConfigPosix);
  writeGenerated(join(dir, "libarchive_posix_stubs.c"), libarchiveStubsPosix);
  const lzmaDir = join(genDir, "liblzma");
  mkdirSync(lzmaDir, { recursive: true });
  const lzmaConfig = t.os === "mac" ? "config_macos.h" : "config_linux.h";
  writeGenerated(join(lzmaDir, "config.h"), readFileSync(join("ext", "liblzma", lzmaConfig), "utf8"));
  return {
    name: "a-libarchive",
    alwaysOptimize: true,
    defines: [
      "LIBARCHIVE_STATIC",
      'PLATFORM_CONFIG_H="config_posix.h"',
      "BZ_NO_STDIO",
      "HAVE_CONFIG_H",
      "LZMA_API_STATIC",
    ],
    includes: [
      dir,
      lzmaDir, // config.h for liblzma, shadows ext/liblzma/config.h
      "ext/a-libarchive",
      "ext/a-libarchive/libarchive",
      "ext/a-zlib",
      "ext/a-bzip2",
      "ext/liblzma/api",
      "ext/liblzma/common",
      "ext/liblzma/check",
      "ext/liblzma/delta",
      "ext/liblzma/lz",
      "ext/liblzma/lzma",
      "ext/liblzma/rangecoder",
      "ext/liblzma/simple",
      "ext/liblzma",
    ],
    files: [
      { dir: "ext/a-libarchive", patterns: ["libarchive.c"] },
      { dir, patterns: ["libarchive_posix_stubs.c"] },
      { dir: "ext/a-bzip2", patterns: ["bzip2.c"] },
      {
        dir: "ext/liblzma",
        patterns: [
          "common/alone_decoder.c",
          "common/auto_decoder.c",
          "common/block_decoder.c",
          "common/block_header_decoder.c",
          "common/block_util.c",
          "common/common.c",
          "common/filter_common.c",
          "common/filter_decoder.c",
          "common/filter_flags_decoder.c",
          "common/index.c",
          "common/index_decoder.c",
          "common/index_hash.c",
          "common/stream_decoder.c",
          "common/stream_flags_common.c",
          "common/stream_flags_decoder.c",
          "common/vli_decoder.c",
          "common/vli_size.c",
          "check/check.c",
          "check/crc32_fast.c",
          "check/crc64_fast.c",
          "lz/lz_decoder.c",
          "lzma/lzma_decoder.c",
          "lzma/lzma2_decoder.c",
          "rangecoder/price_table.c",
          "delta/delta_common.c",
          "delta/delta_decoder.c",
          "simple/simple_coder.c",
          "simple/simple_decoder.c",
          "simple/x86.c",
        ],
      },
    ],
  };
}

// dav1d without asm (HAVE_ASM=0) and without win32/thread.c; config.h is generated per arch
function makeDav1d(t: PosixTarget, genDir: string): LibDef {
  const isArm = t.arch === "arm64";
  const dir = join(genDir, "dav1d");
  mkdirSync(dir, { recursive: true });
  const config = [
    "/* generated by cmd/helper/mac-build.ts */",
    "#pragma once",
    `#define ARCH_AARCH64 ${isArm ? 1 : 0}`,
    "#define ARCH_ARM 0",
    `#define ARCH_X86 ${isArm ? 0 : 1}`,
    "#define ARCH_X86_32 0",
    `#define ARCH_X86_64 ${isArm ? 0 : 1}`,
    "#define CONFIG_16BPC 1",
    "#define CONFIG_8BPC 1",
    "#define CONFIG_LOG 1",
    "#define ENDIANNESS_BIG 0",
    "#define HAVE_ASM 0",
    "#define STACK_ALIGNMENT 16",
    `#define PREFIX ${isArm && t.os === "mac" ? 1 : 0}`,
    "",
  ].join("\n");
  writeGenerated(join(dir, "config.h"), config);
  const files: LibDef["files"] = [
    {
      dir: "ext/dav1d/src",
      patterns: [
        "lib.c",
        "thread_task.c",
        "cdf.c",
        "cpu.c",
        "ctx.c",
        "data.c",
        "decode.c",
        "dequant_tables.c",
        "getbits.c",
        "intra_edge.c",
        "itx_1d.c",
        "lf_mask.c",
        "log.c",
        "mem.c",
        "msac.c",
        "obu.c",
        "pal.c",
        "picture.c",
        "qm.c",
        "ref.c",
        "refmvs.c",
        "scan.c",
        "tables.c",
        "warpmv.c",
        "wedge.c",
        "sumatra_bitdepth_8.c",
        "sumatra_bitdepth_8_2.c",
        "sumatra_bitdepth_16.c",
        "sumatra_bitdepth_16_2.c",
      ],
    },
  ];
  if (!isArm) {
    files.push({ dir: "ext/dav1d/src/x86", patterns: ["cpu.c"] });
  }
  return { name: "dav1d", alwaysOptimize: true, defines: [], includes: [dir, "ext/dav1d", "ext/dav1d/include"], files };
}

const chmdec: LibDef = {
  name: "chmdec",
  alwaysOptimize: true,
  defines: ["_stricmp=strcasecmp", "_strnicmp=strncasecmp"],
  includes: [],
  extraCflags: ["-include", "limits.h"],
  files: [{ dir: "ext/chmdec", patterns: ["chm.c"] }],
};

const msdes: LibDef = {
  name: "msdes",
  alwaysOptimize: true,
  defines: [],
  includes: ["ext/msdes"],
  files: [{ dir: "ext/msdes", patterns: ["des.c"] }],
};

const djvudec: LibDef = {
  name: "djvudec",
  alwaysOptimize: true,
  defines: [],
  includes: [],
  files: [{ dir: "ext/djvudec", patterns: ["djvu.c"] }],
};

// mupdf as in premake5.lua, minus the Windows-only pieces: WIC JPEG-XR, the CryptoAPI signature code
// (pkcs7-windows.c) and the command-line tools that call it (only pdfinfo.c, for EngineMupdfGetPdfInfo())
function makeMupdf(t: PosixTarget): LibDef {
  const lib = structuredClone(mupdfBase);
  lib.defines = lib.defines.filter((d) => !d.startsWith("_CRT"));
  lib.defines.push("FZ_ENABLE_MD=1", "HAVE_LIBARCHIVE", "LIBARCHIVE_STATIC", "HAVE_PTHREAD");
  if (t.arch === "arm64") {
    lib.defines.push("ARCH_HAS_NEON=1");
    lib.extraCflags = [];
  } else {
    lib.extraCflags = ["-msse4.1", "-DARCH_HAS_SSE=1"];
  }
  lib.includes.push("ext/a-libarchive/libarchive");
  for (const g of lib.files) {
    const pats = g.patterns;
    if (g.dir === "src/mupdf") g.patterns = pats.filter((p) => p !== "pkcs7-windows.c");
    if (g.dir === "ext/mupdf/source/fitz") g.patterns = pats.map((p) => (p === "load-jxr-win.c" ? "load-jxr.c" : p));
  }
  for (const g of lib.files) {
    if (g.dir === "ext/mupdf/source/tools") g.patterns = ["pdfinfo.c"];
    // system fonts through CoreText
    if (g.dir === "src/mupdf" && t.frameworks) g.patterns.push("mupdf_load_system_font_mac.c");
  }
  lib.files.push({ dir: "ext/mupdf/source/helpers/pkcs7", patterns: ["pkcs7-openssl.c"] });
  return lib;
}

// ext/a-unrar/unrar.cpp is generated (cmd/amalgam.ts) from upstream's Windows build, which includes isnt.cpp and
// motw.cpp: Windows-only files the Unix makefile leaves out. Build from a copy without them; the rest of the
// amalgamation already has upstream's _UNIX / _APPLE code paths.
const unrarWindowsOnly: [string, string, string][] = [
  ["isnt.cpp", "\nDWORD WinNT()\n{", "\n#if defined(_WIN_ALL) && !defined(SFX_MODULE) && !defined(RARDLL)"],
  ["motw.cpp", "\nMarkOfTheWeb::MarkOfTheWeb()\n{", "\nRAROptions::RAROptions()\n{"],
];

function posixUnrar(genDir: string): LibDef {
  let src = readFileSync(join("ext", "a-unrar", "unrar.cpp"), "latin1");
  for (const [file, startMarker, endMarker] of unrarWindowsOnly) {
    const start = src.indexOf(startMarker);
    const end = start < 0 ? -1 : src.indexOf(endMarker, start);
    if (end < 0) throw new Error(`ext/a-unrar/unrar.cpp: can't find ${file}; update unrarWindowsOnly in mac-build.ts`);
    src = `${src.slice(0, start)}\n// ${file}: Windows only, left out by cmd/helper/mac-build.ts\n${src.slice(end)}`;
  }
  const dir = join(genDir, "unrar");
  mkdirSync(dir, { recursive: true });
  writeGenerated(join(dir, "unrar_posix.cpp"), src, "latin1");
  const lib = structuredClone(unrar);
  lib.defines = lib.defines.filter((d) => d !== "_CRT_SECURE_NO_WARNINGS");
  lib.files = [{ dir, patterns: ["unrar_posix.cpp"] }];
  return lib;
}

// ext/a-harfbuzz only works the way MSVC builds it: gcc / clang otherwise skip the tables that the amalgamation
// put inside an #ifdef HB_NO_VISIBILITY block (MSVC always defines it)
function posixHarfbuzz(): LibDef {
  const lib = structuredClone(harfbuzz);
  lib.defines.push("HB_NO_VISIBILITY");
  return lib;
}

// src/base: the portable sources plus the POSIX (*_posix.cpp) versions of the OS-specific ones
export const BASE_SOURCES = [
  "src/base/Archive.cpp",
  "src/base/Arena_posix.cpp",
  "src/base/Base.cpp",
  "src/base/Base_posix.cpp",
  "src/base/ByteReaderWriter.cpp",
  "src/base/Crypto_posix.cpp",
  "src/base/CssParser.cpp",
  "src/base/DbgHelpDyn_posix.cpp",
  "src/base/Dict.cpp",
  "src/base/DirScan.cpp",
  "src/base/DirScan_posix.cpp",
  "src/base/Exif.cpp",
  "src/base/File.cpp",
  "src/base/File_posix.cpp",
  "src/base/GuessFileType.cpp",
  "src/base/HtmlTags.cpp",
  "src/base/JsonParser.cpp",
  "src/base/Pixmap.cpp",
  "src/base/SettingsUtil.cpp",
  "src/base/SquareTreeParser.cpp",
  "src/base/StrQueue.cpp",
  "src/base/TgaReader.cpp",
  "src/base/UITask_posix.cpp",
  "src/base/WinDynCalls_posix.cpp",
  "src/base/Zip.cpp",
  "src/gui/Dpi_posix.cpp",
];

// every include dir src/ code needs (premake5.lua's test_engines plus zlib)
export const SRC_INCLUDES = [
  "src",
  "ext/mupdf/include",
  "ext/mupdf/generated",
  "ext/djvudec",
  "ext/msdes",
  "ext/chmdec",
  "ext/a-libarchive",
  "ext/a-unrar",
  "ext/heicdec",
  "ext/a-libwebp",
  "ext/jxldec",
  "ext/a-zlib",
];

function baseLib(t: PosixTarget): LibDef {
  const groups = new Map<string, string[]>();
  const srcs = t.frameworks ? [...BASE_SOURCES, "src/base/StrNormalize_mac.c"] : BASE_SOURCES;
  for (const src of srcs) {
    const i = src.lastIndexOf("/");
    const dir = src.slice(0, i);
    groups.set(dir, [...(groups.get(dir) ?? []), src.slice(i + 1)]);
  }
  return {
    name: "base",
    alwaysOptimize: false,
    defines: ["LIBARCHIVE_STATIC"],
    includes: SRC_INCLUDES,
    files: [...groups].map(([dir, patterns]) => ({ dir, patterns })),
  };
}

// The libraries in link order; names are also the archive names (lib<name>.a).
export function posixLibs(t: PosixTarget, genDir: string): LibDef[] {
  const libs: LibDef[] = [
    baseLib(t),
    makeMupdf(t),
    structuredClone(libwebp),
    structuredClone(aGumbo),
    structuredClone(cmarkGfm),
    structuredClone(mujs),
    structuredClone(extract),
    posixHarfbuzz(),
    structuredClone(freetype),
    structuredClone(brotli),
    structuredClone(lcms2),
    structuredClone(openjpeg),
    structuredClone(jbig2dec),
    structuredClone(libjpegTurbo),
    structuredClone(heicdec),
    makeDav1d(t, genDir),
    structuredClone(jxldec),
    djvudec,
    chmdec,
    msdes,
    posixUnrar(genDir),
    makeLibarchive(t, genDir),
    structuredClone(zlib),
  ];
  for (const lib of libs) {
    lib.defines = lib.defines.filter((d) => d !== "_CRT_SECURE_NO_WARNINGS");
    // deps-build-defs.ts's brotli asks for file groups that no longer exist
    lib.files = lib.files.filter((g) => g);
    dropX86OnlyCflags(lib, t.arch);
  }
  return libs;
}

export function libArchivePaths(outDir: string, libs: LibDef[]): string[] {
  return libs.map((l) => join(outDir, "lib", `lib${l.name}.a`));
}

//--- our sources -------------------------------------------------------------------------------------------------

// the document engines and what they need (premake5.files.lua test_engines_files(), with POSIX variants)
export const ENGINE_SOURCES = [
  "src/AvifReader.cpp",
  "src/CachedObjects.cpp",
  "src/ChapterTable.cpp",
  "src/ChmFile.cpp",
  "src/DocProperties.cpp",
  "src/EbookDoc.cpp",
  "src/EmbeddedResources.cpp",
  "src/EmbeddedResources_posix.cpp",
  "src/EngineBase.cpp",
  "src/EngineDjvuDec.cpp",
  "src/EngineImages.cpp",
  "src/EngineMupdf.cpp",
  "src/GumboHtmlParser.cpp",
  "src/ImageReader.cpp",
  "src/ImageReader_posix.cpp",
  "src/JxlReader.cpp",
  "src/LitDoc.cpp",
  "src/MobiDoc.cpp",
  "src/PalmDbReader.cpp",
  "src/PdfCad.cpp",
  "src/PdfDarkModeNoOp.cpp",
  "src/TextSearch.cpp",
  "src/TextSelection.cpp",
  "src/WebpReader.cpp",
  "src/gui/UIModels.cpp",
];

// the portable reader model the Cocoa app drives through src/mac/SumatraMacEngine.h
export const READER_SOURCES = [
  "src/DisplayMode.cpp",
  "src/DocumentLayout.cpp",
  "src/PageRenderPolicy.cpp",
  "src/PageRenderService.cpp",
  "src/ReaderModel.cpp",
  "src/gui/PasswordDialog.cpp",
];

// Sorted files in dir matching ext. src/mac/*.cpp and src/gui/mac/*.cpp are plain C++ (Cocoa code lives in .mm
// files), so new ones are picked up without editing this file.
export function sourcesIn(dir: string, ext: string): string[] {
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.endsWith(ext))
    .sort()
    .map((f) => `${dir}/${f}`);
}

// src/mac/*.cpp: SumatraMacEngine (the plain-C bridge), MacPrefs, MacThumbnails, ...
export function macEngineSources(): string[] {
  return sourcesIn("src/mac", ".cpp");
}

// src/gui/mac/*.cpp: portable C++ that calls into GuiMacBridge.mm, so it links only into the app
export function guiMacSources(): string[] {
  return sourcesIn("src/gui/mac", ".cpp");
}

// Objective-C++ (AppKit): only the macOS app
export function cocoaSources(): string[] {
  return [...sourcesIn("src/gui/mac", ".mm"), ...sourcesIn("src/mac", ".mm")];
}

// Unit tests that run on POSIX (the Windows app runs the full set from src/tests/Sumatra_ut.cpp) and the modules
// they test. Not here: ClipboardImage_ut / Win_ut (Win32), File_ut (Windows path semantics), PdfSync_ut (SyncTeX),
// RefHover_ut, CommandPalette_ut (Commands.cpp needs the Windows settings), PdfDarkModeImageClassifier_ut (the
// engines link PdfDarkModeNoOp.cpp), the gui/ tests and the ones for Win32 UI modules (AnnotSearch, PagePosition,
// ReadAloud, ShortcutParse).
export const TEST_UTIL_SOURCES = [
  "src/base/tests/Base_ut.cpp",
  "src/base/tests/ByteReaderWriter_ut.cpp",
  "src/base/tests/Crypto_ut.cpp",
  "src/base/tests/CssParser_ut.cpp",
  "src/base/tests/Dict_ut.cpp",
  "src/base/tests/GuessFileType_ut.cpp",
  "src/base/tests/JsonParser_ut.cpp",
  "src/base/tests/SettingsUtil_ut.cpp",
  "src/base/tests/SquareTreeParser_ut.cpp",
  "src/base/tests/StrFormat_ut.cpp",
  "src/base/tests/StrVec_ut.cpp",
  "src/base/tests/Str_ut.cpp",
  "src/base/tests/UtAssert.cpp",
  "src/base/tests/Vec_ut.cpp",
  "src/tests/CachedObjects_ut.cpp",
  "src/tests/ChapterTable_ut.cpp",
  "src/tests/EngineDjvuDec_ut.cpp",
  "src/tests/LitDoc_ut.cpp",
  "src/tests/MobiDoc_ut.cpp",
  "src/tests/PageRenderPolicy_ut.cpp",
  "src/tests/PdfDarkModeOklab_ut.cpp",
  "src/tests/SimpleLog_ut.cpp",
  "src/tests/TextSelection_ut.cpp",
  "src/CrashHandlerNoOp.cpp",
  "src/PdfDarkModeOklab.cpp",
  "src/SumatraLog_posix.cpp",
  "src/tools/test_util.cpp",
];

// executables every POSIX build links: name -> its own sources (on top of the libraries)
export function testExeSources(): Record<string, string[]> {
  return {
    test_util: [...ENGINE_SOURCES, "src/PageRenderPolicy.cpp", ...TEST_UTIL_SOURCES],
    test_engines: [...ENGINE_SOURCES, "src/tools/test_engines.cpp"],
    // headless_gui_posix.cpp stands in for the Cocoa GUI (src/gui/mac) the app links
    test_mac_engine: [
      ...ENGINE_SOURCES,
      ...READER_SOURCES,
      ...macEngineSources(),
      "src/tools/headless_gui_posix.cpp",
      "src/tools/test_mac_engine.cpp",
    ],
    // defines its own document type, so not SumatraMacEngine.cpp
    test_mac_thumbnails: [...ENGINE_SOURCES, "src/mac/MacThumbnails.cpp", "src/tools/test_mac_thumbnails.cpp"],
  };
}

//--- compile / link ----------------------------------------------------------------------------------------------

export interface CompileArgs {
  t: PosixTarget;
  cfg: PosixConfig;
  tools: BuildTools;
  outDir: string;
  jobs: number;
}

// Compiles srcs (C++; clang takes .mm as Objective-C++, manual retain/release) into outDir/obj/<group>/.
// Returns the object paths.
export async function compileSources(a: CompileArgs, group: string, srcs: string[]): Promise<string[]> {
  const opt = a.cfg.isRelease ? ["-O2", "-DNDEBUG"] : ["-O0", "-DDEBUG"];
  const common = commonCompileFlags(a.t, a.cfg);
  const includes = SRC_INCLUDES.map((d) => `-I${d}`);
  const units = srcs.map((src) => {
    const obj = objPath(a.outDir, group, src);
    const args = [a.tools.cxx, ...opt, ...common, ...includes, "-DLIBARCHIVE_STATIC", "-w", "-std=c++23"];
    args.push("-fno-rtti", "-fno-exceptions", "-c", src, "-o", obj);
    return { src, obj, args };
  });
  await compileAll(units, a.jobs);
  return units.map((u) => u.obj);
}

export function linkFlags(t: PosixTarget, cfg: PosixConfig): string[] {
  const flags = [...t.flags, ...(cfg.asan ? ["-fsanitize=address"] : [])];
  if (t.os === "mac") return [...flags, "-liconv", "-framework", "CoreText", "-framework", "CoreFoundation"];
  return [...flags, "-lpthread", "-lm"];
}

// Links objs and the static libraries into exePath. GNU ld / lld need the group for the libraries' mutual
// references; ld64 resolves them in any order.
export async function linkExe(a: CompileArgs, exePath: string, objs: string[], libs: string[], extra: string[] = []) {
  const libArgs = a.t.os === "mac" ? libs : ["-Wl,--start-group", ...libs, "-Wl,--end-group"];
  const args = [a.tools.cxx, "-o", exePath, ...objs, ...libArgs, ...linkFlags(a.t, a.cfg), ...extra];
  const res = await spawnCmd(args);
  if (!res.ok) {
    throw new Error(`link ${exePath} failed:\n${res.stderr.slice(0, 4000)}`);
  }
  console.log(`  -> ${exePath}`);
}

// Builds the libraries; returns their archive paths in link order.
export async function buildPosixLibs(a: CompileArgs): Promise<string[]> {
  const libs = posixLibs(a.t, join(a.outDir, "generated"));
  const commonFlags = commonCompileFlags(a.t, a.cfg);
  const cxxFlags = ["-D__GXX_TYPEINFO_EQUALITY_INLINE=1"];
  for (const lib of libs) {
    await buildLibrary(lib, a.outDir, a.cfg.isRelease, {
      tools: a.tools,
      commonDefines: [],
      commonFlags,
      cxxFlags,
      jobs: a.jobs,
    });
  }
  return libArchivePaths(a.outDir, libs);
}

// Compiles and links the test executables; returns their paths by name.
export async function buildTestExes(a: CompileArgs, libs: string[]): Promise<Record<string, string>> {
  const res: Record<string, string> = {};
  for (const [name, srcs] of Object.entries(testExeSources())) {
    console.log(`Building ${name}...`);
    const objs = await compileSources(a, "app", srcs);
    const exe = join(a.outDir, name);
    await linkExe(a, exe, objs, libs);
    res[name] = exe;
  }
  return res;
}

// mupdf's built-in fonts, the same set cmd/pack-embedded-prebuild.cmd packs into the Windows exe; POSIX builds read
// them from <dir>/fonts (see src/EmbeddedResources_posix.cpp)
const mupdfFonts: [string, string[]][] = [
  ["urw", ["Dingbats.cff", "NimbusMonoPS-*.cff", "NimbusRoman-*.cff", "NimbusSans-*.cff", "StandardSymbolsPS.cff"]],
  ["droid", ["DroidSansFallbackFull.ttf"]],
  ["sil", ["CharisSIL*.cff"]],
  [
    "noto",
    [
      "NotoSans-Regular.otf",
      "NotoSerif-Regular.otf",
      "NotoSansMath-Regular.otf",
      "NotoMusic-Regular.otf",
      "NotoSansSymbols-Regular.otf",
      "NotoSansSymbols2-Regular.otf",
      "NotoEmoji-Regular.ttf",
    ],
  ],
];

export function stageMupdfFonts(dir: string): void {
  const dst = join(dir, "fonts");
  rmSync(dst, { recursive: true, force: true });
  mkdirSync(dst, { recursive: true });
  let n = 0;
  for (const [forge, patterns] of mupdfFonts) {
    const src = join("ext", "mupdf", "resources", "fonts", forge);
    for (const pat of patterns) {
      const names = [...new Glob(pat).scanSync(src)].sort();
      if (names.length === 0) throw new Error(`no font matches ${src}/${pat}`);
      for (const name of names) {
        cpSync(join(src, name), join(dst, name));
        n++;
      }
    }
  }
  console.log(`  -> ${dst} (${n} fonts)`);
}

export async function runTestUtil(exePath: string): Promise<void> {
  const env = { ...process.env, ASAN_OPTIONS: "abort_on_error=1:halt_on_error=1:detect_leaks=0" };
  const proc = Bun.spawn([exePath, "-for-ai"], { env, stdout: "inherit", stderr: "inherit" });
  const code = await proc.exited;
  if (code !== 0) throw new Error(`${exePath} failed with exit code ${code}`);
}

//--- the macOS app -----------------------------------------------------------------------------------------------

async function generateDsym(exePath: string, outputPath = `${exePath}.dSYM`): Promise<void> {
  const dsymutil = resolveTool("debug symbol generator", ["dsymutil"]);
  rmSync(outputPath, { recursive: true, force: true });
  const result = await spawnCmd([dsymutil, exePath, "-o", outputPath]);
  if (!result.ok) throw new Error(`dsymutil failed for ${exePath}: ${result.stderr}`);
  console.log(`  -> ${outputPath}`);
}

// arm64 code must be signed to run; ad-hoc unless SUMATRA_MAC_SIGN_IDENTITY names a Developer ID
async function signApp(appDir: string): Promise<void> {
  const identity = process.env.SUMATRA_MAC_SIGN_IDENTITY;
  const args = identity
    ? ["codesign", "--force", "--deep", "--options", "runtime", "--timestamp", "-s", identity, appDir]
    : ["codesign", "--force", "--deep", "-s", "-", appDir];
  const res = await spawnCmd(args);
  if (!res.ok) throw new Error(`codesign failed: ${res.stderr}`);
  console.log(`  signed ${appDir} (${identity ? identity : "ad-hoc"})`);
}

// SumatraPDF.app/Contents: the executable is in place; add Info.plist, icon, licenses (mac-bundle.ts) and mupdf's
// fonts, then sign. Everything that goes into the bundle belongs here, before signing.
async function assembleAppBundle(appDir: string, linkInputs: string[]): Promise<void> {
  addBundleResources(appDir, linkInputs);
  stageMupdfFonts(join(appDir, "Contents", "Resources"));
  await signApp(appDir);
}

async function buildMacApp(a: CompileArgs, libs: string[]): Promise<string> {
  console.log("Building SumatraPDF.app...");
  const srcs = [...ENGINE_SOURCES, ...READER_SOURCES, ...macEngineSources(), ...guiMacSources(), ...cocoaSources()];
  const objs = await compileSources(a, "app", srcs);
  const appDir = join(a.outDir, "SumatraPDF.app");
  rmSync(appDir, { recursive: true, force: true });
  const macosDir = join(appDir, "Contents", "MacOS");
  mkdirSync(macosDir, { recursive: true });
  const exe = join(macosDir, "SumatraPDF");
  await linkExe(a, exe, objs, libs, ["-framework", "Cocoa"]);
  return appDir;
}

export interface MacBuildOptions {
  isRelease: boolean;
  asan: boolean;
  clean: boolean;
  arch: MacArch | "universal";
  jobs?: number;
  dmg?: boolean; // also package a .dmg; default: plain release builds only
}

export function macOutDir(cfg: PosixConfig, arch: MacArch | "universal"): string {
  return join("out", `mac-${configDirName(cfg)}-${arch}`);
}

// one architecture: libraries, test tools (test_util runs when arch is the host's), SumatraPDF.app
async function buildMacArch(opts: MacBuildOptions, arch: MacArch, tools: BuildTools): Promise<string> {
  const cfg = { isRelease: opts.isRelease, asan: opts.asan };
  const outDir = macOutDir(cfg, arch);
  if (opts.clean) rmSync(outDir, { recursive: true, force: true });
  const t = macTarget(arch);
  const a: CompileArgs = { t, cfg, tools, outDir, jobs: opts.jobs ?? defaultJobs() };
  invalidateObjsIfBuildChanged(outDir, tools, configDirName(cfg), commonCompileFlags(t, cfg));
  console.log(`\n=== macOS ${arch} ${configDirName(cfg)} -> ${outDir} ===\n`);

  const libs = await buildPosixLibs(a);
  const exes = await buildTestExes(a, libs);
  stageMupdfFonts(outDir);
  if (arch === hostArch()) {
    await runTestUtil(exes.test_util);
  } else {
    console.log(`  (not running ${arch} test_util on a ${hostArch()} Mac)`);
  }
  const appDir = await buildMacApp(a, libs);
  await generateDsym(join(appDir, "Contents", "MacOS", "SumatraPDF"), `${appDir}.dSYM`);
  await assembleAppBundle(appDir, libs);
  return appDir;
}

// arm64 + x64 apps merged with lipo
async function makeUniversalApp(cfg: PosixConfig, armApp: string, x64App: string, libs: string[]): Promise<string> {
  const outDir = macOutDir(cfg, "universal");
  const appDir = join(outDir, "SumatraPDF.app");
  rmSync(appDir, { recursive: true, force: true });
  mkdirSync(outDir, { recursive: true });
  cpSync(armApp, appDir, { recursive: true });
  const rel = join("Contents", "MacOS", "SumatraPDF");
  const res = await spawnCmd(["lipo", "-create", join(armApp, rel), join(x64App, rel), "-output", join(appDir, rel)]);
  if (!res.ok) throw new Error(`lipo failed: ${res.stderr}`);
  await generateDsym(join(appDir, rel), `${appDir}.dSYM`);
  await assembleAppBundle(appDir, libs);
  return appDir;
}

export async function buildMac(opts: MacBuildOptions): Promise<void> {
  requireDarwin();
  setReproducibleEnv();
  const tools = resolveMacTools();
  const cfg = { isRelease: opts.isRelease, asan: opts.asan };
  const startTime = performance.now();

  let appDir: string;
  if (opts.arch === "universal") {
    const armApp = await buildMacArch(opts, "arm64", tools);
    const x64App = await buildMacArch(opts, "x64", tools);
    const libs = posixLibs(macTarget("arm64"), join(macOutDir(cfg, "arm64"), "generated"));
    appDir = await makeUniversalApp(cfg, armApp, x64App, libArchivePaths(macOutDir(cfg, "arm64"), libs));
  } else {
    appDir = await buildMacArch(opts, opts.arch, tools);
  }

  const cfgName = configDirName(cfg);
  const suffix = cfgName === "rel" ? "" : `-${cfgName}`;
  const packageName = `SumatraPDF-${extractSumatraVersion()}-mac-${opts.arch}${suffix}`;
  console.log("Building macOS packages...");
  const outDir = macOutDir(cfg, opts.arch);
  await packageMacApp({ outDir, appDir, packageName, dmg: opts.dmg ?? cfgName === "rel" });

  const elapsed = ((performance.now() - startTime) / 1000).toFixed(1);
  console.log(`\n=== macOS build (${cfgName}, ${opts.arch}) done in ${elapsed}s: ${appDir} ===`);
}
