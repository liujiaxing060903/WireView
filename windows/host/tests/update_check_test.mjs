import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

let release = { tag_name: 'v2.0.10', draft: false, prerelease: false };
let fail = false;
const window = {};
vm.runInNewContext(readFileSync(new URL('../assets/web/update.js', import.meta.url), 'utf8'), {
  window, AbortController, setTimeout, clearTimeout,
  fetch: async () => { if (fail) throw new Error('offline'); return { ok: true, json: async () => release }; }
});
const { isNewer, check } = window.WireViewUpdate;
assert.equal(isNewer('v2.0.10', '2.0.9'), true, 'numeric patch ordering');
assert.equal(isNewer('3.0.0', '2.99.99'), true, 'major ordering');
assert.equal(isNewer('2.0.0', '2.0.0'), false);
assert.equal(isNewer('1.9.9', '2.0.0'), false);
assert.throws(() => isNewer('2.0.1-beta', '2.0.0'));
assert.equal((await check('2.0.9')).newer, true);
release.prerelease = true;
await assert.rejects(check('2.0.9'));
release.prerelease = false;
release.draft = true;
await assert.rejects(check('2.0.9'));
release.draft = false;
fail = true;
await assert.rejects(check('2.0.9'));
fail = false;
assert.equal((await check('2.0.10')).newer, false, 'checking succeeds after a failed request');
console.log('update_check_test passed');
