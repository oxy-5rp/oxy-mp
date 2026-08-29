# -*- coding: utf-8 -*-
"""Сверка числа доводов у методов классов слоя alt:V с объявлениями @altv/types-*.

Тот же приём, что у сверки конструкторов: ошибка здесь молчит — числа законны,
только не те, — и наружу выходит через несколько звеньев.
"""
import io, re, glob, sys

def body(s, i):
    depth = 1
    start = i
    while i < len(s) and depth:
        if s[i] == '{': depth += 1
        elif s[i] == '}': depth -= 1
        i += 1
    return s[start:i]

def split_args(text):
    """Доводы через запятую верхнего уровня.

    Обычным `split(',')` нельзя: `value: InterfaceValueByKey<Meta, K>` — это один
    довод с запятой внутри. Разъехавшись, он раздувал число доводов, и сверка
    начинала пропускать настоящие расхождения: лишний вариант принимал любую
    нашу подпись.
    """
    args, depth, current = [], 0, ''
    for ch in text:
        if ch in '<([{':
            depth += 1
        elif ch in '>)]}':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(current)
            current = ''
            continue
        current += ch
    args.append(current)
    return [a.strip() for a in args if a.strip()]


def theirs(path):
    s = io.open(path, encoding='utf-8', errors='replace').read()
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    out = {}
    for m in re.finditer(r'export class (\w+)[^{]*\{', s):
        t = body(s, m.end())
        methods = {}
        # Обобщённые объявляются как `setMeta<K extends string>(…)`, и без
        # разрешения на угловые скобки сверка их не видела вовсе — а ими
        # объявлена вся семья метаданных.
        for c in re.finditer(
                r'(?:public |private |protected )?(?:static )?(\w+)\s*(?:<[^(]*>)?\s*\(([^)]*)\)\s*:', t):
            name = c.group(1)
            if name in ('constructor', 'if', 'for', 'while', 'return'):
                continue
            args = split_args(c.group(2))
            need = len([a for a in args if '?' not in a.split(':')[0] and not a.startswith('...')])
            methods.setdefault(name, set()).add((need, len(args)))
        out[m.group(1)] = methods
    return out

def ours(patterns):
    out = {}
    for pat in patterns:
        for path in glob.glob(pat):
            s = io.open(path, encoding='utf-8', errors='replace').read()
            for m in re.finditer(r'\bclass (\w+)(?: extends [\w.]+)?\s*\{', s):
                t = body(s, m.end())
                methods = {}
                for c in re.finditer(r'\n\s{4,}(?:static )?(\w+)\s*\(([^)]*)\)\s*\{', t):
                    name = c.group(1)
                    if name in ('constructor', 'if', 'for', 'while', 'switch', 'catch', 'function'):
                        continue
                    # Громкий отказ доводов не читает и читать не должен: он бросает
                    # первой же строкой. Считать это потерей довода значило бы
                    # топить настоящие находки в шуме — таких отказов в слое десятки.
                    if t[c.end():].lstrip().startswith('throw '):
                        continue
                    args = [a.strip() for a in c.group(2).split(',') if a.strip()]
                    need = len([a for a in args if '=' not in a and not a.startswith('...')])
                    methods[name] = (need, len(args), path)
                out.setdefault(m.group(1), {}).update(methods)
    return out

for types, mine, label in [
    ('D:/backend-VRUSSIA/node_modules/@altv/types-client/index.d.ts',
     ['client-js/js/alt_client*.js'], 'CLIENT'),
    ('D:/backend-VRUSSIA/node_modules/@altv/types-server/index.d.ts',
     ['script-js/js/alt_*.js'], 'SERVER'),
]:
    th, my = theirs(types), ours(mine)
    print('==== %s ====' % label)
    found = 0
    for cls, methods in sorted(my.items()):
        if cls not in th:
            continue
        for name, (need, total, path) in sorted(methods.items()):
            if name not in th[cls]:
                continue
            variants = th[cls][name]
            # хотя бы одна перегрузка обязана уместиться: наш total >= их need
            if any(need <= t and total >= n for (n, t) in variants):
                continue
            found += 1
            print('  %s.%s  our(%d..%d)  alt%s  %s'
                  % (cls, name, need, total, sorted(variants), path))
    if not found:
        print('  расхождений нет')
