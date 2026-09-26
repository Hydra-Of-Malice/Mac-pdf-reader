// Mutation fuzzer for the macOS engine bridge: corrupts every fixture in tests/mac/fixtures/manifest.json in
// several ways, runs src/tools/test_mac_engine on each variant (build it with ASan) and reports crashes, sanitizer
// errors and hangs. Reproducers are saved under tests/tmp/mac-fuzz/repro/. Builds nothing itself.
//
// usage: bun tests/mac/fuzz-engine.ts --driver <test_mac_engine> [--count <variants per fixture>] [--seed <n>]
//          [--jobs <n>] [--timeout-ms <n>] [--budget-s <n>] [--only <id-substring>] [--ci]
// --ci: the small, fixed, fast subset CI runs (seed 1, 3 variants of each small fixture).
// A given --seed / --count always produces the same variants. Exit code 1 if anything crashed or hung.

import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { cpus } from "node:os";
import { extname, join } from "node:path";
import { prng } from "./fixture-lib.ts";

interface Fixture {
  id: string;
  format: string;
  path: string;
  generate?: string;
  password?: string;
}

interface Variant {
  fixture: Fixture;
  index: number;
  file: string;
  mutations: string[];
}

interface Outcome {
  variant: Variant;
  kind: "ok" | "crash" | "sanitizer" | "hang";
  detail: string;
  stage: string;
  ms: number;
  log?: string;
  opened?: boolean;
}

const repoRoot = join(import.meta.dir, "..", "..");
const outDir = join(repoRoot, "tests", "tmp", "mac-fuzz");
const workDir = join(outDir, "work");
const reproDir = join(outDir, "repro");
// fixtures bigger than this are skipped by --ci (each ASan run of a big file takes seconds)
const kCiMaxFixtureSize = 40 * 1024;

function parseArgs() {
  const args = process.argv.slice(2);
  const o = {
    driver: process.env.MAC_ENGINE_DRIVER ?? "",
    count: 20,
    seed: 1,
    jobs: Math.max(1, Math.min(16, cpus().length - 2)),
    timeoutMs: 30000,
    budgetS: 0,
    only: "",
    ci: false,
  };
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    const next = () => args[++i] ?? "";
    if (a === "--driver") o.driver = next();
    else if (a === "--count") o.count = Number(next());
    else if (a === "--seed") o.seed = Number(next());
    else if (a === "--jobs") o.jobs = Number(next());
    else if (a === "--timeout-ms") o.timeoutMs = Number(next());
    else if (a === "--budget-s") o.budgetS = Number(next());
    else if (a === "--only") o.only = next();
    else if (a === "--ci") o.ci = true;
    else {
      console.error(`unknown argument: ${a}`);
      process.exit(2);
    }
  }
  if (o.ci) {
    o.count = 3;
    o.seed = 1;
    o.jobs = Math.min(o.jobs, 4);
  }
  if (!o.driver || !existsSync(o.driver)) {
    console.error(`driver not found: '${o.driver}' (pass --driver or set MAC_ENGINE_DRIVER)`);
    process.exit(2);
  }
  return o;
}

function hashStr(s: string): number {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}

//--- mutations

const enc = new TextEncoder();

// fields worth breaking: [signature, offsets of length / offset / count fields after it]
const kMarkers: [string, number[]][] = [
  ["PK\x03\x04", [14, 18, 22, 26, 28]],
  ["PK\x01\x02", [16, 20, 24, 28, 30, 42]],
  ["PK\x05\x06", [8, 10, 12, 16]],
  ["Rar!\x1a\x07", [7, 9, 12, 14, 16, 20, 24]],
  ["7z\xbc\xaf\x27\x1c", [8, 12, 20, 28]],
  ["ustar", [-133, -124]],
  ["xref", [5, 7, 9]],
  ["startxref", [10, 11, 12]],
  ["/Length", [8]],
  ["/Count", [7]],
  [" obj", [-3, -2]],
  ["stream", [7, 20, 40]],
  ["FORM", [4, 6]],
  ["AT&T", [8, 10, 20]],
  ["BOOKMOBI", [8, 18, 22, 26, 30, 34]],
  ["TEXtREAd", [8, 18, 22, 26, 30]],
  ["MOBI", [4, 12, 64, 92, 96, 112, 148, 226]],
  ["ITOLITLS", [8, 12, 16, 20]],
  ["ITSF", [4, 16, 20]],
  ["IFCM", [8, 24]],
  ["AOLL", [4]],
  ["II*\x00", [4, 8, 10, 18]],
  ["\x89PNG", [8, 16, 20]],
  ["IHDR", [4, 8]],
  ["\xff\xc0", [3, 5]],
  ["GIF8", [6, 8, 10]],
  ["BM", [2, 10, 18, 22, 28]],
];

