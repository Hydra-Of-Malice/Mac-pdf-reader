import { copyFileSync, existsSync, readdirSync, statSync } from "node:fs";
import { cpus } from "node:os";
import { join, relative } from "node:path";
import { $ } from "bun";
import { clearDirPreserveSettings } from "./clean";
import { ensureNinja, ninjaDir, ninjaToRoot } from "./ninja";
import { detectVisualStudio2026, runLogged } from "./util";

type BuildMode =
  | "windows"
  | "all"
  | "smoke"
  | "ci"
  | "daily"
  | "codeql"
  | "mingw"
  | "wine"
  | "mac"
  | "mac-core"
  | "mac-remote"
  | "build-no";
type Config = "debug" | "release" | "profile";
type MacArchOpt = "arm64" | "x64" | "universal";
type CoreCompiler = "gcc" | "clang" | "zig";

interface BuildOptions {
  mode?: BuildMode;
  config?: Config;
  asan: boolean;
  clean: boolean;
  ninja: boolean;
  msbuild: boolean;
  win32: boolean;
  run: boolean;
  runArgs: string[];
  buildNo?: string;
  arch?: MacArchOpt;
  cc?: CoreCompiler;
  cross: boolean;
  dmg: boolean;
  branch?: string;
}

const usage = `Usage: bun cmd/build.ts <mode> [options]

Windows builds:
  -dbg | -rel             Build SumatraPDF.exe for x64
  -profile                Build a function-timing profile variant (out/prf64)
  -rel -32                Build the 32-bit release
  -asan [-dbg|-rel]       Build SumatraPDF-static.exe with MSVC ASan
  -all [-clean]           Build release SumatraPDF and SumatraPDF-static
  -smoke                  Rebuild release SumatraPDF, then run the debug unit tests
  -ci                     Build CI/pre-release artifacts
  -daily                  Build daily artifacts
  -codeql                 Build the static release target for CodeQL

MinGW cross-builds (they still produce a Windows exe):
  -mingw <-dbg|-rel> [-clean]
                           Direct MinGW cross-build on the current host
  -wine [-clean] [-run] [-- <SumatraPDF args>]
                           MinGW build on Linux and optionally run under Wine;
                           from Windows it runs through WSL Ubuntu

macOS (native Cocoa app, see docs/mac/BUILDING.md):
  -mac [-dbg|-rel] [-asan] [-arch arm64|x64|universal] [-dmg] [-clean]
                           Build SumatraPDF.app on macOS (Xcode); defaults to -dbg
                           and the host arch. Output: out/mac-<cfg>-<arch>/ and
                           .tar.gz / .zip packages (-rel also a .dmg, -dmg forces one)
  -mac-core [-dbg|-rel] [-asan] [-cc gcc|clang|zig] [-clean]
                           Build the app's portable core (engines, reader model,
                           src/mac/*.cpp) and its tests on Linux or macOS, run
                           test_util: out/mac-core-<cfg>-<cc>/
  -mac-core -cross [-dbg|-rel] [-arch arm64|x64|universal] [-clean]
                           Compile the same sources for macOS with zig (objects
                           only, no Mac needed); default: both archs
  -mac-remote -branch <name> [-dbg|-rel] [-asan] [-arch ...] [-dmg] [-clean]
                           Run -mac for a pushed branch on a Mac over ssh
                           ($SUMATRA_MAC_HOST, checkout $SUMATRA_MAC_DIR)

Other:
  -build-no [number|sha1] List recent build numbers or resolve a number or sha1
  -h | -help              Print this help

General options:
  -clean                  Clean the selected output directory first
  -ninja                  Use Ninja instead of MSBuild
  -msbuild                Use MSBuild (the default)
  -32                     Select Win32 (valid only with Windows -rel)`;

class CliError extends Error {}

function setMode(opts: BuildOptions, mode: BuildMode): void {
  if (opts.mode) {
    throw new CliError(`build modes -${opts.mode} and -${mode} cannot be used together`);
  }
  opts.mode = mode;
}

