"""Compile un fichier .po (gettext) en .mo, sans dépendance externe (équivalent de msgfmt).

Usage : python scripts/po2mo.py entree.po sortie.mo

Gère msgctxt, les pluriels (msgid_plural / msgstr[n]) et les chaînes sur plusieurs lignes.
Les entrées « fuzzy » (sauf l'en-tête) et les traductions vides sont ignorées, comme msgfmt.
"""
import ast
import struct
import sys


def parse_po(path):
    entries, cur, field, fuzzy = [], {}, None, False

    def complete():
        return 'msgid' in cur and ('msgstr' in cur or 'msgstr_n' in cur)

    def flush():
        nonlocal cur, fuzzy
        if 'msgid' in cur:
            cur['fuzzy'] = fuzzy
            entries.append(cur)
        cur, fuzzy = {}, False

    with open(path, encoding='utf-8') as fh:
        for raw in fh:
            # Le .po français de Bambu contient quelques octets NUL parasites (ex. « l\0'ajuster »).
            line = raw.replace('\x00', '').strip()
            if not line:
                continue
            if line.startswith('#'):
                # Un commentaire après une entrée complète annonce l'entrée suivante.
                if complete():
                    flush()
                if line.startswith('#,') and 'fuzzy' in line:
                    fuzzy = True
                continue
            if line.startswith('"'):
                value = ast.literal_eval(line)
                if field[0] == 'msgstr_n':
                    cur['msgstr_n'][field[1]] += value
                else:
                    cur[field[0]] += value
                continue
            key, _, rest = line.partition(' ')
            value = ast.literal_eval(rest.strip())
            if key in ('msgctxt', 'msgid') and complete():
                flush()
            if key.startswith('msgstr['):
                index = int(key[7:-1])
                cur.setdefault('msgstr_n', {})[index] = value
                field = ('msgstr_n', index)
            else:
                cur[key] = value
                field = (key,)
    flush()
    return entries


def build_catalog(entries):
    catalog = {}
    for e in entries:
        msgid = e['msgid']
        if e['fuzzy'] and msgid != '':
            continue
        if 'msgid_plural' in e:
            forms = [e['msgstr_n'][i] for i in sorted(e.get('msgstr_n', {}))]
            if not any(forms):
                continue
            key, value = msgid + '\0' + e['msgid_plural'], '\0'.join(forms)
        else:
            key, value = msgid, e.get('msgstr', '')
            if not value:
                continue
        if 'msgctxt' in e:
            key = e['msgctxt'] + '\x04' + key
        catalog[key] = value
    return catalog


def write_mo(catalog, path):
    keys = sorted(catalog, key=lambda k: k.encode('utf-8'))
    ids = [k.encode('utf-8') for k in keys]
    strs = [catalog[k].encode('utf-8') for k in keys]
    n = len(keys)
    orig_table = 7 * 4
    trans_table = orig_table + n * 8
    data_start = trans_table + n * 8
    data, orig, trans = b'', [], []
    for s in ids:
        orig.append((len(s), data_start + len(data)))
        data += s + b'\0'
    for s in strs:
        trans.append((len(s), data_start + len(data)))
        data += s + b'\0'
    out = struct.pack('<7I', 0x950412de, 0, n, orig_table, trans_table, 0, 0)
    out += b''.join(struct.pack('<2I', *t) for t in orig)
    out += b''.join(struct.pack('<2I', *t) for t in trans)
    with open(path, 'wb') as fh:
        fh.write(out + data)


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    catalog = build_catalog(parse_po(src))
    write_mo(catalog, dst)
    print(f'{dst} : {len(catalog)} entrées')
