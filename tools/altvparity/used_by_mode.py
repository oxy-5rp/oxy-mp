import re,sys,os,glob
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from extract import extract
api=extract(sys.argv[1])
impl=set()
for pat in sys.argv[3:]:
    for f in glob.glob(pat):
        impl |= set(re.findall(r'[A-Za-z_][A-Za-z0-9_]*', open(f,encoding='utf-8',errors='replace').read()))
# что режим реально трогает
used=set()
for root,_,files in os.walk(sys.argv[2]):
    for fn in files:
        if fn.endswith(('.ts','.cjs','.js')):
            try: t=open(os.path.join(root,fn),encoding='utf-8',errors='replace').read()
            except: continue
            used |= set(re.findall(r'\.([A-Za-z_][A-Za-z0-9_]*)', t))
for cls in ['Player','Vehicle','Entity','Blip','Colshape','Checkpoint','Ped','Object','Marker']:
    if cls not in api: continue
    gap = sorted(m for m in api[cls]['members'] if m not in impl and m in used)
    if gap:
        print(f"\n{cls} — режим зовёт, у нас нет ({len(gap)}):")
        print("  " + ", ".join(gap))
