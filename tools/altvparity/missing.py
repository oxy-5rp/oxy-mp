import re, sys, os, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract import extract

SKIP_CLASSES = set()

def load_impl(paths):
    txt = []
    for p in paths:
        for f in glob.glob(p):
            txt.append(open(f, encoding='utf-8', errors='replace').read())
    return '\n'.join(txt)

def impl_idents(src):
    return set(re.findall(r'[A-Za-z_][A-Za-z0-9_]*', src))

def stubbed(src, name):
    # name declared and immediately routed to absent()/unperformed()
    pats = [
        rf'\b{name}\s*[:=]\s*absent\(', rf'\b{name}\s*[:=]\s*unperformed\(',
        rf'{name}\s*\(\s*\)\s*\{{[^}}]*absent', rf"absent\('[^']*\b{name}\b",
        rf'absent\("[^"]*\b{name}\b', rf"unperformed\('[^']*\b{name}\b",
        rf'unperformed\("[^"]*\b{name}\b', rf"warnOnce\('[^']*\b{name}\b",
    ]
    for p in pats:
        if re.search(p, src):
            return True
    return False

def run(dts, impl_paths, label, extra_skip=()):
    api = extract(dts)
    src = load_impl(impl_paths)
    idents = impl_idents(src)
    print(f"##### {label}")
    total_missing = 0
    for cls in sorted(api):
        if api[cls]['kind'] == 'interface' and cls.startswith('ICustom'):
            continue
        if cls in extra_skip:
            continue
        mems = api[cls]['members']
        if not mems: continue
        missing = sorted(m for m in mems if m not in idents)
        stubs = sorted(m for m in mems if m in idents and stubbed(src, m))
        if missing or stubs:
            print(f"\n[{api[cls]['kind']} {cls}]  всего {len(mems)}, нет {len(missing)}, заглушка {len(stubs)}")
            if missing:
                print("  НЕТ:    " + ", ".join(missing))
            if stubs:
                print("  ЗАГЛУШ: " + ", ".join(stubs))
        total_missing += len(missing)
    print(f"\n=== {label}: не найдено вовсе {total_missing} имён")

if __name__ == '__main__':
    run(sys.argv[1], sys.argv[2:], os.path.basename(os.path.dirname(sys.argv[1])))
