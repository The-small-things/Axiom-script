// keyboard.js — key input without waiting: poll_key() and keys_down().
//
// A script's loop asks for keys between its own steps instead of blocking on input(). Keys come
// from one of two places:
//   - stdin, read without blocking: a terminal (switched to unechoed byte-at-a-time mode for
//     the rest of the run), a pipe, or a file. This is how ^main scripts get them.
//   - the window or the terminal of a running world (main.js feeds them in with pushKey /
//     releaseKey), so &tick and &render code sees the same names.
//
// Bytes are decoded into key names the same way native/kbd.c decodes them:
//   printable characters   themselves ("a", "A", "é", "?")
//   space enter tab backspace escape up down left right home end insert delete pageup pagedown
//   other control bytes    "ctrl+a" … "ctrl+z"
// A terminal reports presses, never releases, so keys_down() counts a key as held for HOLD
// seconds after its last press (auto-repeat renews it while the key is down); a window reports
// releases, and keys_down() is then exact.
'use strict';
const fs = require('fs');

const HOLD = 0.35;
const QUEUE_MAX = 256;   // keys waiting for poll_key(); the oldest is dropped beyond this
const HELD_MAX = 64;     // keys keys_down() tracks at once

const state = {
  source: null,      // null: not opened yet; 'tty' | 'pipe' | 'file' | 'none' | 'external'
  fd: 0,
  eof: false,
  pending: [],       // bytes read but not yet decoded
  queue: [],         // decoded key names for poll_key()
  held: new Map(),   // name → time held until (Infinity while a window reports it down)
  lastCR: false,
};

const nowS = () => performance.now() / 1000;

function press(name, now) {
  if (state.queue.length >= QUEUE_MAX) state.queue.shift();
  state.queue.push(name);
  state.held.delete(name);
  if (state.held.size >= HELD_MAX) state.held.delete(state.held.keys().next().value);   // the oldest
  state.held.set(name, now + HOLD);
}

// Decode whole keys from the front of `pending`. A sequence cut off by the end of what has
// arrived is decoded as far as it goes (a lone ESC is the escape key).
const CSI_FINAL = { A: 'up', B: 'down', C: 'right', D: 'left', H: 'home', F: 'end' };
const CSI_TILDE = { 1: 'home', 2: 'insert', 3: 'delete', 4: 'end', 5: 'pageup', 6: 'pagedown', 7: 'home', 8: 'end' };
function decode(now) {
  const b = state.pending;
  let i = 0;
  while (i < b.length) {
    const c = b[i];
    if (c === 0x1b) {
      state.lastCR = false;
      if (i + 1 < b.length && (b[i + 1] === 0x5b || b[i + 1] === 0x4f)) {   // ESC [ or ESC O
        let j = i + 2;
        while (j < b.length && !(b[j] >= 0x40 && b[j] <= 0x7e)) j++;
        if (j >= b.length) { i = b.length; break; }                         // cut off: dropped
        const fin = String.fromCharCode(b[j]);
        let name = CSI_FINAL[fin] || null;
        if (fin === '~') {
          const param = parseInt(Buffer.from(b.slice(i + 2, j)).toString('latin1'), 10);
          name = CSI_TILDE[param] || null;
        }
        if (name) press(name, now);
        i = j + 1;
        continue;
      }
      press('escape', now);
      i++;
      continue;
    }
    if (c === 0x0a && state.lastCR) { state.lastCR = false; i++; continue; }   // CR LF is one enter
    state.lastCR = c === 0x0d;
    if (c === 0x0d || c === 0x0a) { press('enter', now); i++; continue; }
    if (c === 0x09) { press('tab', now); i++; continue; }
    if (c === 0x7f || c === 0x08) { press('backspace', now); i++; continue; }
    if (c === 0x20) { press('space', now); i++; continue; }
    if (c < 0x20) { press(c === 0 ? 'ctrl+space' : 'ctrl+' + String.fromCharCode(c <= 0x1a ? c + 0x60 : c + 0x40), now); i++; continue; }
    if (c < 0x80) { press(String.fromCharCode(c), now); i++; continue; }
    // UTF-8: a well-formed sequence is one key; anything else is U+FFFD, one byte at a time.
    const len = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 ? (c <= 0xef ? 3 : 0) : c >= 0xc2 ? 2 : 0;
    let cp = -1;
    if (len && i + len <= b.length) {
      cp = len === 2 ? c & 0x1f : len === 3 ? c & 0x0f : c & 0x07;
      for (let k = 1; k < len; k++) {
        if ((b[i + k] & 0xc0) !== 0x80) { cp = -1; break; }
        cp = (cp << 6) | (b[i + k] & 0x3f);
      }
      if (cp >= 0 && ((len === 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (len === 4 && (cp < 0x10000 || cp > 0x10ffff)))) cp = -1;
    }
    if (cp < 0) { press('�', now); i++; continue; }
    press(String.fromCodePoint(cp), now);
    i += len;
  }
  state.pending = b.slice(i);
}

function open() {
  if (state.source !== null) return;
  let st = null;
  try { st = fs.fstatSync(0); } catch (e) { state.source = 'none'; return; }
  if (process.stdin.isTTY) {
    // Raw mode turns off the terminal's own Ctrl-C; poll_key() brings it back (below).
    try { process.stdin.setRawMode(true); } catch (e) { state.source = 'none'; return; }
    process.on('exit', () => { try { process.stdin.setRawMode(false); } catch (e) { /* closed */ } });
    state.source = 'tty';
    state.fd = 0;   // libuv has made a terminal stdin non-blocking
  } else if (st.isFIFO() || st.isSocket()) {
    // A second, non-blocking descriptor for the same pipe.
    try { state.fd = fs.openSync('/dev/stdin', fs.constants.O_RDONLY | fs.constants.O_NONBLOCK); state.source = 'pipe'; }
    catch (e) { state.source = 'none'; }
  } else if (st.isFile()) {
    state.source = 'file';
    state.fd = 0;
  } else {
    state.source = 'none';
  }
}

function readAvailable(now) {
  open();
  if (state.source === 'external' || state.source === 'none' || state.eof) return;
  const buf = Buffer.alloc(256);
  for (;;) {
    let n;
    try { n = fs.readSync(state.fd, buf, 0, buf.length, null); }
    catch (e) { break; }   // EAGAIN: nothing waiting
    if (n === 0) { if (state.source !== 'tty') state.eof = true; break; }
    for (let k = 0; k < n; k++) {
      if (state.source === 'tty' && buf[k] === 0x03) {
        // Ctrl-C at a terminal interrupts, as it would without raw mode.
        try { process.stdin.setRawMode(false); } catch (e) { /* closed */ }
        process.exit(130);
      }
      state.pending.push(buf[k]);
    }
    if (n < buf.length) break;
  }
  decode(now);
}

function pollKey() {
  readAvailable(nowS());
  return state.queue.length ? state.queue.shift() : null;
}

function keysDown() {
  const now = nowS();
  readAvailable(now);
  const out = [];
  for (const [name, until] of state.held) {
    if (until > now) out.push(name);
    else state.held.delete(name);
  }
  return out;
}

// For main.js: a running world's window or terminal owns the keyboard, and feeds keys in.
function useExternal() { state.source = 'external'; }
function feedBytes(bytes) { for (const b of bytes) state.pending.push(b); decode(nowS()); }
function pushKey(name, held) {
  press(name, nowS());
  if (held) state.held.set(name, Infinity);
}
function releaseKey(name) { state.held.delete(name); }

module.exports = { pollKey, keysDown, useExternal, feedBytes, pushKey, releaseKey };
