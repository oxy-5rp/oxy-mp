import re, sys, json, os

def extract(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    lines = src.split('\n')
    # find class/interface/namespace blocks by brace depth
    out = {}
    i = 0
    decl_re = re.compile(r'^\s*(?:export\s+)?(?:declare\s+)?(?:abstract\s+)?(class|interface|namespace|enum)\s+([A-Za-z0-9_]+)')
    while i < len(lines):
        m = decl_re.match(lines[i])
        if m:
            kind, name = m.group(1), m.group(2)
            # scan block
            depth = 0
            started = False
            members = []
            j = i
            while j < len(lines):
                l = lines[j]
                depth += l.count('{') - l.count('}')
                if '{' in l: started = True
                if j > i:
                    members.append(l)
                if started and depth <= 0:
                    break
                j += 1
            body = '\n'.join(members)
            # member names: prop or method at indent level
            mem = set()
            for mm in re.finditer(r'^\s{4,6}(?:public\s+|private\s+|protected\s+)?(?:static\s+)?(?:readonly\s+)?(?:get\s+|set\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*[\(<:?]', body, re.M):
                mem.add(mm.group(1))
            # enum members
            if kind == 'enum':
                for mm in re.finditer(r'^\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|,|$)', body, re.M):
                    mem.add(mm.group(1))
            out.setdefault(name, {'kind': kind, 'members': set()})
            out[name]['members'] |= mem
            i = j + 1
        else:
            i += 1
    return out

if __name__ == '__main__':
    res = extract(sys.argv[1])
    for name in sorted(res):
        print(f"{res[name]['kind']} {name}: {len(res[name]['members'])}")
