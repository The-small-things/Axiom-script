// gen_regex.js — write regex_tables.h, the Unicode data regex.c needs to match as V8 does, all
// of it read back from this Node's own regular expressions:
//   * the sets behind \p{…}: General_Category, binary properties, Script and Script_Extensions,
//     under every name V8 accepts for them;
//   * the strings of the properties of strings (\p{RGI_Emoji} …, v flag), from
//     tools/data/emoji-sequences.json, each checked against Node;
//   * case-insensitive equivalence: which characters /x/i and /x/iu treat as the same, found by
//     scanning every character with V8's matcher (not derived from the spec's Canonicalize).
// usage: node tools/gen_regex.js > regex_tables.h     (difftest.sh checks it is current)
'use strict';
const fs = require('fs');
const path = require('path');

const MAX = 0x110000;
// Every code point once, in one string: the low surrogates are placed before the high ones so
// that no high surrogate is followed by a low one (which would make a pair).
const order = [];
for (let c = 0; c < 0xd800; c++) order.push(c);
for (let c = 0xdc00; c < 0xe000; c++) order.push(c);
for (let c = 0xd800; c < 0xdc00; c++) order.push(c);
for (let c = 0xe000; c < MAX; c++) order.push(c);
const ALL = String.fromCodePoint(...order.slice(0, 1)) + order.slice(1).map((c) => String.fromCodePoint(c)).join('');
const unitToCp = new Uint32Array(ALL.length);
for (let i = 0, k = 0; i < ALL.length; k++) {
  const w = order[k] >= 0x10000 ? 2 : 1;
  unitToCp[i] = order[k];
  if (w === 2) unitToCp[i + 1] = order[k];
  i += w;
}

const valid = (src, flags = 'u') => { try { new RegExp(src, flags); return true; } catch { return false; } };

// The code points a one-character class matches, as sorted ranges.
function setOf(classSrc) {
  const cps = [];
  for (const m of ALL.matchAll(new RegExp(`(?:${classSrc})+`, 'gu'))) {
    for (let i = m.index; i < m.index + m[0].length;) {
      const cp = unitToCp[i];
      cps.push(cp);
      i += cp >= 0x10000 ? 2 : 1;
    }
  }
  cps.sort((a, b) => a - b);
  const ranges = [];
  for (const c of cps) {
    const r = ranges[ranges.length - 1];
    if (r && r[1] + 1 === c) r[1] = c;
    else ranges.push([c, c]);
  }
  return ranges;
}

const sets = [];
const setIndex = new Map();
function internSet(ranges) {
  const key = ranges.map((r) => r.join('-')).join(',');
  if (!setIndex.has(key)) { setIndex.set(key, sets.length); sets.push(ranges); }
  return setIndex.get(key);
}

// --- General_Category ---------------------------------------------------------------------------
const GC = [['C', 'Other'], ['Cc', 'Control', 'cntrl'], ['Cf', 'Format'], ['Cn', 'Unassigned'], ['Co', 'Private_Use'],
  ['Cs', 'Surrogate'], ['L', 'Letter'], ['LC', 'Cased_Letter'], ['Ll', 'Lowercase_Letter'], ['Lm', 'Modifier_Letter'],
  ['Lo', 'Other_Letter'], ['Lt', 'Titlecase_Letter'], ['Lu', 'Uppercase_Letter'], ['M', 'Mark', 'Combining_Mark'],
  ['Mc', 'Spacing_Mark'], ['Me', 'Enclosing_Mark'], ['Mn', 'Nonspacing_Mark'], ['N', 'Number'], ['Nd', 'Decimal_Number', 'digit'],
  ['Nl', 'Letter_Number'], ['No', 'Other_Number'], ['P', 'Punctuation', 'punct'], ['Pc', 'Connector_Punctuation'],
  ['Pd', 'Dash_Punctuation'], ['Pe', 'Close_Punctuation'], ['Pf', 'Final_Punctuation'], ['Pi', 'Initial_Punctuation'],
  ['Po', 'Other_Punctuation'], ['Ps', 'Open_Punctuation'], ['S', 'Symbol'], ['Sc', 'Currency_Symbol'], ['Sk', 'Modifier_Symbol'],
  ['Sm', 'Math_Symbol'], ['So', 'Other_Symbol'], ['Z', 'Separator'], ['Zl', 'Line_Separator'], ['Zp', 'Paragraph_Separator'],
  ['Zs', 'Space_Separator']];