// the command-line flag that selects a given configuration
function configFlag(config: Config): string {
  if (config === "debug") return "-dbg";
  if (config === "profile") return "-profile";
  return "-rel";
}

function setConfig(opts: BuildOptions, config: Config): void {
  if (opts.config) {
    throw new CliError(`${configFlag(opts.config)} and ${configFlag(config)} cannot be used together`);
  }
  opts.config = config;
}

function parseArgs(args: string[]): BuildOptions | undefined {
  if (args.length === 0) {
    return undefined;
  }
  const helpArgs = new Set(["-h", "-help", "--help"]);
  if (args.some((arg) => helpArgs.has(arg))) {
    if (args.length !== 1) throw new CliError("help cannot be combined with other options");
    return undefined;
  }
  const opts: BuildOptions = {
    asan: false,
    clean: false,
    ninja: false,
    msbuild: false,
    win32: false,
    run: false,
    runArgs: [],
    cross: false,
    dmg: false,
  };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === "--") {
      opts.runArgs.push(...args.slice(i + 1));
      break;
    }
    if (arg === "-dbg") setConfig(opts, "debug");
    else if (arg === "-rel") setConfig(opts, "release");
    else if (arg === "-profile") setConfig(opts, "profile");
    else if (arg === "-asan") {
      if (opts.asan) throw new CliError("-asan can only be specified once");
      opts.asan = true;
    } else if (arg === "-clean") {
      if (opts.clean) throw new CliError("-clean can only be specified once");
      opts.clean = true;
    } else if (arg === "-ninja") {
      if (opts.ninja) throw new CliError("-ninja can only be specified once");
      opts.ninja = true;
    } else if (arg === "-msbuild") {
      if (opts.msbuild) throw new CliError("-msbuild can only be specified once");
      opts.msbuild = true;
    } else if (arg === "-32") {
      if (opts.win32) throw new CliError("-32 can only be specified once");
      opts.win32 = true;
    } else if (arg === "-rel-32") {
      if (opts.win32) throw new CliError("-32 can only be specified once");
      setConfig(opts, "release");
      opts.win32 = true;
    } else if (arg === "-all") setMode(opts, "all");
    else if (arg === "-smoke") setMode(opts, "smoke");
    else if (arg === "-ci") setMode(opts, "ci");
    else if (arg === "-daily") setMode(opts, "daily");
    else if (arg === "-codeql") setMode(opts, "codeql");
    else if (arg === "-mingw") setMode(opts, "mingw");
    else if (arg === "-wine" || arg === "-win") setMode(opts, "wine");
    else if (arg === "-run") {
      if (opts.run) throw new CliError("-run can only be specified once");
      opts.run = true;
    } else if (arg === "-mac") setMode(opts, "mac");
    else if (arg === "-mac-core") setMode(opts, "mac-core");
    else if (arg === "-mac-remote") setMode(opts, "mac-remote");
    else if (arg === "-cross") {
      if (opts.cross) throw new CliError("-cross can only be specified once");
      opts.cross = true;
    } else if (arg === "-dmg") {
      if (opts.dmg) throw new CliError("-dmg can only be specified once");
      opts.dmg = true;
    } else if (arg === "-arch") {
      if (opts.arch) throw new CliError("-arch can only be specified once");
      const value = args[++i];
      if (value !== "arm64" && value !== "x64" && value !== "universal") {
        throw new CliError("-arch requires arm64, x64 or universal");
      }
      opts.arch = value;
    } else if (arg === "-cc") {
      if (opts.cc) throw new CliError("-cc can only be specified once");
      const value = args[++i];
      if (value !== "gcc" && value !== "clang" && value !== "zig") throw new CliError("-cc requires gcc, clang or zig");
      opts.cc = value;
    } else if (arg === "-branch") {
      if (opts.branch) throw new CliError("-branch can only be specified once");
      const value = args[++i];
      if (!value || value.startsWith("-")) throw new CliError("-branch requires a branch name");
      opts.branch = value;
    } else if (arg === "-build-no") {
      setMode(opts, "build-no");
      const value = args[i + 1];
      if (value && !value.startsWith("-")) {
        opts.buildNo = value;
        i++;
      }
    } else {
      throw new CliError(`unknown option: ${arg}`);
    }
  }

  if (!opts.mode) {
    if (opts.config || opts.asan || opts.win32) opts.mode = "windows";
    else throw new CliError("missing build mode");
  }
  validateOptions(opts);
  return opts;
}

