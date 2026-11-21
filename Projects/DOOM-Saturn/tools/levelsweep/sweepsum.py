import re,sys,collections
rows=collections.OrderedDict()
for l in open(sys.argv[1]):
    m=re.match(r'\[SWEEP\] (\S+) E(\d)M(\d) (\S+) (\S+) (\S+) \| (.*)',l)
    if not m: 
        if 'INCOMPLETE' in l: print(l.strip())
        continue
    key='E%sM%s'%(m.group(2),m.group(3))
    r=rows.setdefault(key,{'n':0,'err':[],'crash':0,'bad':[]})
    r['n']+=1
    rest=m.group(7)
    if 'REFCRASH' in rest: r['crash']+=1; continue
    a=float(re.search(r'AvgErr:\s*([\d.]+)',rest).group(1))
    g=float(re.search(r'>=1%:\s*([\d.]+)',rest).group(1))
    r['err'].append(a)
    if a>=8: r['bad'].append((a,m.group(1)))
tot=[]
for k,r in rows.items():
    e=r['err']; tot+=e
    print("%s views %3d mean %5.2f max %5.2f  >=8%%: %2d  refcrash %d  worst %s"%(k,r['n'],sum(e)/max(1,len(e)),max(e) if e else 0,len(r['bad']),r['crash'],sorted(r['bad'])[-1][1] if r['bad'] else ''))
print("ALL mean %.3f n %d"%(sum(tot)/len(tot),len(tot)))
