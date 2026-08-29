# То же сравнение доводов, но для слоя на JavaScript.
#
# Метод, объявленный с меньшим числом доводов, чем у alt:V, теряет остальные
# молча: JavaScript лишних доводов не замечает, и вызов удаётся.
import re, sys, glob

dts = open(sys.argv[1], encoding='utf-8', errors='replace').read()

declared = {}
for m in re.finditer(r'^\s*(?:public\s+)?([A-Za-z_]\w*)\s*\(([^()]*)\)\s*:', dts, re.M):
    name, params = m.group(1), m.group(2).strip()
    if name in ('constructor', 'function', 'if', 'for', 'while', 'switch', 'catch'):
        continue
    count = 0 if params == '' else len([p for p in params.split(',') if p.strip()])
    declared[name] = max(declared.get(name, 0), count)

# Наши: `name(a, b) {` в теле класса или объекта.
ours = {}
for pat in sys.argv[2:]:
    for f in glob.glob(pat):
        text = open(f, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'^\s{4,}(?:(?:get|set|static|async)\s+)?([A-Za-z_]\w*)\s*\(([^()]*)\)\s*\{', text, re.M):
            name, params = m.group(1), m.group(2).strip()
            if name in ('constructor', 'if', 'for', 'while', 'switch', 'catch', 'function'):
                continue
            count = 0 if params == '' else len([p for p in params.split(',') if p.strip()])
            prev = ours.get(name)
            ours[name] = count if prev is None else max(prev, count)

print(f"{'метод':32} alt:V  мы")
short = 0
for name, count in sorted(ours.items()):
    want = declared.get(name)
    if want is None or count >= want:
        continue
    print(f"{name:32} {want:>5}  {count:>3}")
    short += 1
print(f"\nкороче объявленного: {short} из {len(ours)} наших методов")