const kInteresting = [0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffff0];

function findAll(data: Uint8Array, sig: string, limit: number): number[] {
  const s = Uint8Array.from(sig, (c) => c.charCodeAt(0));
  const res: number[] = [];
  outer: for (let i = 0; i + s.length <= data.length && res.length < limit; i++) {
    for (let j = 0; j < s.length; j++) if (data[i + j] !== s[j]) continue outer;
    res.push(i);
  }
  return res;
}

function writeInt(data: Uint8Array, off: number, v: number, size: number, bigEndian: boolean) {
  for (let k = 0; k < size; k++) {
    const shift = bigEndian ? (size - 1 - k) * 8 : k * 8;
    if (off + k >= 0 && off + k < data.length) data[off + k] = (v >>> shift) & 0xff;
  }
}

type Mutator = (d: Uint8Array, rnd: () => number) => [Uint8Array, string];

const pick = (rnd: () => number, n: number) => Math.floor(rnd() * n);

const mutators: Mutator[] = [
  (d, rnd) => {
    const n = pick(rnd, d.length);
    return [d.slice(0, n), `truncate@${n}`];
  },
  (d, rnd) => {
    const n = 1 + pick(rnd, 8);
    for (let i = 0; i < n; i++) d[pick(rnd, d.length)]! ^= 1 << pick(rnd, 8);
    return [d, `flip${n}bits`];
  },
  (d, rnd) => {
    const n = 1 + pick(rnd, 16);
    const at = pick(rnd, d.length);
    for (let i = 0; i < n && at + i < d.length; i++) d[at + i] = pick(rnd, 256);
    return [d, `random${n}@${at}`];
  },
  (d, rnd) => {
    const n = 1 + pick(rnd, Math.max(1, Math.min(512, d.length >> 2)));
    const at = pick(rnd, d.length);
    d.fill(rnd() < 0.5 ? 0 : 0xff, at, at + n);
    return [d, `fill${n}@${at}`];
  },
  (d, rnd) => {
    const at = pick(rnd, d.length);
    const n = 1 + pick(rnd, Math.max(1, Math.min(4096, d.length - at)));
    const pos = pick(rnd, d.length);
    const chunk = d.slice(at, at + n);
    const res = new Uint8Array(d.length + chunk.length);
    res.set(d.subarray(0, pos));
    res.set(chunk, pos);
    res.set(d.subarray(pos), pos + chunk.length);
    return [res, `dup${n}@${pos}`];
  },
  (d, rnd) => {
    const at = pick(rnd, d.length);
    const n = 1 + pick(rnd, Math.max(1, Math.min(4096, d.length - at)));
    const res = new Uint8Array(d.length - Math.min(n, d.length - at));
    res.set(d.subarray(0, at));
    res.set(d.subarray(at + n), at);
    return [res, `cut${n}@${at}`];
  },
  // structure-aware: an extreme value in a length / offset / count field
  (d, rnd) => {
    const hits: [number, number[]][] = [];
    for (const [sig, fields] of kMarkers) {
      for (const at of findAll(d, sig, 64)) hits.push([at, fields]);
    }
    if (hits.length === 0) return [d, "no-marker"];
    const [at, fields] = hits[pick(rnd, hits.length)]!;
    const off = at + fields[pick(rnd, fields.length)]!;
    const v = kInteresting[pick(rnd, kInteresting.length)]!;
    const size = [1, 2, 4][pick(rnd, 3)]!;
    writeInt(d, off, v, size, rnd() < 0.5);
    return [d, `field@${off}=${v >>> 0}/${size}`];
  },
  // numbers in PDF syntax (object numbers, lengths, xref offsets)
  (d, rnd) => {
    const digits: number[] = [];
    for (let i = 0; i < d.length && digits.length < 4096; i++) if (d[i]! >= 0x30 && d[i]! <= 0x39) digits.push(i);
    if (digits.length === 0) return [d, "no-digits"];
    const at = digits[pick(rnd, digits.length)]!;
    const repl = enc.encode(["0", "9999999999", "-1", "2147483648", "65536"][pick(rnd, 5)]!);
    const res = new Uint8Array(d.length - 1 + repl.length);
    res.set(d.subarray(0, at));
    res.set(repl, at);
    res.set(d.subarray(at + 1), at + repl.length);
    return [res, `number@${at}`];
  },
];

