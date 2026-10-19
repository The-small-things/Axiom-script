// simcmp.js — compare two `--sim N --json` dumps from the two runtimes.
//
// Everything must match exactly (entities, fields, positions, log, frame count, sim time)
// except the prose of a diagnostic: its code, entity, block and line must agree, but the
// wording of the raw error comes from two different host languages.
const fs = require('fs');
const [a, b] = process.argv.slice(2).map(p => JSON.parse(fs.readFileSync(p, 'utf8')));
const norm = d => {
  d.diagnostics = (d.diagnostics || [])
    .filter(x => x.error_code !== 'AX-RENDER-000')   // the reference's missing-renderer advisory
    .map(x => [x.error_code, x.location && x.location.entity, x.location && x.location.block, x.location && x.location.line]);
  return d;
};
const ja = JSON.stringify(norm(a), null, 1), jb = JSON.stringify(norm(b), null, 1);
if (ja === jb) process.exit(0);
const la = ja.split('\n'), lb = jb.split('\n');
for (let i = 0; i < Math.max(la.length, lb.length); i++) {
  if (la[i] !== lb[i]) {
    console.log(`first difference at line ${i + 1}:\n  js:     ${la[i]}\n  native: ${lb[i]}`);
    console.log('  context: ' + la.slice(Math.max(0, i - 6), i).join(' ').replace(/\s+/g, ' ').slice(-300));
    break;
  }
}
process.exit(1);
