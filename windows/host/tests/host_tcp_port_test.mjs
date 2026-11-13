// Verify the real Host CLI listens and emits HELLO on a per-session port.
// This does not capture a display, create a VDD, or talk to an Android device.
import assert from 'node:assert/strict';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
const fixture = mkdtempSync(path.join(tmpdir(), 'wireview-host-port-'));
try {
  const reservation = net.createServer();
  reservation.listen(0, '127.0.0.1'); await once(reservation, 'listening');
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const child = spawn(process.argv[2], ['--probe-device-profile', '--status-file', path.join(fixture, 'status.ini'), '--tcp-port', String(port)], { windowsHide: true });
  const exited = once(child, 'exit');
  let errorOutput = '';
  child.stderr.on('data', data => { errorOutput += data; });
  let timer, retry, socket, finished = false;
  try {
    await new Promise((resolve, reject) => {
      let completed = false;
      child.once('exit', code => { if (!completed) reject(new Error(`Host exited ${code}: ${errorOutput}`)); });
      timer = setTimeout(() => reject(new Error('Host did not listen on the requested session port')), 7000);
      const connect = () => {
        socket = net.connect({ host: '127.0.0.1', port });
        let hello = Buffer.alloc(0);
        socket.once('error', () => { if (!finished) retry = setTimeout(connect, 40); });
        socket.on('data', data => {
          hello = Buffer.concat([hello, data]);
          if (hello.length < 40) return;
          const payloadSize = hello.readUInt32LE(12);
          if (hello.length < 40 + payloadSize) return;
          try {
            assert.equal(hello.subarray(0, 4).toString('ascii'), 'WVR1');
            assert.equal(hello[6], 1, 'expected WireView HELLO packet');
            assert.equal(hello.subarray(40, 40 + payloadSize).toString('utf8'), 'WireView device profile',
              'identification must not announce a preview or stream startup');
          } catch (error) { reject(error); return; }
          finished = true; completed = true; resolve();
        });
      };
      connect();
    });
  } finally {
    finished = true;
    clearTimeout(timer); clearTimeout(retry);
    child.kill(); await exited;
    socket?.destroy();
  }
  console.log('Host custom TCP port emitted HELLO');
} finally { rmSync(fixture, { recursive: true, force: true }); }
