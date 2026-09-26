// Runs the headless bridge driver (src/tools/test_mac_engine.cpp) over tests/mac/fixtures/manifest.json
// and prints a per-fixture and per-format PASS/FAIL table. Builds nothing itself.
//
// usage: bun tests/mac/run-engine-tests.ts --driver <path-to-test_mac_engine> [--only <id-substring>]
//                                          [--json <results.json>] [--timeout-ms <n>]
// The driver path can also come from MAC_ENGINE_DRIVER. Exit code 1 if any fixture fails.

import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { makeLargePdf } from "./fixture-lib.ts";

interface LinkExpect {
  kind: string;
  targetPage?: number;
  value?: string;
}

interface Fixture {
  id: string;
  format: string;
  path: string;
  expect: "open" | "fail" | "any";
  target?: boolean;
  generate?: string;
  pages?: number;
  minPages?: number;
  password?: string;
  prompts?: number;
  search?: string;
  searchPage?: number;
  tocMin?: number;
  tocFirst?: string;
  links?: LinkExpect[];
  props?: Record<string, string>;
  lastPageLandscape?: boolean;
  stress?: number;
  timeoutMs?: number;
}

interface RunResult {
  id: string;
  format: string;
  target: boolean;
  ok: boolean;
  outcome: string; // opened / failed-to-open / crash / timeout / missing
  failures: string[];
  ms: number;
  report?: any;
  stage?: string;
}

const repoRoot = join(import.meta.dir, "..", "..");
const defaultTimeoutMs = 60000;

function parseArgs() {
  const args = process.argv.slice(2);
  const opts = {
    driver: process.env.MAC_ENGINE_DRIVER ?? "",
    only: "",
    json: "",
    timeoutMs: defaultTimeoutMs,
  };
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    const next = () => args[++i] ?? "";
    if (a === "--driver") opts.driver = next();
    else if (a === "--only") opts.only = next();
    else if (a === "--json") opts.json = next();
    else if (a === "--timeout-ms") opts.timeoutMs = Number(next());
    else {
      console.error(`unknown argument: ${a}`);
      process.exit(2);
    }
  }
  if (!opts.driver || !existsSync(opts.driver)) {
    console.error(`driver not found: '${opts.driver}' (pass --driver or set MAC_ENGINE_DRIVER)`);
    process.exit(2);
  }
  // mupdf's built-in fonts are read from fonts/ next to the exe (src/EmbeddedResources_posix.cpp)
  const exeDir = dirname(opts.driver);
  if (!existsSync(join(exeDir, "fonts")) && !existsSync(join(exeDir, "..", "Resources", "fonts"))) {
    console.warn(
      `warning: no fonts/ next to ${opts.driver}: text in PDFs without embedded fonts and ebooks won't render`,
    );
  }
  return opts;
}

function ensureGenerated(f: Fixture, absPath: string) {
  if (!f.generate || existsSync(absPath)) return;
  const m = /^large-pdf-(\d+)$/.exec(f.generate);
  if (!m) throw new Error(`${f.id}: unknown generator ${f.generate}`);
  mkdirSync(dirname(absPath), { recursive: true });
  writeFileSync(absPath, makeLargePdf(Number(m[1])));
}

function driverArgs(f: Fixture, absPath: string): string[] {
  const args = [absPath];
  if (f.password) args.push("-password", f.password);
  if (f.search) args.push("-search", f.search);
  if (f.stress) args.push("-stress", String(f.stress));
  return args;
}

async function runDriver(driver: string, args: string[], timeoutMs: number) {
  const start = performance.now();
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
  const ms = performance.now() - start;
  clearTimeout(timer);
  // a killed driver's children could keep the pipes open: don't wait for them
  const drain = (p: Promise<string>) => Promise.race([p, Bun.sleep(2000).then(() => "")]);
  const [stdout, stderr] = await Promise.all([drain(stdoutP), drain(stderrP)]);
  return { stdout, stderr, code, timedOut, ms, signal: proc.signalCode };
}

function lastStage(stderr: string): string {
  const stages = [...stderr.matchAll(/^stage: (.*)$/gm)];
  return stages.length > 0 ? stages[stages.length - 1]![1]! : "";
}