function mutate(data: Uint8Array, rnd: () => number): [Uint8Array, string[]] {
  let d = data.slice();
  const names: string[] = [];
  const n = 1 + pick(rnd, 3);
  for (let i = 0; i < n && d.length > 0; i++) {
    const m = mutators[pick(rnd, mutators.length)]!;
    const [res, name] = m(d, rnd);
    d = res;
    names.push(name);
  }
  return [d, names];
}

//--- running

async function runDriver(driver: string, v: Variant, timeoutMs: number): Promise<Outcome> {
  const start = performance.now();
  const args = [v.file, "-render-all"];
  if (v.fixture.password) args.push("-password", v.fixture.password.split(",").pop()!);
  const proc = Bun.spawn([driver, ...args], {
    stdout: "pipe",
    stderr: "pipe",
    env: { ...process.env, ASAN_OPTIONS: process.env.ASAN_OPTIONS ?? "detect_leaks=0:abort_on_error=1" },
  });
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    proc.kill(9);
  }, timeoutMs);
  const stdoutP = new Response(proc.stdout).text();
  const stderrP = new Response(proc.stderr).text();
  const code = await proc.exited;
  clearTimeout(timer);
  const drain = (p: Promise<string>) => Promise.race([p, Bun.sleep(2000).then(() => "")]);
  const [stdout, stderr] = await Promise.all([drain(stdoutP), drain(stderrP)]);
  const ms = performance.now() - start;
  const stages = [...stderr.matchAll(/^stage: (.*)$/gm)];
  const stage = stages.length > 0 ? stages[stages.length - 1]![1]! : "";
  const lines = stderr.split("\n");
  const log = lines
    .filter((l) => !l.startsWith("stage: "))
    .slice(-60)
    .join("\n");
  const san = stderr.split("\n").find((l) => /ERROR: AddressSanitizer|runtime error:|SUMMARY: \w+Sanitizer/.test(l));
  if (timedOut) return { variant: v, kind: "hang", detail: `timeout ${timeoutMs} ms`, stage, ms, log };
  if (san) {
    // first frame in our code or a library, for grouping
    const frame = stderr.split("\n").find((l) => /^\s+#[0-9]+ .* in (?!__|asan|free|malloc|calloc|realloc)/.test(l));
    return { variant: v, kind: "sanitizer", detail: `${san.trim()} | ${frame?.trim() ?? ""}`, stage, ms, log };
  }
  if (code !== 0) {
    return {
      variant: v,
      kind: "crash",
      detail: `exit ${code}${proc.signalCode ? ` (${proc.signalCode})` : ""}`,
      stage,
      ms,
      log,
    };
  }
  return { variant: v, kind: "ok", detail: "", stage, ms, opened: stdout.includes('"open":true') };
}

function saveRepro(o: Outcome, stderrNote: string) {
  const v = o.variant;
  const base = `${v.fixture.id}-${v.index}-${o.kind}`;
  const dst = join(reproDir, base + extname(v.file));
  copyFileSync(v.file, dst);
  const note = [
    `fixture: ${v.fixture.path}`,
    `mutations: ${v.mutations.join(", ")}`,
    `result: ${o.kind} ${o.detail}`,
    `last stage: ${o.stage}`,
    `repro: <driver> ${dst} -render-all${v.fixture.password ? " -password <pw>" : ""}`,
    stderrNote,
  ].join("\n");
  writeFileSync(join(reproDir, `${base}.txt`), note);
  return dst;
}