const gcNames = [];
for (const names of GC) {
  const ok = names.filter((n) => valid(`\\p{gc=${n}}`));
  if (!ok.length) continue;
  const set = internSet(setOf(`\\p{gc=${ok[0]}}`));
  for (const n of ok) gcNames.push([n, set]);
}

// --- binary properties ----------------------------------------------------------------------------
const BINARY = [['ASCII'], ['ASCII_Hex_Digit', 'AHex'], ['Alphabetic', 'Alpha'], ['Any'], ['Assigned'], ['Bidi_Control', 'Bidi_C'],
  ['Bidi_Mirrored', 'Bidi_M'], ['Case_Ignorable', 'CI'], ['Cased'], ['Changes_When_Casefolded', 'CWCF'],
  ['Changes_When_Casemapped', 'CWCM'], ['Changes_When_Lowercased', 'CWL'], ['Changes_When_NFKC_Casefolded', 'CWKCF'],
  ['Changes_When_Titlecased', 'CWT'], ['Changes_When_Uppercased', 'CWU'], ['Dash'], ['Default_Ignorable_Code_Point', 'DI'],
  ['Deprecated', 'Dep'], ['Diacritic', 'Dia'], ['Emoji'], ['Emoji_Component', 'EComp'], ['Emoji_Modifier', 'EMod'],
  ['Emoji_Modifier_Base', 'EBase'], ['Emoji_Presentation', 'EPres'], ['Extended_Pictographic', 'ExtPict'], ['Extender', 'Ext'],
  ['Grapheme_Base', 'Gr_Base'], ['Grapheme_Extend', 'Gr_Ext'], ['Hex_Digit', 'Hex'], ['IDS_Binary_Operator', 'IDSB'],
  ['IDS_Trinary_Operator', 'IDST'], ['IDS_Unary_Operator', 'IDSU'], ['ID_Compat_Math_Continue'], ['ID_Compat_Math_Start'],
  ['ID_Continue', 'IDC'], ['ID_Start', 'IDS'], ['Ideographic', 'Ideo'], ['InCB'], ['Join_Control', 'Join_C'],
  ['Logical_Order_Exception', 'LOE'], ['Lowercase', 'Lower'], ['Math'], ['Modifier_Combining_Mark', 'MCM'],
  ['Noncharacter_Code_Point', 'NChar'], ['Pattern_Syntax', 'Pat_Syn'], ['Pattern_White_Space', 'Pat_WS'],
  ['Quotation_Mark', 'QMark'], ['Radical'], ['Regional_Indicator', 'RI'], ['Sentence_Terminal', 'STerm'], ['Soft_Dotted', 'SD'],
  ['Terminal_Punctuation', 'Term'], ['Unified_Ideograph', 'UIdeo'], ['Uppercase', 'Upper'], ['Variation_Selector', 'VS'],
  ['White_Space', 'space'], ['XID_Continue', 'XIDC'], ['XID_Start', 'XIDS']];
const binNames = [];
for (const names of BINARY) {
  const ok = names.filter((n) => valid(`\\p{${n}}`));
  if (!ok.length) continue;
  const set = internSet(setOf(`\\p{${ok[0]}}`));
  for (const n of ok) binNames.push([n, set]);
}

// --- Script and Script_Extensions ----------------------------------------------------------------
// Every four-letter code V8 accepts, found by trying them all; long names from the display names
// and, where those differ from Unicode's property value names, from PropertyValueAliases.txt.
const EXTRA_LONG = { Beng: 'Bengali', Cans: 'Canadian_Aboriginal', Dupl: 'Duployan', Egyp: 'Egyptian_Hieroglyphs',
  Kits: 'Khitan_Small_Script', Lana: 'Tai_Tham', Mand: 'Mandaic', Mend: 'Mende_Kikakui', Merc: 'Meroitic_Cursive',
  Mero: 'Meroitic_Hieroglyphs', Mtei: 'Meetei_Mayek', Nkoo: 'Nko', Nshu: 'Nushu', Orkh: 'Old_Turkic', Orya: 'Oriya',
  Phag: 'Phags_Pa', Plrd: 'Miao', Qaac: 'Coptic', Qaai: 'Inherited', Rohg: 'Hanifi_Rohingya', Wara: 'Warang_Citi',
  Xsux: 'Cuneiform', Zzzz: 'Unknown', Zyyy: 'Common', Zinh: 'Inherited', Hrkt: 'Katakana_Or_Hiragana', Copt: 'Coptic',
  Geok: 'Georgian', Laoo: 'Lao', Hira: 'Hiragana', Kana: 'Katakana', Hani: 'Han', Hang: 'Hangul' };
