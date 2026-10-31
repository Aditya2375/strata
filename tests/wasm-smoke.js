/* Smoke test for the browser (wasm) build: boots the demo module and checks
   the session API, including that over-long lines are refused whole - the
   wasm path shares the CLI's line-length check. Run after sh wasm/build.sh:
   node tests/wasm-smoke.js [path/to/strata.js] */
const factory = require(process.argv[2] || require("path").join(__dirname, "..", "docs", "strata.js"));
let out = "";
let fails = 0;
function ok(cond, label) {
    if (cond) console.log("ok: " + label);
    else { console.log("FAIL: " + label); fails = 1; }
}
factory({ print: s => { out += s + "\n"; } }).then(m => {
    const run = (line) => { out = ""; m.ccall("strata_exec", null, ["string"], [line]); return out; };
    m.ccall("strata_boot", null, [], []);
    run("write /f hello");
    ok(run("cat /f").includes("hello"), "wasm: write/cat round-trip");
    const long = run("mkdir " + "x".repeat(5000));
    ok(long.includes("line too long"), "wasm: over-long line refused");
    ok(!run("ls").split("\n").some(l => l.startsWith("xxx")), "wasm: long line left no partial entry");
    ok(run("fsck").includes("clean"), "wasm: fsck clean after refused line");
    const eoflen = run("mkdir /" + "y".repeat(5000));
    ok(eoflen.includes("line too long"), "wasm: 5000-char arg refused, not truncated");
    process.exit(fails);
});
