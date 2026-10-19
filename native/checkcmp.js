// checkcmp.js — compare two `--check --json` outputs from the two runtimes.
//
// Everything must match exactly — codes, severities, locations, snippets, every message and
// suggested fix, in order — except a parse error (AX-PARSE-000, and AX-USE-003 for an import
// that fails to parse), whose wording and position come from two different parsers: both must
// reject the program, but the reference lexes the whole file before parsing and recovers after
// an error, where the native parser reports the first error it meets. A program with a parse
// error is compared by that first diagnostic's code.
const fs = require('fs');
const [a, b] = process.argv.slice(2).map(p => JSON.parse(fs.readFileSync(p, 'utf8')));
const prose = (d) => d.error_code === 'AX-PARSE-000' || d.error_code === 'AX-USE-003';
const norm = (r) => {
  let ds = r.diagnostics;
  if (ds.length && ds[0].error_code === 'AX-PARSE-000') ds = [ds[0]];
  return { ok: r.ok, diagnostics: ds.map(d => prose(d) ? { ...d, message_for_human: '(parse error)', message_for_agent: '(parse error)', context_snippet: null, location: { ...d.location, line: d.error_code === 'AX-PARSE-000' ? null : d.location.line, col: null } } : d) };
};
const ja = JSON.stringify(norm(a), null, 1), jb = JSON.stringify(norm(b), null, 1);
if (ja === jb) process.exit(0);
const la = ja.split('\n'), lb = jb.split('\n');
for (let i = 0; i < Math.max(la.length, lb.length); i++) {
  if (la[i] !== lb[i]) { console.log(`first difference at line ${i + 1}:\n  js:     ${la[i]}\n  native: ${lb[i]}`); break; }
}
process.exit(1);
