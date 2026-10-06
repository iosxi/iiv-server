"""実画面(DXGI)を取り込む iiv-server を、決まった動く窓(srcwin.py)で測る。

    python tools/realbench.py [--scene office|video|idle] [--seconds 10] [--port 5913]

サーバーは -dryrun と listen=127.0.0.1(パスワードなし)の ini で、この道具が起動して止める。
受け手は iivcheck.Conn(復号しない。受け取ったら返事を返す)。遅れは、フレームに付いて来る
「画面に出た時刻」(DXGI の LastPresentTime。QPC)から受け取るまで。サーバーの CPU も測る。
iivnc の compare.py と同じ窓・同じ時間で比べられる(compare.py の遅れは窓に描いた時刻から)。
"""
import argparse, ctypes, os, socket, subprocess, sys, time
from ctypes import wintypes as W

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import iivcheck  # noqa: E402

EXE = os.path.join(ROOT, 'iiv-server.exe')
TEST = os.path.join(ROOT, 'build', 'test')
SRCWIN = os.path.join(ROOT, '..', 'iivnc-server', 'tools', 'srcwin.py')


def cpu_seconds(pid):
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)
    if not h:
        return None
    c, e, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
    k.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    k.CloseHandle(h)
    f = lambda t: (t.dwHighDateTime << 32 | t.dwLowDateTime) / 1e7
    return f(kt) + f(ut)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', default='office', choices=['office', 'video', 'idle'])
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--port', type=int, default=5913)
    ap.add_argument('--args', default='')
    ap.add_argument('--ini', default='', help='ini に足す行(; 区切り)')
    a = ap.parse_args()
    os.makedirs(TEST, exist_ok=True)
    ini = os.path.join(TEST, 'real.ini')
    with open(ini, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'port={a.port}\nlisten=127.0.0.1\nnotify=0\nlog=1\n' + ''.join(x + '\n' for x in a.ini.split(';') if x))
    srv = subprocess.Popen([EXE, '-ini', ini, '-dryrun'] + a.args.split())
    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', a.port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    out = os.path.join(TEST, f'src-{a.port}.json')
    src = subprocess.Popen([sys.executable, SRCWIN, '--scene', a.scene, '--seconds', str(a.seconds + 4), '--out', out],
                           stdout=subprocess.DEVNULL)
    time.sleep(1.5)
    c = iivcheck.Conn(a.port)
    t0 = time.perf_counter()
    lat, n, b0, c0, tstart = [], 0, None, None, None
    while time.perf_counter() - t0 < a.seconds + 1:
        t, b = c.msg()
        v = c.handle(t, b)
        if not v:
            continue
        no, key, pq, sq, data = v
        c.ack(no)
        now = time.perf_counter()
        if now - t0 < 1:                # 最初の 1 秒(全体のキーフレーム)は数えない
            continue
        if b0 is None:
            b0, c0, tstart = c.bytes, cpu_seconds(srv.pid), now
        n += 1
        if pq and c.config:
            lat.append((now - pq / c.config['qpf']) * 1000)
    dt = time.perf_counter() - tstart
    cpu = (cpu_seconds(srv.pid) - c0) / dt * 100
    c.s.close()
    src.wait()
    subprocess.run([EXE, '-ini', ini, '-exit'])
    lat.sort()
    print(f'iiv {a.scene:6s} [{a.ini}] {n / dt:5.1f} フレーム/秒  {(c.bytes - b0) * 8 / dt / 1e6:6.1f} Mbps  '
          f'遅れ 中央 {lat[len(lat) // 2]:.1f}ms 95% {lat[int(len(lat) * 0.95)]:.1f}ms  サーバーの CPU {cpu:.1f}%  {c.config}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
