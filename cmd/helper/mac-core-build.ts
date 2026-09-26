/**
 * POSIX core check for the macOS app (cmd/build.ts -mac-core): builds, on Linux or macOS, everything of the app
 * except its Cocoa (.mm) layer, from the source lists in mac-build.ts.
 *
 * Native (default): third-party libraries, src/base, the engines, the reader model and src/mac/*.cpp; links and
 * runs test_util and test_mac_thumbnails, links test_engines and test_mac_engine, compiles src/gui/mac/*.cpp.
 * Output: out/mac-core-<cfg>-<cc>/ (cfg: dbg / rel / asan / rel_asan; cc: gcc / clang / zig).
 *
 * -cross: compiles the same sources to macOS objects with zig (`zig c++ -target aarch64-macos` / x86_64-macos) to
 * catch Darwin-specific breakage without a Mac. Objects only; Apple frameworks aren't available, so the .mm files
 * are skipped. Output: out/mac-cross-<cfg>-<arch>/.
 */

import { chmodSync, existsSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { type BuildTools, invalidateObjsIfBuildChanged } from "../deps-build-common";
import {
  type CompileArgs,
  type MacArch,
  type PosixConfig,
  type PosixTarget,
  buildPosixLibs,
  buildTestExes,
  cocoaSources,
  commonCompileFlags,
  compileSources,
  configDirName,
  defaultJobs,
  guiMacSources,
  hostArch,
  kMacMinVersion,
  macTarget,
  runTestUtil,
  setReproducibleEnv,
  stageMupdfFonts,
  testExeSources,
} from "./mac-build";

export type CoreCompiler = "gcc" | "clang" | "zig";

export interface MacCoreOptions {
  isRelease: boolean;
  asan: boolean;
  clean: boolean;
  cc?: CoreCompiler;
  jobs?: number;
}

export interface MacCrossOptions {
  isRelease: boolean;
  clean: boolean;
  arch: MacArch | "universal";
  jobs?: number;
}

function which(names: string[]): string | undefined {
  for (const n of names) {
    const p = Bun.which(n);
    if (p) return p;
  }
  return undefined;
}

// zig, from SUMATRA_ZIG, PATH, or the pip ziglang package's python-zig wrapper
function findZig(): string {
  const zig = process.env.SUMATRA_ZIG || which(["zig", "python-zig"]);
  if (!zig) {
    throw new Error("zig not found: put zig on PATH or set SUMATRA_ZIG (e.g. pip install ziglang -> python-zig)");
  }
  return zig;
}

// `zig cc` & co. as single commands, for tools that take a compiler path
function zigTools(dir: string): BuildTools {
  const zig = findZig();
  mkdirSync(dir, { recursive: true });
  const make = (name: string, sub: string): string => {
    const path = resolve(dir, name);
    writeFileSync(path, `#!/bin/sh\nexec "${zig}" ${sub} "$@"\n`);
    chmodSync(path, 0o755);
    return path;
  };
  return { cc: make("zig-cc", "cc"), cxx: make("zig-c++", "c++"), ar: make("zig-ar", "ar") };
}

function nativeTools(cc: CoreCompiler, outDir: string): BuildTools {
  if (cc === "zig") return zigTools(join(outDir, "tools"));
  const [c, cxx] = cc === "gcc" ? ["gcc", "g++"] : ["clang", "clang++"];
  if (!Bun.which(c) || !Bun.which(cxx)) throw new Error(`${c} / ${cxx} not found`);
  return { cc: c, cxx, ar: "ar" };
}

function nativeTarget(): PosixTarget {
  if (process.platform === "darwin") return macTarget(hostArch());
  if (process.platform === "linux") return { os: "linux", arch: hostArch(), flags: [] };
  throw new Error(`-mac-core runs on Linux or macOS (this is ${process.platform}); from Windows use WSL`);
}

async function runExe(exe: string, args: string[]): Promise<void> {
  console.log(`> ${exe} ${args.join(" ")}`);
  const env = { ...process.env, ASAN_OPTIONS: "abort_on_error=1:halt_on_error=1:detect_leaks=0" };
  const proc = Bun.spawn([exe, ...args], { env, stdout: "inherit", stderr: "inherit" });
  const code = await proc.exited;
  if (code !== 0) throw new Error(`${exe} failed with exit code ${code}`);
}

export async function buildMacCore(opts: MacCoreOptions): Promise<void> {
  const t = nativeTarget();
  const cc: CoreCompiler = opts.cc ?? (t.os === "mac" ? "clang" : "gcc");
  if (cc === "zig" && opts.asan) throw new Error("zig ships no ASan runtime: use -cc gcc or -cc clang with -asan");
  setReproducibleEnv();
  const cfg: PosixConfig = { isRelease: opts.isRelease, asan: opts.asan };
  const outDir = join("out", `mac-core-${configDirName(cfg)}-${cc}`);
  if (opts.clean) rmSync(outDir, { recursive: true, force: true });
  const tools = nativeTools(cc, outDir);
  const a: CompileArgs = { t, cfg, tools, outDir, jobs: opts.jobs ?? defaultJobs() };
  invalidateObjsIfBuildChanged(outDir, tools, configDirName(cfg), commonCompileFlags(t, cfg));
  const startTime = performance.now();
  console.log(`\n=== mac core check (${t.os} ${t.arch}, ${configDirName(cfg)}, ${cc}) -> ${outDir} ===\n`);

  const libs = await buildPosixLibs(a);
  const exes = await buildTestExes(a, libs);
  console.log("Compiling src/gui/mac/*.cpp (links only into the app)...");
  await compileSources(a, "app", guiMacSources());
  stageMupdfFonts(outDir);

  await runTestUtil(exes.test_util);
  await runExe(exes.test_mac_thumbnails, []);

  const elapsed = ((performance.now() - startTime) / 1000).toFixed(1);
  console.log(`\n=== mac core check done in ${elapsed}s ===`);
  for (const [name, exe] of Object.entries(exes)) console.log(`${name}: ${exe}`);
}

// zig's clang with Apple's libc headers but no frameworks
function crossTarget(arch: MacArch): PosixTarget {
  const zigArch = arch === "arm64" ? "aarch64" : "x86_64";
  return { os: "mac", arch, flags: ["-target", `${zigArch}-macos.${kMacMinVersion}`, "-Wno-nullability-completeness"] };
}

async function crossCompileArch(opts: MacCrossOptions, arch: MacArch): Promise<void> {
  const cfg: PosixConfig = { isRelease: opts.isRelease, asan: false };
  const outDir = join("out", `mac-cross-${configDirName(cfg)}-${arch}`);
  if (opts.clean) rmSync(outDir, { recursive: true, force: true });
  const tools = zigTools(join(outDir, "tools"));
  const t = crossTarget(arch);
  const a: CompileArgs = { t, cfg, tools, outDir, jobs: opts.jobs ?? defaultJobs() };
  invalidateObjsIfBuildChanged(outDir, tools, configDirName(cfg), commonCompileFlags(t, cfg));
  console.log(`\n=== macOS ${arch} cross-compile (objects only) -> ${outDir} ===\n`);

  const libs = await buildPosixLibs(a);
  const srcs = new Set<string>();
  for (const list of Object.values(testExeSources())) for (const s of list) srcs.add(s);
  for (const s of guiMacSources()) srcs.add(s);
  const sorted = [...srcs].sort();
  const objs = await compileSources(a, "app", sorted);
  const missing = objs.filter((o) => !existsSync(o));
  if (missing.length > 0) throw new Error(`missing objects: ${missing.join(", ")}`);
  console.log(`macOS ${arch}: ${libs.length} libraries, ${sorted.length} app / test sources compiled`);
  console.log(`skipped (need AppKit, not available to zig): ${cocoaSources().join(" ")}`);
}

export async function buildMacCross(opts: MacCrossOptions): Promise<void> {
  if (process.platform === "win32") throw new Error("-mac-core -cross runs on Linux or macOS; from Windows use WSL");
  setReproducibleEnv();
  const archs: MacArch[] = opts.arch === "universal" ? ["arm64", "x64"] : [opts.arch];
  for (const arch of archs) await crossCompileArch(opts, arch);
}
