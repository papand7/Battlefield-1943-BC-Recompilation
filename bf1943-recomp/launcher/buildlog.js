// The build's full output, saved to <root>/logs/build-<game>.log (root: the
// BattlefieldRecomp folder), and the part of it a failed build shows in the
// launcher: ninja's last lines ("ninja: build stopped: subcommand failed.")
// say nothing, the first FAILED: line and the first compiler errors do.
// (tests/buildlog_test.js tests failureSummary on a sample log.)
const fs = require('fs');
const path = require('path');

const kContext = 8;       // lines shown after each error line
const kMaxBlocks = 3;     // error spots shown (overlapping ones are one)
const kMaxLine = 220;     // a longer line is cut (compiler command lines are huge)
const kMaxLines = 60;     // the most lines the whole text has
// "file.cpp:12:3: error: ...", "error C2065: ...", "ld.lld: error: ...", "fatal error: ..."
const kError = /(^|[\s:\]])(fatal )?error( [A-Z]+\d+)?:/i;

const cut = (s) => (s.length > kMaxLine ? s.slice(0, kMaxLine) + '...' : s);

// The text for a failed build from its output lines: the first FAILED: line,
// then the first error lines each with the lines after them, then where the
// whole log is. logPath may be null.
function failureSummary(lines, logPath) {
  const out = [];
  const failed = lines.findIndex((l) => l.startsWith('FAILED:'));
  if (failed >= 0) out.push(cut(lines[failed]));
  // Error lines, starting from the first FAILED: (what was before is the
  // configure step's, shown only when nothing is after).
  const blocks = [];
  for (let i = Math.max(failed, 0); i < lines.length && blocks.length < kMaxBlocks; ++i) {
    if (!kError.test(lines[i]) || lines[i].startsWith('FAILED:')) continue;
    const last = blocks[blocks.length - 1];
    if (last && i <= last.end + 1) { last.end = Math.min(i + kContext, lines.length - 1); continue; }
    blocks.push({ start: i, end: Math.min(i + kContext, lines.length - 1) });
  }
  if (!blocks.length && failed < 0) {
    // Nothing recognisable: the end of the output, as before.
    out.push(...lines.slice(-12).map(cut));
  }
  for (const b of blocks) {
    if (out.length) out.push('');
    for (let i = b.start; i <= b.end; ++i) {
      if (lines[i].startsWith('FAILED:')) break;  // (the next target's failure is not this one's context)
      out.push(cut(lines[i]));
    }
  }
  const text = out.length > kMaxLines ? [...out.slice(0, kMaxLines), '...'] : out;
  if (logPath) text.push('', `Full build log: ${logPath}`);
  return text.join('\n');
}

// build-<game>.log under <root>/logs: { path, write(line), close() }. Never
// throws (a log that cannot be written must not stop the build).
function openBuildLog(root, game) {
  const file = path.join(root, 'logs', `build-${game}.log`);
  let fd = null;
  try {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fd = fs.openSync(file, 'w');
    fs.writeSync(fd, `# ${new Date().toISOString()} ${game} build\n`);
  } catch { fd = null; }
  return {
    path: file,
    write(line) { if (fd !== null) { try { fs.writeSync(fd, line + '\n'); } catch { /* disk full: carry on */ } } },
    close() { if (fd !== null) { try { fs.closeSync(fd); } catch { /* */ } fd = null; } },
    // The text for the failure just logged (reads the file back).
    explain() {
      this.close();
      let lines = [];
      try { lines = fs.readFileSync(file, 'utf8').split(/\r?\n/); } catch { /* none */ }
      return failureSummary(lines, file);
    },
  };
}

module.exports = { failureSummary, openBuildLog };