function reject(condition: boolean, message: string): void {
  if (condition) throw new CliError(message);
}

function validateOptions(opts: BuildOptions): void {
  const mode = opts.mode!;
  const fixedModes: BuildMode[] = ["all", "smoke", "ci", "daily", "codeql", "wine", "build-no"];
  if (fixedModes.includes(mode)) {
    reject(!!opts.config, `${opts.config ? configFlag(opts.config) : ""} is not valid with -${mode}`);
    reject(opts.asan, `-asan is not valid with -${mode}`);
  }
  if (mode === "windows") {
    reject(!opts.config && !opts.asan, "Windows builds require -dbg, -rel, -profile, or -asan");
    reject(opts.win32 && (opts.config !== "release" || opts.asan), "-32 requires a non-ASan -rel build");
    reject(opts.asan && opts.config === "profile", "-asan is not supported with -profile");
  }
  if (mode === "mingw") {
    reject(!opts.config, "-mingw requires -dbg or -rel");
    reject(opts.asan, "-asan is not supported with -mingw");
  }
  const macModes: BuildMode[] = ["mac", "mac-core", "mac-remote"];
  if (macModes.includes(mode)) {
    reject(opts.config === "profile", `-profile is not valid with -${mode}`);
  }
  reject(opts.cross && mode !== "mac-core", "-cross is only valid with -mac-core");
  reject(opts.cross && opts.asan, "-asan is not valid with -mac-core -cross");
  reject(!!opts.cc && (mode !== "mac-core" || opts.cross), "-cc is only valid with -mac-core (not -cross)");
  reject(
    !!opts.arch && !(mode === "mac" || mode === "mac-remote" || opts.cross),
    "-arch is only valid with -mac, -mac-remote or -mac-core -cross",
  );
  reject(opts.dmg && mode !== "mac" && mode !== "mac-remote", "-dmg is only valid with -mac or -mac-remote");
  reject(!!opts.branch && mode !== "mac-remote", "-branch is only valid with -mac-remote");
  reject(mode === "mac-remote" && !opts.branch, "-mac-remote requires -branch <name>");
  reject(
    opts.clean && !["windows", "all", "mingw", "wine", ...macModes].includes(mode),
    `-clean is not valid with -${mode}`,
  );
  reject(opts.ninja && opts.msbuild, "-ninja and -msbuild cannot be used together");
  reject(opts.ninja && !["windows", "all", "smoke"].includes(mode), `-ninja is not valid with -${mode}`);
  reject(opts.msbuild && !["windows", "all", "smoke"].includes(mode), `-msbuild is not valid with -${mode}`);
  reject(opts.win32 && mode !== "windows", "-32 is only valid for Windows builds");
  reject(opts.run && mode !== "wine", "-run is only valid with -wine");
  reject(opts.runArgs.length > 0 && mode !== "wine", "arguments after -- are only valid with -wine");
  reject(opts.runArgs.length > 0 && !opts.run, "arguments after -- require -run");
}

function formatElapsed(ms: number): string {
  const seconds = Math.round(ms / 1000);
  const minutes = Math.floor(seconds / 60);
  if (minutes === 0) return `${seconds}s`;
  return `${minutes}m ${seconds % 60}s`;
}

async function buildApp(msbuildPath: string, configName: string, platform: string, target: string): Promise<void> {
  const targets = ["PdfFilter", "PdfPreview", "sumatrapdf-tool", target];
  for (const name of targets) {
    await runLogged(msbuildPath, [
      String.raw`vs2022\SumatraPDF.sln`,
      `/t:${name}`,
      `/p:Configuration=${configName};Platform=${platform}`,
      "/m",
    ]);
  }
}

