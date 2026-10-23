import io
import os, sys, json, zipfile, subprocess, re, struct, io
from PIL import Image

D = '/data/local/tmp/.hg_apk'
OUT = '/data/user/0/com.ai.assistance.operit/files/.hg_out/icons'
os.makedirs(OUT, exist_ok=True)
META='/data/user/0/com.ai.assistance.operit/files/.hg_out/meta.json'
os.makedirs('/data/user/0/com.ai.assistance.operit/files/.hg_out', exist_ok=True)

def u16(d,o): return struct.unpack_from('<H',d,o)[0]
def u32(d,o): return struct.unpack_from('<I',d,o)[0]

def parse_sp(d,o):
    hs=u16(d,o+2); cnt=u32(d,o+8); flags=u32(d,o+16); sstart=u32(d,o+20)
    utf8=bool(flags&0x100); base=o+sstart; out=[]
    for i in range(cnt):
        p=base+u32(d,o+hs+4*i)
        if utf8:
            l=d[p]; p+=1
            if l&0x80: p+=1
            l2=d[p]; p+=1
            if l2&0x80:
                l2=((l2&0x7f)<<8)|d[p]; p+=1
            out.append(d[p:p+l2].decode('utf-8','replace'))
        else:
            l=u16(d,p); p+=2
            if l&0x8000:
                l=((l&0x7fff)<<16)|u16(d,p); p+=2
            out.append(d[p:p+l*2].decode('utf-16-le','replace'))
    return out

def parse_axml(d):
    strings=[]; o=8; n=len(d); elems=[]
    while o+8<=n:
        typ=u16(d,o); hs=u16(d,o+2); sz=u32(d,o+4)
        if sz<8: break
        if typ==1: strings=parse_sp(d,o)
        elif typ==0x102:
            name=u32(d,o+20); astart=u16(d,o+24); asize=u16(d,o+26); acnt=u16(d,o+28)
            elem=strings[name] if name<len(strings) else ''
            at={}
            for i in range(acnt):
                a=o+16+astart+i*asize
                an=u32(d,a+4); raw=u32(d,a+8); ty=d[a+15]; da=u32(d,a+16)
                nm=strings[an] if an<len(strings) else ''
                if raw!=0xffffffff and raw<len(strings): at[nm]=('str',strings[raw])
                elif ty==0x01: at[nm]=('ref',da)
            elems.append((elem,at))
        o+=sz
    return elems

def parse_arsc(d, targets):
    o=12; gstr=[]; res={}
    n=len(d)
    while o+8<=n:
        typ=u16(d,o); hs=u16(d,o+2); sz=u32(d,o+4)
        if sz<8: break
        if typ==1: gstr=parse_sp(d,o)
        elif typ==0x200:
            pkgid=u32(d,o+8); p=o+hs; pend=o+sz
            while p+8<=pend:
                t=u16(d,p); ths=u16(d,p+2); tsz=u32(d,p+4)
                if tsz<8: break
                if t==0x0201:
                    tid=d[p+8]; ecnt=u32(d,p+12); estart=u32(d,p+16)
                    for eid in range(ecnt):
                        off=u32(d,p+ths+eid*4)
                        if off==0xffffffff: continue
                        rid=(pkgid<<24)|(tid<<16)|eid
                        if rid not in targets: continue
                        ep=p+estart+off; esz=u16(d,ep); vp=ep+esz
                        ty=d[vp+3]; da=u32(d,vp+4)
                        if ty==0x03:
                            res[rid]=gstr[da] if da<len(gstr) else ''
                p+=tsz
        o+=sz
    return res

def to_png2(bg, fg, dst, size=144):
    try:
        b = Image.open(io.BytesIO(bg)).convert('RGBA').resize((size, size), Image.LANCZOS)
        f = Image.open(io.BytesIO(fg)).convert('RGBA').resize((size, size), Image.LANCZOS)
        out = Image.alpha_composite(b, f)
        if not _ok(out):
            return False
        out.save(dst, 'PNG', optimize=True)
        return True
    except Exception:
        return False

def _ok(im):
    try:
        px = list(im.convert('RGBA').resize((32,32)).getdata())
        n = len(px)
        op = sum(1 for r,g,b,a in px if a > 8) / float(n)
        return op >= 0.5
    except Exception:
        return True

def to_png(data, dst, size=144):
    try:
        im = Image.open(io.BytesIO(data)).convert('RGBA')
        if not _ok(im):
            return False
        im = im.resize((size,size), Image.LANCZOS)
        im.save(dst, 'PNG', optimize=True)
        return True
    except Exception:
        return False