function checkOpened(f: Fixture, r: any, fail: (s: string) => void) {
  const pages = r.pages ?? 0;
  if (f.pages !== undefined && pages !== f.pages) fail(`pages ${pages} != ${f.pages}`);
  if (f.minPages !== undefined && pages < f.minPages) fail(`pages ${pages} < ${f.minPages}`);
  if (r.badPageSizes) fail(`${r.badPageSizes} pages without a size`);
  if (!r.layout?.ok || !r.layoutPageCountOk) fail("continuous layout failed");
  if (!r.layoutSinglePageRotated) fail("single page rotated layout failed");
  if (!r.render1?.ok) fail("sync render of page 1 failed");
  else if (!(r.render1.inkRatio > 0)) fail("page 1 renders blank");
  if (!r.render1Rotated?.ok) fail("rotated render failed");
  else if (r.render1?.ok && Math.abs(r.render1Rotated.width - r.render1.height / 2) > 2) {
    fail(`rotated render ${r.render1Rotated.width}x${r.render1Rotated.height} not ~half of swapped page 1`);
  }
  if (pages > 1 && !r.renderLast?.ok) fail("render of last page failed");
  if (!r.renderOutOfRangeFails) fail("render of page count + 1 didn't fail");
  if (r.async?.copied !== r.async?.requested) fail(`async render: ${r.async?.copied}/${r.async?.requested} pages`);
  if (!r.asyncAfterReset) fail("async render after reset failed");
  if (f.lastPageLandscape) {
    const sz = r.pageSizes?.[pages - 1];
    if (!sz || !(sz[0] > sz[1])) fail(`last page not landscape: ${JSON.stringify(sz)}`);
  }

  if (f.search) {
    const s = r.search ?? {};
    if (s.anyTimedOut) fail("search timed out");
    if (!s.found) fail(`'${f.search}' not found`);
    else {
      if (f.searchPage !== undefined && s.page !== f.searchPage) fail(`'${f.search}' found on page ${s.page}`);
      if (!(s.rects > 0)) fail("search hit without rectangles");
      if (!s.backwardFound) fail("backward search failed");
      if (!r.selectionAtHit) fail("mouse selection over the hit copied no text");
    }
    if (s.missingWordFound) fail("found a word that isn't in the document");
    if (!r.selectAll?.containsWord) fail(`select-all text lacks '${f.search}'`);
  }

  const toc = r.toc ?? { count: 0, items: [] };
  if (f.tocMin !== undefined && toc.count < f.tocMin) fail(`toc has ${toc.count} items < ${f.tocMin}`);
  if (f.tocFirst !== undefined && toc.items?.[0]?.title !== f.tocFirst) {
    fail(`first toc item '${toc.items?.[0]?.title}' != '${f.tocFirst}'`);
  }
  if (toc.badPages) fail(`${toc.badPages} toc items point outside the document`);

  for (const want of f.links ?? []) {
    const hit = (r.links ?? []).some(
      (l: any) =>
        l.kind === want.kind &&
        (want.targetPage === undefined || l.targetPage === want.targetPage) &&
        (want.value === undefined || l.value === want.value),
    );
    if (!hit) fail(`no ${JSON.stringify(want)} link (found ${JSON.stringify(r.links ?? [])})`);
  }
  for (const [k, v] of Object.entries(f.props ?? {})) {
    const got = r.properties?.[k];
    if (!got || !String(got).includes(v)) fail(`property ${k}='${got}', want '${v}'`);
  }
}