function windowsConfigName(config: Config): string {
  if (config === "release") return "Release";
  if (config === "profile") return "Profile";
  return "Debug";
}

function windowsOutDir(config: Config, win32: boolean): string {
  if (win32) return "rel32";
  if (config === "release") return "rel64";
  if (config === "profile") return "prf64";
  return "dbg64";
}

async function buildWindows(config: Config, win32: boolean, clean: boolean, ninja: boolean): Promise<void> {
  const configName = windowsConfigName(config);
  const platform = win32 ? "Win32" : "x64";
  const outDir = join("out", windowsOutDir(config, win32));
  console.log(`${configName} ${platform} build`);
  if (clean) clearDirPreserveSettings(outDir);
  if (ninja) {
    await buildNinja([join(ninjaToRoot, outDir, "SumatraPDF.exe")]);
  } else {
    const { msbuildPath } = detectVisualStudio2026();
    await buildApp(msbuildPath, configName, platform, "SumatraPDF");
  }
  printBinaries(outDir, new Set(["SumatraPDF.exe"]));
}

async function buildNinja(targets: string[]): Promise<void> {
  await ensureNinja();
  const jobs = Math.max(1, cpus().length - 1);
  await runLogged("ninja", ["-C", ninjaDir, "-j", `${jobs}`, ...targets]);
}

function printBinaries(dir: string, targets: Set<string>): void {
  const paths: string[] = [];
  const dynamicFiles = new Set([
    "SumatraPDF.exe",
    "libsumatrapdf.dll",
    "PdfFilter.dll",
    "PdfPreview.dll",
    "sumatrapdf-tool.exe",
  ]);
  const walk = (path: string): void => {
    for (const entry of readdirSync(path, { withFileTypes: true })) {
      const entryPath = join(path, entry.name);
      if (entry.isDirectory()) {
        walk(entryPath);
        continue;
      }
      const relPath = relative(dir, entryPath).replaceAll("\\", "/");
      const isDynamic = targets.has("SumatraPDF.exe") && dynamicFiles.has(relPath);
      if (entry.isFile() && (targets.has(entry.name) || isDynamic)) {
        paths.push(entryPath);
      }
    }
  };

  walk(dir);
  for (const path of paths.sort()) {
    const size = statSync(path).size;
    console.log(`${relative(".", path)}: ${(size / 1_000_000).toFixed(1)} MB, ${size.toLocaleString("en-US")}`);
  }
}

const asanDllName = "clang_rt.asan_dynamic-x86_64.dll";

function findAsanDll(vsRoot: string): string {
  const candidates = [join(vsRoot, String.raw`VC\Tools\MSVC`), join(vsRoot, String.raw`VC\Tools\Llvm\x64\lib\clang`)];
  const walk = (dir: string): string | undefined => {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      const path = join(dir, entry.name);
      if (entry.isDirectory()) {
        const found = walk(path);
        if (found) return found;
      } else if (entry.name === asanDllName) {
        return path;
      }
    }
    return undefined;
  };
  for (const base of candidates) {
    if (existsSync(base)) {
      const found = walk(base);
      if (found) return found;
    }
  }
  throw new Error(`could not find ${asanDllName} under ${vsRoot}`);
}

async function buildWindowsAsan(config: Config, clean: boolean, ninja: boolean): Promise<void> {
  const configName = config === "release" ? "Release" : "Debug";
  const outDir = join("out", config === "release" ? "rel64_asan" : "dbg64_asan");
  console.log(`${configName} ASan build (SumatraPDF-static.exe, x64_asan)`);
  if (clean) clearDirPreserveSettings(outDir);
  const { msbuildPath, vsRoot } = detectVisualStudio2026();
  if (ninja) {
    await buildNinja([join(ninjaToRoot, outDir, "SumatraPDF-static.exe")]);
  } else {
    await runLogged(msbuildPath, [
      String.raw`vs2022\SumatraPDF.sln`,
      "/t:SumatraPDF-static",
      `/p:Configuration=${configName};Platform=x64_asan`,
      "/m",
    ]);
  }
  printBinaries(outDir, new Set(["SumatraPDF-static.exe"]));
  copyFileSync(findAsanDll(vsRoot), join(outDir, asanDllName));
  console.log(`exe: ${join(outDir, "SumatraPDF-static.exe")}`);
}