const UP = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';
const LO = 'abcdefghijklmnopqrstuvwxyz';
const codes = [];
for (const a of UP) for (const b of LO) for (const c of LO) for (const d of LO) if (valid(`\\p{sc=${a + b + c + d}}`)) codes.push(a + b + c + d);
const display = new Intl.DisplayNames(['en'], { type: 'script' });
const scriptNames = new Map();   // name -> [sc set, scx set]
for (const code of codes) {
  const sc = internSet(setOf(`\\p{sc=${code}}`));
  const scx = internSet(setOf(`\\p{scx=${code}}`));
  scriptNames.set(code, [sc, scx]);
  const dn = display.of(code) || '';
  const cands = new Set([EXTRA_LONG[code], dn, dn.replace(/[ -]/g, '_'), dn.replace(/[’']/g, '').replace(/[ -]/g, '_')].filter(Boolean));
  for (const n of cands) {
    if (n === code || !/^[A-Za-z_]+$/.test(n) || !valid(`\\p{sc=${n}}`)) continue;
    // A long name counts only if it names the same sets.
    if (internSet(setOf(`\\p{sc=${n}}`)) === sc && internSet(setOf(`\\p{scx=${n}}`)) === scx) scriptNames.set(n, [sc, scx]);
  }
}

// --- case-insensitive equivalence ------------------------------------------------------------------
// /i without u works on UTF-16 units: each unit's class is every unit V8 matches it with.
function foldPairs(unicode) {
  const pairs = [];
  const seen = new Set();
  const universe = unicode ? ALL : Array.from({ length: 0x10000 }, (_, u) => String.fromCharCode(u)).join('');
  const toCp = (s, i) => (unicode ? unitToCp[i] : s.charCodeAt(i));
  const candidates = [];
  if (unicode) {
    for (const [lo, hi] of setOf('[\\p{CWCF}\\p{CWCM}]')) for (let c = lo; c <= hi; c++) candidates.push(c);
  } else {
    for (let u = 0; u < 0x10000; u++) {
      const s = String.fromCharCode(u);
      if (s.toUpperCase() !== s || s.toLowerCase() !== s) candidates.push(u);
    }
  }
  for (const c of candidates) {
    if (seen.has(c)) continue;
    const esc = c < 0x10000 ? `\\u${c.toString(16).padStart(4, '0')}` : `\\u{${c.toString(16)}}`;
    const re = new RegExp(`[${esc}]`, unicode ? 'giu' : 'gi');
    const members = [];
    for (const m of universe.matchAll(re)) members.push(toCp(universe, m.index));
    members.sort((a, b) => a - b);
    for (const m of members) seen.add(m);
    if (members.length < 2) continue;
    for (const m of members) if (m !== members[0]) pairs.push([m, members[0]]);
  }
  pairs.sort((a, b) => a[0] - b[0]);
  return pairs;
}
const foldNonU = foldPairs(false);
const foldU = foldPairs(true);

// --- properties of strings (v flag) ---------------------------------------------------------------
const emojiData = JSON.parse(fs.readFileSync(path.join(__dirname, 'data', 'emoji-sequences.json'), 'utf8'));
const STRING_PROPS = ['Basic_Emoji', 'Emoji_Keycap_Sequence', 'RGI_Emoji_Modifier_Sequence', 'RGI_Emoji_Flag_Sequence',
  'RGI_Emoji_Tag_Sequence', 'RGI_Emoji_ZWJ_Sequence'];
const stringProps = [];
for (const p of STRING_PROPS) {
  const list = (emojiData[p] || []).filter((s) => new RegExp(`^\\p{${p}}$`, 'v').test(s));
  const singles = list.filter((s) => [...s].length === 1).map((s) => s.codePointAt(0)).sort((a, b) => a - b);
  const ranges = [];
  for (const c of singles) { const r = ranges[ranges.length - 1]; if (r && r[1] + 1 === c) r[1] = c; else ranges.push([c, c]); }
  stringProps.push([p, list.filter((s) => [...s].length > 1), internSet(ranges)]);
}

// --- output -----------------------------------------------------------------------------------------
const lines = [
  `// regex_tables.h — generated by tools/gen_regex.js from Node ${process.version} (Unicode ${process.versions.unicode},`,
  `// ICU ${process.versions.icu}); do not edit.`,
  '',
];
// Ranges as deltas from the previous number, set by set.
const flat = [];
const setDefs = [];
for (const rs of sets) {
  setDefs.push([flat.length, rs.length]);
  let prev = 0;
  for (const [lo, hi] of rs) { flat.push(lo - prev, hi - lo); prev = hi; }
}
lines.push('// Each set is n ranges, written as (start - previous end, end - start) pairs.');
lines.push('static const uint32_t RX_DELTAS[] = {');
for (let i = 0; i < flat.length; i += 24) lines.push('  ' + flat.slice(i, i + 24).join(',') + ',');
lines.push('};');
lines.push('typedef struct { uint32_t off, n; } RxSetDef;');
lines.push('static const RxSetDef RX_SETS[] = {');
for (let i = 0; i < setDefs.length; i += 8) lines.push('  ' + setDefs.slice(i, i + 8).map(([o, n]) => `{${o},${n}}`).join(', ') + ',');
lines.push('};', `#define RX_SETS_N ${setDefs.length}`, '');
lines.push('typedef struct { const char *name; uint16_t set; } RxName;');
for (const [cname, list] of [['RX_GC', gcNames], ['RX_BINARY', binNames]]) {
  list.sort((a, b) => (a[0] < b[0] ? -1 : 1));
  lines.push(`static const RxName ${cname}[] = {`);
  for (const [n, s] of list) lines.push(`  { "${n}", ${s} },`);
  lines.push('};', `#define ${cname}_N ${list.length}`, '');
}
lines.push('typedef struct { const char *name; uint16_t sc, scx; } RxScript;');
lines.push('static const RxScript RX_SCRIPT[] = {');
const sn = [...scriptNames].sort((a, b) => (a[0] < b[0] ? -1 : 1));
for (const [n, [a, b]] of sn) lines.push(`  { "${n}", ${a}, ${b} },`);
lines.push('};', `#define RX_SCRIPT_N ${sn.length}`, '');
for (const [cname, pairs] of [['RX_FOLD_NONU', foldNonU], ['RX_FOLD_U', foldU]]) {
  lines.push(`// Characters that ${cname === 'RX_FOLD_U' ? '/x/iu' : '/x/i'} matches with a smaller one: (character, the smallest of its class).`);
  lines.push(`static const uint32_t ${cname}[][2] = {`);
  for (let i = 0; i < pairs.length; i += 8) lines.push('  ' + pairs.slice(i, i + 8).map(([a, b]) => `{0x${a.toString(16)},0x${b.toString(16)}}`).join(',') + ',');
  lines.push('};', `#define ${cname}_N ${pairs.length}`, '');
}
const cstr = (s) => '"' + [...Buffer.from(s, 'utf8')].map((b) => '\\x' + b.toString(16).padStart(2, '0')).join('') + '"';
lines.push('// Properties of strings: their sequences of two or more characters, and a set for the rest.');
lines.push('typedef struct { const char *name; const char *const *strings; uint32_t n; uint16_t set; } RxStringProp;');
for (const [p, multi] of stringProps) {
  lines.push(`static const char *const RX_STR_${p}[] = {`);
  for (let i = 0; i < multi.length; i += 6) lines.push('  ' + multi.slice(i, i + 6).map(cstr).join(', ') + ',');
  if (!multi.length) lines.push('  ""');
  lines.push('};');
}
lines.push('static const RxStringProp RX_STRING_PROPS[] = {');
for (const [p, multi, set] of stringProps) lines.push(`  { "${p}", RX_STR_${p}, ${multi.length}, ${set} },`);
lines.push('};', `#define RX_STRING_PROPS_N ${stringProps.length}`, '');
process.stdout.write(lines.join('\n'));