async function runFixture(driver: string, f: Fixture, defTimeout: number): Promise<RunResult> {
  const res: RunResult = {
    id: f.id,
    format: f.format,
    target: f.target !== false,
    ok: false,
    outcome: "",
    failures: [],
    ms: 0,
  };
  const fail = (s: string) => res.failures.push(s);
  const absPath = join(repoRoot, f.path);
  ensureGenerated(f, absPath);
  if (!existsSync(absPath)) {
    res.outcome = "missing";
    fail(`fixture missing: ${f.path}`);
    return res;
  }

  const out = await runDriver(driver, driverArgs(f, absPath), f.timeoutMs ?? defTimeout);
  res.ms = out.ms;
  res.stage = lastStage(out.stderr);
  if (/AddressSanitizer|runtime error:|LeakSanitizer/.test(out.stderr)) {
    const line = out.stderr.split("\n").find((l) => /AddressSanitizer|runtime error:/.test(l)) ?? "";
    fail(`sanitizer: ${line.trim()}`);
  }
  if (out.timedOut) {
    res.outcome = "timeout";
    fail(`timeout after ${Math.round(out.ms)} ms in stage '${res.stage}'`);
    return res;
  }
  if (out.code !== 0) {
    res.outcome = "crash";
    fail(`driver exit ${out.code}${out.signal ? ` (${out.signal})` : ""} in stage '${res.stage}'`);
    return res;
  }
  let r: any;
  try {
    r = JSON.parse(out.stdout);
  } catch (e) {
    res.outcome = "crash";
    fail(`bad driver output: ${String(e)}`);
    return res;
  }
  res.report = r;
  res.outcome = r.open ? "opened" : "failed-to-open";

  if (f.prompts !== undefined && r.passwordPrompts !== f.prompts) {
    fail(`password prompts ${r.passwordPrompts} != ${f.prompts}`);
  }
  if (f.expect === "open") {
    if (!r.open) fail(`did not open: ${r.error}`);
    else checkOpened(f, r, fail);
  } else if (f.expect === "fail") {
    if (r.open) fail("opened but should fail");
    else if (!r.error) fail("failed without an error message");
  }
  res.ok = res.failures.length === 0;
  return res;
}

function pad(s: string, n: number): string {
  return s.length >= n ? s : s + " ".repeat(n - s.length);
}

function printTables(results: RunResult[]) {
  console.log("\nfixture results:");
  console.log(
    `${pad("id", 26)} ${pad("format", 11)} ${pad("result", 6)} ${pad("outcome", 15)} ${pad("ms", 7)} details`,
  );
  for (const r of results) {
    const details = r.failures.join("; ");
    console.log(
      `${pad(r.id, 26)} ${pad(r.format, 11)} ${pad(r.ok ? "PASS" : "FAIL", 6)} ${pad(r.outcome, 15)} ${pad(String(Math.round(r.ms)), 7)} ${details}`,
    );
  }

  const formats = new Map<string, RunResult[]>();
  for (const r of results) {
    if (!formats.has(r.format)) formats.set(r.format, []);
    formats.get(r.format)!.push(r);
  }
  console.log("\nper format:");
  console.log(`${pad("format", 11)} ${pad("result", 6)} ${pad("pass", 5)} ${pad("fail", 5)} failing fixtures`);
  for (const [format, rs] of formats) {
    const nFail = rs.filter((r) => !r.ok).length;
    const failing = rs
      .filter((r) => !r.ok)
      .map((r) => r.id)
      .join(", ");
    const note = rs.every((r) => !r.target) ? " (not a mac port target)" : "";
    console.log(
      `${pad(format, 11)} ${pad(nFail === 0 ? "PASS" : "FAIL", 6)} ${pad(String(rs.length - nFail), 5)} ${pad(String(nFail), 5)} ${failing}${note}`,
    );
  }
}

async function main() {
  const opts = parseArgs();
  const manifest = JSON.parse(readFileSync(join(import.meta.dir, "fixtures", "manifest.json"), "utf8"));
  const fixtures: Fixture[] = manifest.fixtures.filter((f: Fixture) => !opts.only || f.id.includes(opts.only));
  const results: RunResult[] = [];
  for (const f of fixtures) {
    const r = await runFixture(opts.driver, f, opts.timeoutMs);
    console.log(
      `${r.ok ? "PASS" : "FAIL"} ${f.id} (${Math.round(r.ms)} ms)${r.ok ? "" : ": " + r.failures.join("; ")}`,
    );
    results.push(r);
  }
  printTables(results);
  if (opts.json) writeFileSync(opts.json, JSON.stringify(results, null, 1));
  const nFail = results.filter((r) => !r.ok).length;
  console.log(`\n${results.length - nFail}/${results.length} fixtures passed`);
  process.exit(nFail === 0 ? 0 : 1);
}

await main();