# hgicon2：不拷贝 APK，直接按 pkg<TAB>apkpath 清单原地读取
LIST='/data/local/tmp/.hg_list.tsv'
items=[]
if os.path.exists(LIST):
    for line in io.open(LIST, encoding='utf-8', errors='replace'):
        line=line.rstrip('\n')
        if not line: continue
        p,_,a=line.partition('\t')
        if p and a: items.append((p.strip(), a.strip()))
apps=[]; noicon=[]
for pkg,apk in items:
    label=pkg; icon_path=''
    try:
        out=subprocess.run(['aapt','dump','badging',apk],capture_output=True,timeout=120).stdout.decode('utf-8','replace')
        # aapt 的 locale 后缀是 zh-CN / zh-HK / zh-rCN 这几种，按优先级挑中文
        for _pat in (r"^application-label-zh-CN:'([^']*)'",
                     r"^application-label-zh-rCN:'([^']*)'",
                     r"^application-label-zh:'([^']*)'",
                     r"^application-label-zh[-_][A-Za-z]+:'([^']*)'",
                     r"^application-label:'([^']*)'"):
            m=re.search(_pat, out, re.M)
            if m and m.group(1): label=m.group(1); break
        mi=re.search(r"^application:.*?icon='(.*)'", out, re.M)
        icon_path = mi.group(1).strip() if mi else ''
    except Exception:
        pass
    done=False
    try:
        z=zipfile.ZipFile(apk)
        names=set(z.namelist())
        cands=[]
        if icon_path and icon_path in names: cands.append(icon_path)
        # 自适应图标（res/xxx.xml）：单独 try，解析失败不能连带把后面的
        # 「随便挑一张图」兜底一起跳过（ColorOS 日历/KernelSU 就是这么丢图标的）
        if icon_path and icon_path.lower().endswith('.xml') and icon_path in names:
            try:
                xraw=z.read(icon_path)
                arsc = z.read('resources.arsc') if 'resources.arsc' in names else None
                if arsc:
                    elems=parse_axml(xraw)
                    refs=[]
                    for nm,at in elems:
                        for k,v in at.items():
                            if v[0]=='ref': refs.append(v[1])
                    if refs:
                        m2=parse_arsc(arsc,set(refs))
                        layers=[]
                        for r in refs:
                            v=m2.get(r,'')
                            if v and v in names and v.lower().endswith(('.png','.webp')):
                                layers.append(v)
                        if len(layers) >= 2:
                            try:
                                if to_png2(z.read(layers[0]), z.read(layers[1]),
                                           os.path.join(OUT, pkg + '.png')):
                                    done = True
                            except Exception:
                                done = False
                        if not done:
                            for v in layers:
                                cands.append(v)
            except Exception:
                pass
        raster=[c for c in cands if c.lower().endswith(('.png','.webp')) and c in names]
        pick=None
        if raster:
            def _sz(n):
                try: return z.getinfo(n).file_size
                except Exception: return 0
            fg=[c for c in raster if 'foreground' in c.lower()]
            if fg:
                fg.sort(key=_sz, reverse=True); pick=fg[0]
            else:
                raster.sort(key=_sz, reverse=True); pick=raster[0]
        for cand in ([pick] + [c for c in raster if c != pick]) if pick else []:
            if to_png(z.read(cand), os.path.join(OUT,pkg+'.png')):
                done = True
                break
        if not done:
            # 兜底：优先名字像启动图的（ic_launcher / mipmap），否则取最大的一张
            best=None; bscore=-1
            for i in z.infolist():
                n=i.filename.lower()
                if not n.endswith(('.png','.webp','.jpg','.jpeg')): continue
                if i.file_size>1500000 or i.file_size<500: continue
                sc=i.file_size
                if 'ic_launcher' in n: sc+=3000000
                if '/mipmap' in n: sc+=1000000
                if n.startswith('assets/'): sc-=1000000
                if sc>bscore: bscore=sc; best=i.filename
            if best:
                done=to_png(z.read(best), os.path.join(OUT,pkg+'.png'))
        z.close()
    except Exception as e:
        pass
    apps.append({'pkg':pkg,'label':label,'ok':1 if done else 0})
    if not done: noicon.append(pkg)

meta={a['pkg']:{'label':a['label'],'ok':a['ok']} for a in apps}
json.dump(meta, open(META,'w'), ensure_ascii=False)
print('DONE', len(meta), sum(v['ok'] for v in meta.values()))
with open(os.path.join(os.path.dirname(META),'meta.tsv'),'w') as _f:
    for _p,_v in meta.items():
        _f.write('%s\t%s\t%s\n' % (_p, _v['label'].replace(chr(9),' ').replace(chr(10),' '), _v['ok']))
