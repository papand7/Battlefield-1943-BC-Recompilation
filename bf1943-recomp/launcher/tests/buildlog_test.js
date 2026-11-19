// node launcher/tests/buildlog_test.js: tests buildlog.js on a sample build log
// (no build is run).
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { failureSummary, openBuildLog } = require('../buildlog');

const sample = [
  '-- The C compiler identification is Clang 19.1.0',
  '-- Configuring done (3.2s)',
  '[1/812] Building CXX object game/CMakeFiles/bf1943.dir/runtime/a.cpp.obj',
  '[2/812] Building CXX object game/CMakeFiles/bf1943.dir/runtime/b.cpp.obj',
  'FAILED: game/CMakeFiles/bf1943.dir/runtime/net.cpp.obj ',
  'C:\\PROGRA~1\\LLVM\\bin\\clang-cl.exe  /nologo -TP -DNOMINMAX ' + '-I/x'.repeat(200) + ' -c net.cpp',
  'C:/src/runtime/net.cpp:120:17: error: use of undeclared identifier \'kPort\'',
  '  120 |   addr.port = kPort;',
  '      |               ^',
  'C:/src/runtime/net.cpp:131:9: error: no member named \'foo\' in \'Net\'',
  '  131 |   net.foo();',
  '      |   ~~~ ^',
  '2 errors generated.',
  'ninja: build stopped: subcommand failed.',
];
// a long tail after it: only the lines near the errors count
for (let i = 0; i < 30; ++i) sample.push(`noise line ${i}`);
sample.push('C:/src/runtime/other.cpp:9:1: error: expected \';\'');
sample.splice(sample.length, 0, 'FAILED: second.obj', 'clang-cl second', 'second.cpp:1:1: error: never shown');

const text = failureSummary(sample, 'C:/BattlefieldRecomp/logs/build-bf1943.log');
console.log(text);
const L = text.split('\n');
assert.strictEqual(L[0], 'FAILED: game/CMakeFiles/bf1943.dir/runtime/net.cpp.obj ');   // the first FAILED: line, first
assert.ok(text.includes("kPort"), 'first error');
assert.ok(text.includes("no member named 'foo'"), 'second error, in the first one\'s context');
assert.ok(text.includes('2 errors generated.') && text.includes('ninja: build stopped'), 'context after the error');
assert.ok(text.includes("other.cpp:9:1: error"), 'a later error spot');
assert.ok(!text.includes('second.obj') && !text.includes('never shown'), 'the second FAILED: is left out');
assert.ok(!text.includes('noise line 20'), 'lines far from an error are left out');
assert.ok(L.every((l) => l.length <= 240), 'long lines are cut');
assert.ok(text.trim().endsWith('Full build log: C:/BattlefieldRecomp/logs/build-bf1943.log'));

// No FAILED: and no error: line (a tool that died): the end of the output.
const t2 = failureSummary(['a', 'b', 'c'], null);
assert.strictEqual(t2, 'a\nb\nc');

// The file: every line written, read back for the summary.
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bflog-'));
const log = openBuildLog(dir, 'bfbc1');
for (const l of sample) log.write(l);
const t3 = log.explain();
assert.ok(fs.readFileSync(log.path, 'utf8').includes('noise line 29'), 'the log has the whole output');
assert.strictEqual(path.basename(log.path), 'build-bfbc1.log');
assert.ok(t3.startsWith('FAILED: game/CMakeFiles') && t3.includes(log.path));
fs.rmSync(dir, { recursive: true });
console.log('\nbuildlog tests passed');
