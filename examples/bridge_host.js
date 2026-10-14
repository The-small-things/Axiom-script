// bridge_host.js — the host side of examples/bridge.ax: start the program, then for each step
// write one command and read one line of state. A real host would draw the state and take the
// commands from its own input (a window's key events, a controller, an agent); this one sends
// a fixed list and prints what comes back.
//
//   node examples/bridge_host.js                     (the JavaScript runtime)
//   node examples/bridge_host.js native/axiom        (or any runtime command)
'use strict';
const { spawn } = require('child_process');
const path = require('path');
const readline = require('readline');

const root = path.join(__dirname, '..');
const program = path.join(__dirname, 'bridge.ax');
const runtime = process.argv[2];
const child = runtime
  ? spawn(runtime, [program], { stdio: ['pipe', 'pipe', 'inherit'] })
  : spawn(process.execPath, [path.join(root, 'main.js'), program], { stdio: ['pipe', 'pipe', 'inherit'] });

const lines = readline.createInterface({ input: child.stdout });
const commands = ['right', 'right', 'jump', 'right', 'none', 'none', 'left', 'quit'];
let i = 0;
function send() {
  const cmd = commands[i++];
  child.stdin.write(cmd + '\n');
  if (cmd === 'quit') child.stdin.end();
}
lines.on('line', (line) => {
  const state = JSON.parse(line);
  console.log(`${commands[i - 1].padEnd(6)} → step ${state.step}: x=${state.x} y=${state.y}`);
  send();
});
child.on('exit', (code) => process.exit(code || 0));
send();
