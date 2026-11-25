// node launcher/tests/setup_xex_test.js: tests the XEX2 header check and the
// "not the USA disc" message on Bad Company's other builds (Gold Edition,
// Platinum Hits, other regions), from made-up files (no game is read).
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { checkDefaultXex, findGameFiles, kGames } = require('../setup');

const kBfbc = 0x454107F9;
// A minimal XEX2 header with an EXECUTION_INFO optional header.
function fakeXex(titleId, version, mediaId) {
  const b = Buffer.alloc(0x40);
  b.write('XEX2', 0, 'latin1');
  b.writeUInt32BE(1, 20);              // 1 optional header
  b.writeUInt32BE(0x00040006, 0x18);   // EXECUTION_INFO
  b.writeUInt32BE(0x30, 0x1C);         // its offset
  b.writeUInt32BE(mediaId, 0x30);
  b.writeUInt32BE(version, 0x34);
  b.writeUInt32BE(0, 0x38);            // base version
  b.writeUInt32BE(titleId, 0x3C);
  return b;
}
// A tiny XDVDFS disc (as tools/xiso_extract.py reads it) with default.xex.
function fakeIso(xex) {
  const sector = 2048, root = 33, file = 40;
  const img = Buffer.alloc((file + 2) * sector);
  const vd = img.subarray(32 * sector, 32 * sector + 28);
  vd.write('MICROSOFT*XBOX*MEDIA', 0, 'latin1');
  vd.writeUInt32LE(root, 20);
  vd.writeUInt32LE(sector, 24);
  const dir = img.subarray(root * sector, root * sector + sector);
  const o = 0;
  dir.writeUInt16LE(0, o);             // left
  dir.writeUInt16LE(0, o + 2);         // right
  dir.writeUInt32LE(file, o + 4);      // first sector
  dir.writeUInt32LE(xex.length, o + 8);
  dir[o + 12] = 0;                     // attributes (a file)
  dir[o + 13] = 11;                    // name length
  dir.write('default.xex', o + 14, 'latin1');
  xex.copy(img, file * sector);
  return img;
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'bf1943-setup-'));
try {
  const gold = fakeXex(kBfbc, 0x00000006, 0x75F7BA6A);
  const folder = path.join(tmp, 'gold');
  fs.mkdirSync(folder);
  fs.writeFileSync(path.join(folder, 'default.xex'), gold);
  fs.writeFileSync(path.join(tmp, 'gold.iso'), fakeIso(gold));

  const why = checkDefaultXex('bfbc1', path.join(folder, 'default.xex'));
  assert.ok(why.includes('Gold Edition'), 'names the editions');
  assert.ok(why.includes('version 0.0.0.6') && why.includes('75F7BA6A'), 'says which build it is');
  assert.ok(why.includes(kGames.bfbc1.sha1), 'names the SHA-1 of the USA disc');

  const foundDisk = findGameFiles(tmp, 'bfbc1');
  assert.ok(foundDisk && foundDisk.wrong, 'the Gold .iso is found and refused');
  assert.ok(foundDisk.wrong.includes('Gold Edition'), 'with the same reason');
  const foundDir = findGameFiles(folder, 'bfbc1');
  assert.ok(foundDir && foundDir.wrong && foundDir.wrong.includes('0.0.0.6'), 'a Gold folder too');

  const other = path.join(tmp, 'other.xex');
  const otherXex = fakeXex(0x415607D1, 1, 0);
  fs.writeFileSync(other, otherXex);
  assert.strictEqual(checkDefaultXex('bfbc1', other),
    'This default.xex is not ' + kGames.bfbc1.expected + ' (SHA-1 ' +
    require('crypto').createHash('sha1').update(otherXex).digest('hex') + ').', 'other games stay generic');
  console.log('setup_xex_test: ok');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
