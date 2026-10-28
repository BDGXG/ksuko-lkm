import os, sys, time, urllib.request, zipfile
sys.path.insert(0, '/root/work')
import ghlib as g
import commit as C
TOKEN = os.environ['GH_TOKEN']

def ret(fn, n=6, w=5):
    for i in range(n):
        try: return fn()
        except Exception as e:
            last = e; time.sleep(w)
    raise last

msg = '''feat(drm_guard): aidcopy dry-run 探针（binder_alloc_copy_user_to_buffer）

旧 aid 段改的是 binder_ioctl 返回后的用户态 PROT_READ vma，真机 aid_written 恒 0。
新增 AC 探针：kprobe binder_alloc_copy_user_to_buffer 入口，regs[3]/regs[4] 即发送方
parcel 的用户地址与长度，copy_from_user_nofault 只读拷入 4KB 静态缓冲，扫 UTF-16
\"value\"/\"android_id\" key 后跟 [len=16][16 位 UTF-16 hex]，命中则记 alloc->pid 并反查
cred->uid 判断是否白名单。全程不写任何字节（dry-run）。

proc 侧：读 aidcopy_enable/ac_armed/ac_calls/ac_scan/ac_key/ac_hit/ac_uid_hit/ac_big/
ac_bad/ac_nosym/ac_last*；写 aidcopy 1|0；reset 清 AC 计数器；init 调 ac_init()
（BTF 解析 binder_alloc.pid 与 cred.uid 偏移 + kallsyms 取 find_get_task_by_vpid/
get_task_cred），exit disarm。
'''
files = [('kernel/drm_guard.c', '/root/work/cur/drm_guard_v2.c', '100644')]
ret(lambda: C.commit(msg, files))
print('committed')
ret(lambda: g.req('/repos/BDGXG/ksuko-lkm/actions/workflows/393000070/dispatches', 'POST', {'ref': 'dim-module'}))
print('dispatched')
time.sleep(10)
run = None
for i in range(30):
    d = ret(lambda: g.req('/repos/BDGXG/ksuko-lkm/actions/runs?per_page=2'))
    r = d['workflow_runs'][0]
    run = r['id']
    print(i, run, r['status'], r['conclusion'], flush=True)
    if r['status'] == 'completed':
        break
    time.sleep(20)
if r['conclusion'] != 'success':
    print('BUILD FAIL'); sys.exit(1)
a = ret(lambda: g.req(f'/repos/BDGXG/ksuko-lkm/actions/runs/{run}/artifacts'))
aid = [x['id'] for x in a['artifacts'] if x['name']=='susfs_guard_lkm.ko-android15-6.6'][0]
class R(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        n = super().redirect_request(req, fp, code, msg, headers, newurl)
        if n is not None:
            for h in ('Authorization','authorization'):
                n.headers.pop(h,None); n.unredirected_hdrs.pop(h,None)
        return n
for attempt in range(4):
    try:
        op = urllib.request.build_opener(R)
        rq = urllib.request.Request(f'https://api.github.com/repos/BDGXG/ksuko-lkm/actions/artifacts/{aid}/zip')
        rq.add_header('Authorization','token '+TOKEN); rq.add_header('User-Agent','operit')
        with op.open(rq, timeout=240) as resp, open('/root/work/cur/ko_v2.zip','wb') as f:
            f.write(resp.read())
        with zipfile.ZipFile('/root/work/cur/ko_v2.zip') as z:
            z.extractall('/root/work/cur/ko_v2')
        print('downloaded')
        break
    except Exception as e:
        print('dl retry', e); time.sleep(6)