async function main() {
  const o = parseArgs();
  const manifest = JSON.parse(readFileSync(join(import.meta.dir, "fixtures", "manifest.json"), "utf8"));
  rmSync(workDir, { recursive: true, force: true });
  mkdirSync(workDir, { recursive: true });
  mkdirSync(reproDir, { recursive: true });

  // one seed per path: fixtures listed twice (e.g. with and without a password) share their variants
  const seen = new Set<string>();
  const variants: Variant[] = [];
  for (const f of manifest.fixtures as Fixture[]) {
    if (f.generate || seen.has(f.path) || (o.only && !f.id.includes(o.only))) continue;
    const abs = join(repoRoot, f.path);
    // image folders etc.: only single files are mutated
    if (!existsSync(abs) || !statSync(abs).isFile()) continue;
    const data = new Uint8Array(readFileSync(abs));
    if (data.length === 0 || (o.ci && data.length > kCiMaxFixtureSize)) continue;
    seen.add(f.path);
    for (let i = 0; i < o.count; i++) {
      const rnd = prng((o.seed * 1000003) ^ hashStr(f.path) ^ Math.imul(i + 1, 2654435761));
      const [mutated, mutations] = mutate(data, rnd);
      const file = join(workDir, `${f.id}-${i}${extname(f.path)}`);
      writeFileSync(file, mutated);
      variants.push({ fixture: f, index: i, file, mutations });
    }
  }
  console.log(`${variants.length} variants of ${seen.size} fixtures, ${o.jobs} jobs, timeout ${o.timeoutMs} ms`);

  const outcomes: Outcome[] = [];
  const startAll = performance.now();
  let next = 0;
  let stoppedByBudget = false;
  const worker = async () => {
    for (;;) {
      if (o.budgetS > 0 && performance.now() - startAll > o.budgetS * 1000) {
        stoppedByBudget = true;
        return;
      }
      const v = variants[next++];
      if (!v) return;
      const res = await runDriver(o.driver, v, o.timeoutMs);
      outcomes.push(res);
      if (res.kind !== "ok") {
        const dst = saveRepro(res, res.log ?? "");
        console.log(
          `${res.kind.toUpperCase()} ${v.fixture.id}#${v.index} [${v.mutations.join(", ")}] stage '${res.stage}': ${res.detail}\n  -> ${dst}`,
        );
      }
    }
  };
  await Promise.all(Array.from({ length: o.jobs }, worker));

  // per format summary; problems grouped by signature
  const byFormat = new Map<string, { runs: number; bad: number; opened: number }>();
  for (const r of outcomes) {
    const s = byFormat.get(r.variant.fixture.format) ?? { runs: 0, bad: 0, opened: 0 };
    s.runs++;
    if (r.opened) s.opened++;
    if (r.kind !== "ok") s.bad++;
    byFormat.set(r.variant.fixture.format, s);
  }
  console.log("\nformat      runs  crashes+hangs");
  for (const [fmt, s] of byFormat) {
    console.log(`${fmt.padEnd(11)} ${String(s.runs).padEnd(5)} ${String(s.opened).padEnd(7)} ${s.bad}`);
  }
  const bad = outcomes.filter((r) => r.kind !== "ok");
  const groups = new Map<string, Outcome[]>();
  for (const r of bad) {
    const key = `${r.kind} | ${r.stage} | ${r.detail.replace(/0x[0-9a-f]+/g, "")}`;
    groups.set(key, [...(groups.get(key) ?? []), r]);
  }
  if (groups.size > 0) console.log("\nunique problems:");
  for (const [key, rs] of groups) console.log(`  ${rs.length}x ${key}\n     e.g. ${rs[0]!.variant.file}`);
  const secs = ((performance.now() - startAll) / 1000).toFixed(0);
  const budgetNote = stoppedByBudget ? ` (stopped by the ${o.budgetS} s budget)` : "";
  console.log(`\n${outcomes.length} runs in ${secs} s${budgetNote}: ${bad.length} crashes / hangs`);
  process.exit(bad.length === 0 ? 0 : 1);
}

await main();
