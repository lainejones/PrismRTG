#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Laine Jones
"""mkcatalog.py : PrismRTG's translation catalog, from the texts in the sources.

  mkcatalog.py --ids SOURCE.c ...            scan S(ID_NAME, "text") in the sources; number the IDs
                                              (stable: numbers already in catalogs/PrismRTG.cd are kept,
                                              new names get the next ones), write src/catalog_ids.h and
                                              catalogs/PrismRTG.cd (the CatComp-style description a
                                              translator starts from)
  mkcatalog.py --build LANG.ct OUT.catalog   compile a translation (.ct: "## language deutsch",
                                              "## version ...", then ID_NAME / translation / ; blocks)
                                              into a locale.library catalog (IFF CTLG)
  mkcatalog.py --check LANG.ct               list the IDs a translation lacks or has in excess

The English text is the one in the source; the .cd carries it so a translator sees it. A text may
hold %-formats; the translation must keep them in order."""
import io, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CD = os.path.join(ROOT, 'catalogs', 'PrismRTG.cd')
IDS_H = os.path.join(ROOT, 'src', 'catalog_ids.h')
VERSION = 1

S_RE = re.compile(r'\bS\(\s*(ID_[A-Z0-9_]+)\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)')


class Unchanged(io.StringIO):
    """a file written only if its text changed, so make doesn't rebuild what includes it"""
    def __init__(self, path):
        super().__init__()
        self.path = path

    def __exit__(self, *exc):
        if exc[0] is None:
            text = self.getvalue()
            try:
                with open(self.path, encoding='latin-1', newline='') as f:
                    same = f.read() == text
            except OSError:
                same = False
            if not same:
                with open(self.path, 'w', encoding='latin-1', newline='\n') as f:
                    f.write(text)
        return super().__exit__(*exc)


def c_unescape(lit):
    """the C string literal(s) -> text"""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', lit)
    s = ''.join(parts)
    return bytes(s, 'latin-1').decode('unicode_escape').encode('latin-1').decode('latin-1')


def read_cd(path):
    """existing numbering: name -> (number, text)"""
    d = {}
    if not os.path.exists(path):
        return d
    lines = open(path, encoding='latin-1').read().split('\n')
    i = 0
    while i < len(lines):
        m = re.match(r'^(ID_[A-Z0-9_]+)\s+\((\d+)//\)\s*$', lines[i])
        if m:
            text = []
            i += 1
            while i < len(lines) and lines[i] != ';':
                text.append(lines[i]); i += 1
            d[m.group(1)] = (int(m.group(2)), '\n'.join(text))
        i += 1
    return d


def cmd_ids(sources):
    found = {}
    for src in sources:
        s = open(src, encoding='latin-1').read()
        for m in S_RE.finditer(s):
            name, text = m.group(1), c_unescape(m.group(2))
            if name in found and found[name] != text:
                sys.exit('%s: %s has two texts: %r and %r' % (src, name, found[name], text))
            found[name] = text
    old = read_cd(CD)
    numbers = {n: old[n][0] for n in old}
    nxt = max(numbers.values(), default=0) + 1
    for name in sorted(found):
        if name not in numbers:
            numbers[name] = nxt; nxt += 1
    os.makedirs(os.path.dirname(CD), exist_ok=True)
    with Unchanged(CD) as f:
        f.write('; PrismRTG.cd - the texts of PrismRTG\'s tools, for translators (CatComp style).\n'
                '; Written by tools/mkcatalog.py from the sources; the numbers never change.\n'
                '; A translation is a .ct next to it: catalogs/deutsch.ct, compiled with mkcatalog.py --build.\n'
                '## version %d\n' % VERSION)
        for name in sorted(found, key=lambda n: numbers[n]):
            f.write('%s (%d//)\n%s\n;\n' % (name, numbers[name], found[name]))
    with Unchanged(IDS_H) as f:
        f.write('/* catalog_ids.h - written by tools/mkcatalog.py --ids; do not edit */\n'
                '#ifndef PRISM_CATALOG_IDS_H\n#define PRISM_CATALOG_IDS_H\n')
        for name in sorted(found, key=lambda n: numbers[n]):
            f.write('#define %s %d\n' % (name, numbers[name]))
        f.write('#endif\n')
    gone = [n for n in old if n not in found]
    print('%d texts, %d new, %d no longer in the sources%s' % (len(found), len([n for n in found if n not in old]),
          len(gone), ': ' + ' '.join(gone) if gone else ''))


def read_ct(path):
    lang, ver, d = None, None, {}
    lines = open(path, encoding='latin-1').read().split('\n')
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith('## language'):
            lang = l.split(None, 2)[2].strip()
        elif l.startswith('## version'):
            ver = l[10:].strip()
        elif re.match(r'^ID_[A-Z0-9_]+\s*$', l):
            name = l.strip(); text = []
            i += 1
            while i < len(lines) and lines[i] != ';':
                text.append(lines[i]); i += 1
            d[name] = '\n'.join(text)
        i += 1
    if not lang:
        sys.exit('%s: no "## language" line' % path)
    return lang, ver, d


def cmd_build(ct, out):
    lang, ver, tr = read_ct(ct)
    cd = read_cd(CD)
    body = b''
    for name, text in tr.items():
        if name not in cd:
            print('%s: not in the .cd, skipped' % name); continue
        n = cd[name][0]
        b = text.encode('latin-1') + b'\0'
        pad = (-len(b)) & 3
        body += struct.pack('>II', n, len(b)) + b + b'\0' * pad
    # the $VER locale.library parses for OC_Version: "name <version>.<revision> (<date>)"; the .ct's
    # "## version" line holds the date (dd.mm.yy), the number is this catalog format's
    import time as _t
    date = (ver or '').strip() or _t.strftime('%d.%m.%y')
    fver = ('$VER: PrismRTG.catalog %d.0 (%s)' % (VERSION, date)).encode('latin-1') + b'\0'
    langb = lang.encode('latin-1') + b'\0'
    chunks = b''                     # IFF: the size is the unpadded size, odd chunks get a pad byte
    for tag, data in (('FVER', fver), ('LANG', langb), ('CSET', b'\0' * 32), ('STRS', body)):
        chunks += tag.encode() + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')
    form = b'CTLG' + chunks
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'wb').write(b'FORM' + struct.pack('>I', len(form)) + form)
    missing = [n for n in cd if n not in tr]
    print('%s: %d texts for %s%s' % (out, len(tr), lang, ', %d untranslated' % len(missing) if missing else ''))


def cmd_check(ct):
    lang, ver, tr = read_ct(ct)
    cd = read_cd(CD)
    for n in cd:
        if n not in tr:
            print('missing  %s: %r' % (n, cd[n][1]))
    for n in tr:
        if n not in cd:
            print('unknown  %s' % n)


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == '--ids':
        cmd_ids(a[1:])
    elif len(a) == 3 and a[0] == '--build':
        cmd_build(a[1], a[2])
    elif len(a) == 2 and a[0] == '--check':
        cmd_check(a[1])
    else:
        sys.exit(__doc__)