async function buildAll(clean: boolean, ninja: boolean): Promise<void> {
  const outDir = join("out", "rel64");
  console.log("Release x64 SumatraPDF and SumatraPDF-static build");
  if (clean) clearDirPreserveSettings(outDir);
  if (ninja) {
    await buildNinja([join(ninjaToRoot, outDir, "SumatraPDF.exe"), join(ninjaToRoot, outDir, "SumatraPDF-static.exe")]);
  } else {
    const { msbuildPath } = detectVisualStudio2026();
    await buildApp(msbuildPath, "Release", "x64", "SumatraPDF");
    await runLogged(msbuildPath, [
      String.raw`vs2022\SumatraPDF.sln`,
      "/t:SumatraPDF-static",
      "/p:Configuration=Release;Platform=x64",
      "/m",
    ]);
  }
  printBinaries(outDir, new Set(["SumatraPDF.exe", "SumatraPDF-static.exe"]));
}

async function buildSmoke(ninja: boolean): Promise<void> {
  const outDir = join("out", "rel64");
  console.log("smoke build");
  clearDirPreserveSettings(outDir);
  if (ninja) {
    await buildNinja([join(ninjaToRoot, outDir, "SumatraPDF.exe")]);
  } else {
    const { msbuildPath } = detectVisualStudio2026();
    await buildApp(msbuildPath, "Release", "x64", "SumatraPDF:Rebuild");
  }
  printBinaries(outDir, new Set(["SumatraPDF.exe"]));
  // unit tests are compiled into the debug SumatraPDF only
  await runLogged("bun", [join("cmd", "run-unit-tests.ts"), "-dbg"]);
}

async function showBuildNo(query?: string): Promise<void> {
  const total = Number((await $`git rev-list --count HEAD`.text()).trim());
  if (!query) {
    const out = await $`git log -32 --oneline`.text();
    const lines = out.split("\n").filter((line) => line.trim() !== "");
    for (let i = 0; i < lines.length; i++) console.log(`${total - i + 1000} ${lines[i]}`);
    return;
  }
  if (/^\d+$/.test(query)) {
    const buildNo = Number(query);
    const skip = total - (buildNo - 1000);
    if (skip >= 0 && skip < total) {
      const line = (await $`git log -1 --skip ${skip} --oneline`.text()).trim();
      console.log(`${buildNo} ${line}`);
      return;
    }
  }
  const sha = (await $`git rev-parse --verify --quiet ${query}^{commit}`.nothrow().text()).trim();
  if (sha) {
    const count = Number((await $`git rev-list --count ${sha}`.text()).trim());
    const line = (await $`git log -1 --oneline ${sha}`.text()).trim();
    console.log(`${count + 1000} ${line}`);
    return;
  }
  if (/^\d+$/.test(query)) throw new Error(`build number ${query} is out of range`);
  throw new Error(`unknown commit or build number: ${query}`);
}

// the wine build runs in WSL Ubuntu when started from Windows
async function runWslLauncher(args: string[]): Promise<void> {
  const proc = Bun.spawn(["bun", "cmd/helper/wsl-build.ts", "-win", ...args], {
    stdout: "inherit",
    stderr: "inherit",
    stdin: "inherit",
  });
  const code = await proc.exited;
  if (code !== 0) throw new Error(`WSL wine build failed with exit code ${code}`);
}

