#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The interface's catalogues (lang/, runtime/lang/lang.h) made from the sources.

Every text marked in launcher/ and runtime/ goes into lang/hiraku.pot (the template), and each
lang/<code>.po is brought up to date with it: a text already translated keeps its translation, a new
one comes in empty (shown in English until it is translated), one whose English changed a little
keeps its old translation marked fuzzy (not used until a translator checks it: gettext's way), and
one no longer in the sources is kept at the end as obsolete (#~), not lost.

What is marked (the context is msgctxt: who shows the text):
    tr("text")  tr("text", "disambiguation")  tr("%n text", nullptr, n)
                                  in a launcher class's member (Q_OBJECT, Q_DECLARE_TR_FUNCTIONS):
                                  the context is the class's name, as Qt's own tr() has it
    Class::tr("text")             that class's
    language::tr("ctx", "text")   language::trn("ctx", "singular", "plural", n)   (launcher/language.h)
    QCoreApplication::translate("ctx", "text")   QT_TRANSLATE_NOOP("ctx", "text")
    lang::tr("ctx", "text")       lang::trn("ctx", "singular", "plural", n)       (runtime/lang/lang.h)
    HK_N("ctx", "text")           a text in a table, translated where it is drawn
    HK_NP("ctx", "singular", "plural")   a plural's two forms, handed to a function that takes them
    #define M(text) HK_N("ctx", text)   then M("text"): a file's own short marker
A comment "// TR: ..." on the lines just before (or at the end of the line) goes to the translators.

    tools/lang.py               write lang/hiraku.pot and bring every lang/*.po up to date
    tools/lang.py --check       change nothing; status 1 if any of them isn't what the sources make
                                (the unit tests' lang_extracted: a text marked without this run)
    tools/lang.py --stats       each language: translated, fuzzy, untranslated
    tools/lang.py --new CODE    start lang/CODE.po (fr, de, es, it, pt_PT, ja, ko, zh_CN, zh_TW)
"""
import difflib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG = os.path.join(ROOT, "lang")
SOURCES = ["launcher", "runtime"]
SUFFIXES = (".cpp", ".h", ".c")

# each language's header: its name in gettext's terms and its plural rule (gettext's and CLDR's)
LANGUAGES = {
    "fr": ("French", "nplurals=2; plural=(n > 1);"),
    "de": ("German", "nplurals=2; plural=(n != 1);"),
    "es": ("Spanish", "nplurals=2; plural=(n != 1);"),
    "it": ("Italian", "nplurals=2; plural=(n != 1);"),
    "pt_PT": ("Portuguese (Portugal)", "nplurals=2; plural=(n != 1);"),
    "ja": ("Japanese", "nplurals=1; plural=0;"),
    "ko": ("Korean", "nplurals=1; plural=0;"),
    "zh_CN": ("Chinese (simplified)", "nplurals=1; plural=0;"),
    "zh_TW": ("Chinese (traditional)", "nplurals=1; plural=0;"),
}

STRING = r'(?:"(?:[^"\\\n]|\\.)*"\s*)+'   # one literal, or several side by side ("a" "b")
CALLS = [
    # (pattern, kind): kind says what the groups are
    (re.compile(r'\b(?:language|lang|hk::lang)::trn\(\s*(' + STRING + r'),\s*(' + STRING + r'),\s*(' + STRING + r')'), "ctx-plural"),
    (re.compile(r'\b(?:language|lang|hk::lang)::tr\(\s*(' + STRING + r'),\s*(' + STRING + r')\)'), "ctx-text"),
    (re.compile(r'\bQCoreApplication::translate\(\s*(' + STRING + r'),\s*(' + STRING + r')'), "ctx-text"),
    (re.compile(r'\bQT_TRANSLATE_NOOP\(\s*(' + STRING + r'),\s*(' + STRING + r')\)'), "ctx-text"),
    (re.compile(r'\bHK_NP\(\s*(' + STRING + r'),\s*(' + STRING + r'),\s*(' + STRING + r')\)'), "ctx-plural"),
    (re.compile(r'\bHK_N\(\s*(' + STRING + r'),\s*(' + STRING + r')\)'), "ctx-text"),
    (re.compile(r'\b([A-Z]\w*)::tr\(\s*(' + STRING + r')(?:,\s*(' + STRING + r'|nullptr))?(?:,\s*([^)]+))?\)'), "class-tr"),
    (re.compile(r'(?<![\w:.>])tr\(\s*(' + STRING + r')(?:,\s*(' + STRING + r'|nullptr))?(?:,\s*([^)]+))?\)'), "tr"),
]
DEFINE = re.compile(r'^#define\s+(\w+)\((\w+)\)\s+HK_N\(\s*("(?:[^"\\]|\\.)*")\s*,\s*\2\s*\)', re.M)
# a member function's definition at the line's start: "QString Class::name(" / "Class::Class(" (not
# indented: an indented "QMessageBox::warning(this, tr(..." broken over lines is a call, not a definition)
MEMBER = re.compile(r'^(?![ \t])(?:[\w:<>,*&\s]+?\s+[*&]*)?(\w+)::~?\w+\s*\([^;]*$', re.M)
COMMENT = re.compile(r'//\s*TR:\s*(.*)$')


ESCAPES = {"n": b"\n", "t": b"\t", "r": b"\r", '"': b'"', "\\": b"\\", "'": b"'", "a": b"\a", "?": b"?"}


def unescape(lit):
    """the text of one or more C string literals side by side (UTF-8)"""
    out = bytearray()
    for piece in re.findall(r'"((?:[^"\\\n]|\\.)*)"', lit):
        i = 0
        raw = piece.encode("utf-8")
        while i < len(raw):
            c = raw[i]
            if c != 0x5c:
                out.append(c)
                i += 1
                continue
            e = chr(raw[i + 1])
            if e == "x":
                m = re.match(rb'[0-9a-fA-F]+', raw[i + 2:])
                out.append(int(m.group(0), 16) & 0xff)
                i += 2 + len(m.group(0))
                continue
            if e in "01234567":
                m = re.match(rb'[0-7]{1,3}', raw[i + 1:])
                out.append(int(m.group(0), 8) & 0xff)
                i += 1 + len(m.group(0))
                continue
            out += ESCAPES[e]
            i += 2
    return out.decode("utf-8")


def strip_comments(text):
    """the source with its comments blanked (same length and lines), "// TR:" kept"""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            piece = text[i:j]
            out.append(piece if COMMENT.match(piece) else " " * len(piece))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


class Entry:
    def __init__(self, ctx, msgid, plural=None):
        self.ctx, self.msgid, self.plural = ctx, msgid, plural
        self.refs, self.notes, self.flags = [], [], set()
        self.msgstr = []          # a .po's: [text] or [form0, form1, ..]
        self.comments = []        # a translator's own ("# ...")
        self.previous = None      # (ctx, msgid) a fuzzy one was made from
        self.obsolete = False

    def key(self):
        return (self.ctx, self.msgid)


def extract():
    """every marked text in the sources: {(ctx, msgid): Entry}, in the order first met"""
    found = {}
    files = []
    for top in SOURCES:
        for d, dirs, names in os.walk(os.path.join(ROOT, top)):
            dirs.sort()
            files += [os.path.join(d, f) for f in sorted(names) if f.endswith(SUFFIXES)]
    problems = []
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if not re.search(r'\btr\(|::tr\(|::trn\(|translate\(|TRANSLATE_NOOP|HK_NP?\(', text):
            continue
        rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
        code = strip_comments(text)
        calls = list(CALLS)
        for name, _, ctx in DEFINE.findall(code):
            calls.append((re.compile(r'(?<![\w:.>])' + name + r'\(\s*(' + STRING + r')\)'), "fixed:" + unescape(ctx)))
        members = [(m.start(), m.group(1)) for m in MEMBER.finditer(code)]
        lines = code.split("\n")
        taken = []   # spans already matched (a longer pattern first)
        for pattern, kind in calls:
            for m in pattern.finditer(code):
                if any(a <= m.start() < b for a, b in taken):
                    continue
                if kind == "tr" and rel.startswith("runtime/"):
                    continue   # (the game side has no classes' tr: lang::tr with its context)
                taken.append((m.start(), m.end()))
                line = code.count("\n", 0, m.start()) + 1
                plural = None
                if kind == "ctx-plural":
                    ctx, msgid, plural = unescape(m.group(1)), unescape(m.group(2)), unescape(m.group(3))
                elif kind == "ctx-text":
                    ctx, msgid = unescape(m.group(1)), unescape(m.group(2))
                elif kind.startswith("fixed:"):
                    ctx, msgid = kind[6:], unescape(m.group(1))
                else:
                    if kind == "class-tr":
                        cls, lit, dis, n = m.group(1), m.group(2), m.group(3), m.group(4)
                        if cls in ("language", "lang", "QObject"):
                            continue
                    else:
                        lit, dis, n = m.group(1), m.group(2), m.group(3)
                        cls = next((c for at, c in reversed(members) if at < m.start()), None)
                        if not cls:
                            problems.append("%s:%d: tr() outside a class: language::tr(\"context\", ...)" % (rel, line))
                            continue
                    ctx, msgid = cls, unescape(lit)
                    if dis and dis.strip() != "nullptr":
                        ctx += "|" + unescape(dis)
                    if n:
                        plural = msgid   # (Qt's own plural: one English for both)
                e = found.get((ctx, msgid))
                if not e:
                    e = found[(ctx, msgid)] = Entry(ctx, msgid, plural)
                elif plural and not e.plural:
                    e.plural = plural
                # the file only, not its line (gettext's --add-location=file): an edit elsewhere in
                # the file then changes no catalogue, and --check stays quiet until a text changes
                if rel not in e.refs:
                    e.refs.append(rel)
                # the translators' note: "// TR:" on this line or the lines just before
                k = line - 1
                note = COMMENT.search(lines[k]) if k < len(lines) else None
                before = []
                while k - 1 >= 0 and COMMENT.search(lines[k - 1]):
                    before.insert(0, COMMENT.search(lines[k - 1]).group(1).strip())
                    k -= 1
                for t in before + ([note.group(1).strip()] if note else []):
                    if t and t not in e.notes:
                        e.notes.append(t)
                if re.search(r'%[1-9]', msgid + (plural or "")) or plural:
                    e.flags.add("qt-plural-format" if plural else "qt-format")
    # Qt's own texts that the launcher shows (its standard buttons, a text field's menu, a message
    # box's details): answered from the same catalogue, so a Qt without its own translations (the
    # static Windows build has none) shows them in the language too
    for ctx, texts in QT_TEXTS:
        for msgid in texts:
            e = found.setdefault((ctx, msgid), Entry(ctx, msgid))
            e.notes = ["Qt's own standard text; & marks the letter for Alt (Japanese and Chinese: \"(&Y)\" after the word)"]
    return found, problems


QT_TEXTS = [
    ("QPlatformTheme", ["OK", "Cancel", "Close", "Apply", "&Yes", "&No", "Yes to &All", "N&o to All", "Save", "Save All", "Open",
                        "Discard", "Reset", "Restore Defaults", "Help", "Abort", "Retry", "Ignore"]),
    ("QLineEdit", ["&Undo", "&Redo", "Cu&t", "&Copy", "&Paste", "Delete", "Select All"]),
    ("QWidgetTextControl", ["&Undo", "&Redo", "Cu&t", "&Copy", "&Paste", "Delete", "Select All", "Copy &Link Location"]),
    ("QMessageBox", ["Show Details...", "Hide Details..."]),
]


# --- .po files --------------------------------------------------------------------------------
def po_quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\r", "\\r")
    parts = s.split("\n")
    pieces = [p + "\\n" for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
    if len(pieces) <= 1:
        return '"%s"' % (pieces[0] if pieces else "")
    return '""\n' + "\n".join('"%s"' % p for p in pieces)


def po_unquote(s):
    return unescape(s)


def read_po(path):
    """(header text, [Entry]) of a .po file"""
    entries, header = [], ""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    for block in re.split(r"\n\s*\n", text.strip("\n")):
        e = Entry(None, None)
        field, values = None, {}
        obsolete = False
        for line in block.split("\n"):
            if line.startswith("#~"):
                obsolete = True
                line = line[2:].lstrip()
                if line.startswith("|"):
                    continue
            if line.startswith("#,"):
                e.flags |= {f.strip() for f in line[2:].split(",") if f.strip()}
            elif line.startswith("#|"):
                m = re.match(r'#\|\s*(msgctxt|msgid)\s+(".*")$', line)
                if m:
                    prev = e.previous or (None, None)
                    e.previous = (po_unquote(m.group(2)), prev[1]) if m.group(1) == "msgctxt" else (prev[0], po_unquote(m.group(2)))
            elif line.startswith("#.") or line.startswith("#:"):
                pass   # (made again from the sources)
            elif line.startswith("#"):
                e.comments.append(line[1:].strip())
            elif line.startswith('"'):
                values[field] += po_unquote(line)
            else:
                m = re.match(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(".*")$', line)
                if not m:
                    raise SystemExit("%s: a line this tool can't read: %s" % (path, line))
                field = m.group(1)
                values[field] = po_unquote(m.group(2))
        if "msgid" not in values:
            continue
        e.ctx, e.msgid, e.plural = values.get("msgctxt", ""), values["msgid"], values.get("msgid_plural")
        if e.plural is not None:
            k = 0
            while "msgstr[%d]" % k in values:
                e.msgstr.append(values["msgstr[%d]" % k])
                k += 1
        else:
            e.msgstr = [values.get("msgstr", "")]
        e.obsolete = obsolete
        if e.msgid == "" and "msgctxt" not in values:
            header = e.msgstr[0]
            continue
        entries.append(e)
    return header, entries


def write_entry(e, out, template=False):
    pre = "#~ " if e.obsolete else ""
    for c in e.comments:
        out.append(("# " + c).rstrip())
    for n in e.notes:
        out.append("#. " + n)
    if e.refs and not e.obsolete:
        out.append("#: " + " ".join(e.refs))
    if e.flags:
        out.append("#, " + ", ".join(sorted(e.flags)))
    if e.previous and "fuzzy" in e.flags:
        if e.previous[0]:
            out.append("#| msgctxt " + po_quote(e.previous[0]).replace("\n", "\n#| "))
        out.append("#| msgid " + po_quote(e.previous[1]).replace("\n", "\n#| "))
    body = []
    if e.ctx is not None:
        body.append("msgctxt " + po_quote(e.ctx))
    body.append("msgid " + po_quote(e.msgid))
    if e.plural is not None:
        body.append("msgid_plural " + po_quote(e.plural))
        forms = e.msgstr if e.msgstr and not template else ["", ""]
        for k, s in enumerate(forms):
            body.append("msgstr[%d] %s" % (k, po_quote(s)))
    else:
        body.append("msgstr " + po_quote(e.msgstr[0] if e.msgstr and not template else ""))
    for b in body:
        out.append("\n".join(pre + x for x in b.split("\n")))
    out.append("")


POT_HEADER = ("Project-Id-Version: HIRAKU\\n\n"
              "Report-Msgid-Bugs-To: https://github.com/Roganis/HIRAKU/issues\\n\n"
              "MIME-Version: 1.0\\n\n"
              "Content-Type: text/plain; charset=UTF-8\\n\n"
              "Content-Transfer-Encoding: 8bit\\n\n")


def make_pot(found):
    out = ["# The texts of HIRAKU's interface, to translate: made by tools/lang.py from the sources",
           "# (never edited by hand). A language's own file is lang/<code>.po: docs in runtime/lang/lang.h.",
           "# SPDX-License-Identifier: GPL-3.0-or-later",
           'msgid ""', 'msgstr ""']
    out += ['"%s"' % line for line in POT_HEADER.split("\n") if line]
    out.append("")
    for e in found.values():
        write_entry(e, out, template=True)
    return "\n".join(out)


def merge(path, found, code=None):
    """the .po at path brought up to the sources' texts: its new text"""
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            top = []
            for line in f:
                if not line.startswith("#"):
                    break
                top.append(line.rstrip("\n"))
        header, old = read_po(path)
    else:
        name, rule = LANGUAGES[code]
        top = ["# HIRAKU's interface in %s." % name,
               "# SPDX-License-Identifier: GPL-3.0-or-later"]
        header = ("Project-Id-Version: HIRAKU\nLanguage: %s\nMIME-Version: 1.0\nContent-Type: text/plain; charset=UTF-8\n"
                  "Content-Transfer-Encoding: 8bit\nPlural-Forms: %s\n" % (code, rule))
        old = []
    nplurals = int(re.search(r"nplurals=(\d+)", header).group(1)) if re.search(r"nplurals=(\d+)", header) else 2
    by_key = {e.key(): e for e in old}
    used = set()
    out = top + ['msgid ""', 'msgstr ' + po_quote(header).replace('""\n', '""\n', 1)]
    out.append("")
    for k, src in found.items():
        e = Entry(src.ctx, src.msgid, src.plural)
        e.refs, e.notes, e.flags = list(src.refs), list(src.notes), set(src.flags)
        was = by_key.get(k)
        if was and was.key() not in used:
            used.add(was.key())
            e.comments, e.msgstr, e.previous = was.comments, was.msgstr, was.previous
            if "fuzzy" in was.flags:
                e.flags.add("fuzzy")
            if was.obsolete:   # (back in the sources)
                pass
        else:
            # a text whose English changed a little: its old translation, fuzzy (not used)
            near, best = None, 0.0
            for o in old:
                if o.key() in used or o.ctx != src.ctx or o.key() in found or not any(o.msgstr):
                    continue
                r = difflib.SequenceMatcher(None, o.msgid, src.msgid).ratio()
                if r > best:
                    near, best = o, r
            if near and best >= 0.75 and (near.plural is None) == (src.plural is None):
                used.add(near.key())
                e.msgstr, e.comments = list(near.msgstr), near.comments
                e.flags.add("fuzzy")
                e.previous = (near.ctx, near.msgid)
            else:
                e.msgstr = [""] * (nplurals if src.plural is not None else 1)
        if src.plural is not None and len(e.msgstr) != nplurals:
            e.msgstr = (e.msgstr + [""] * nplurals)[:nplurals]
        write_entry(e, out)
    for o in old:   # no longer in the sources: kept, obsolete
        if o.key() not in used and o.key() not in found and any(o.msgstr):
            o.obsolete, o.refs, o.notes = True, [], []
            o.flags.discard("qt-format"), o.flags.discard("qt-plural-format")
            write_entry(o, out)
    return "\n".join(out)


def stats():
    for name in sorted(os.listdir(LANG)):
        if not name.endswith(".po"):
            continue
        _, entries = read_po(os.path.join(LANG, name))
        live = [e for e in entries if not e.obsolete]
        done = sum(1 for e in live if all(e.msgstr) and "fuzzy" not in e.flags)
        fuzzy = sum(1 for e in live if "fuzzy" in e.flags)
        print("%-10s %4d translated, %3d fuzzy, %4d untranslated, %3d obsolete" %
              (name[:-3], done, fuzzy, len(live) - done - fuzzy, len(entries) - len(live)))


def main(argv):
    check = "--check" in argv
    if "--stats" in argv:
        return stats()
    found, problems = extract()
    for p in problems:
        print(p, file=sys.stderr)
    want = {os.path.join(LANG, "hiraku.pot"): make_pot(found)}
    codes = [n[:-3] for n in sorted(os.listdir(LANG)) if n.endswith(".po")] if os.path.isdir(LANG) else []
    if "--new" in argv:
        code = argv[argv.index("--new") + 1]
        if code not in LANGUAGES:
            raise SystemExit("lang.py: %s isn't one of %s" % (code, ", ".join(LANGUAGES)))
        codes.append(code)
    for code in codes:
        want[os.path.join(LANG, code + ".po")] = merge(os.path.join(LANG, code + ".po"), found, code)
    stale = []
    for path, text in want.items():
        have = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if have != text + "\n":
            stale.append(os.path.relpath(path, ROOT))
            if not check:
                os.makedirs(LANG, exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text + "\n")
    if check:
        if stale or problems:
            print("lang: not as the sources make them: %s (run tools/lang.py)" % ", ".join(stale or ["(marks)"]), file=sys.stderr)
            return 1
        print("lang: %d texts, the catalogues up to date" % len(found))
        return 0
    print("lang: %d texts; %s" % (len(found), ", ".join(stale) + " written" if stale else "nothing changed"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