async function runBuild(opts: BuildOptions): Promise<void> {
  const mode = opts.mode!;
  if (["windows", "all", "smoke"].includes(mode)) {
    // the exe embeds the manual from .work/docs (ci / daily do this themselves)
    const { genDocsForBuild } = await import("./gen-docs");
    await genDocsForBuild();
  }
  if (mode === "windows") {
    const config = opts.config ?? "debug";
    if (opts.asan) await buildWindowsAsan(config, opts.clean, opts.ninja);
    else await buildWindows(config, opts.win32, opts.clean, opts.ninja);
  } else if (mode === "all") await buildAll(opts.clean, opts.ninja);
  else if (mode === "smoke") await buildSmoke(opts.ninja);
  else if (mode === "ci") {
    const { buildCi } = await import("./helper/ci-build");
    await buildCi();
  } else if (mode === "daily") {
    const { buildDaily } = await import("./helper/daily-build");
    await buildDaily();
  } else if (mode === "codeql") {
    const { buildCodeql } = await import("./helper/codeql-build");
    await buildCodeql();
  } else if (mode === "mingw") {
    const { buildMingw } = await import("./helper/mingw-build");
    await buildMingw({
      outDir: `out/mingw-${opts.config === "release" ? "rel" : "dbg"}64`,
      isRelease: opts.config === "release",
      clean: opts.clean,
    });
  } else if (mode === "wine") {
    if (process.platform === "win32") {
      const args = [...(opts.clean ? ["-clean"] : []), ...(opts.run ? ["-run"] : [])];
      if (opts.runArgs.length) args.push("--", ...opts.runArgs);
      await runWslLauncher(args);
    } else {
      const { buildWine } = await import("./helper/wine-build");
      await buildWine({ clean: opts.clean, run: opts.run, runArgs: opts.runArgs });
    }
  } else if (mode === "mac" || mode === "mac-core" || mode === "mac-remote") await runMacBuild(opts);
  else if (mode === "build-no") await showBuildNo(opts.buildNo);
}

// the native arch of this machine, as -arch spells it
function hostMacArch(): "arm64" | "x64" {
  return process.arch === "arm64" ? "arm64" : "x64";
}

async function runMacBuild(opts: BuildOptions): Promise<void> {
  const isRelease = opts.config === "release";
  const jobs = cpus().length;
  if (opts.mode === "mac") {
    const { buildMac } = await import("./helper/mac-build");
    const arch = opts.arch ?? hostMacArch();
    await buildMac({ isRelease, asan: opts.asan, clean: opts.clean, arch, jobs, dmg: opts.dmg || undefined });
  } else if (opts.mode === "mac-remote") {
    const { buildMacRemote } = await import("./helper/mac-remote-build");
    const args = [isRelease ? "-rel" : "-dbg", ...(opts.asan ? ["-asan"] : []), ...(opts.clean ? ["-clean"] : [])];
    if (opts.arch) args.push("-arch", opts.arch);
    if (opts.dmg) args.push("-dmg");
    await buildMacRemote(opts.branch!, args);
  } else if (process.platform === "win32") {
    throw new Error("-mac-core runs on Linux or macOS; from Windows run it in WSL: wsl -- bun cmd/build.ts -mac-core");
  } else if (opts.cross) {
    const { buildMacCross } = await import("./helper/mac-core-build");
    await buildMacCross({ isRelease, clean: opts.clean, arch: opts.arch ?? "universal", jobs });
  } else {
    const { buildMacCore } = await import("./helper/mac-core-build");
    await buildMacCore({ isRelease, asan: opts.asan, clean: opts.clean, cc: opts.cc, jobs });
  }
}

async function main(): Promise<void> {
  let opts: BuildOptions | undefined;
  try {
    opts = parseArgs(Bun.argv.slice(2));
  } catch (error) {
    if (!(error instanceof CliError)) throw error;
    console.error(`error: ${error.message}\n`);
    console.error(usage);
    process.exitCode = 1;
    return;
  }
  if (!opts) {
    console.log(usage);
    return;
  }
  const timeStart = performance.now();
  try {
    await runBuild(opts);
  } finally {
    console.log(`build took ${formatElapsed(performance.now() - timeStart)}`);
  }
}

try {
  await main();
} catch (error) {
  const message = error instanceof Error ? error.message : String(error);
  console.error(`\nBuild failed: ${message}`);
  process.exitCode = 1;
}
