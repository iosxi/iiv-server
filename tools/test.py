"""iiv-server の自動検証(画面は写さない・入力は再現しない)。

    python tools/test.py

サーバーを -testsrc(合成した絵。入力はログに書くだけ)と検証用の ini(127.0.0.1:5999)で起動し、
tools/iivcheck.py の受け手で確かめる。受け取った H.264 は tools/mfdec.c(build/mf/mfdec.exe)で
復号できるか確かめる(無ければ tools/build-tools.bat で作る)。
利用者が普段使っている iiv-server(既定の ini)とは干渉しない(多重起動の判定は ini ごと)。
"""
import os, socket, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import iivcheck as t  # noqa: E402

ROOT = os.path.dirname(HERE)
MFDEC = os.path.join(ROOT, 'build', 'mf', 'mfdec.exe')
TMP = os.path.join(ROOT, 'build', 'test')
ok = True


def check(cond, label, detail=''):
    global ok
    print(f'  {label:44s} {"OK" if cond else "NG"}  {detail}')
    if not cond:
        ok = False


def receive(c, frames, seconds=10, ack=True, out=None):
    """frames 枚(か seconds 秒)受け取る。(フレームの数, キーの数, 設定の変化の数) を返す"""
    n = keys = cfgs = 0
    last_cfg = None
    f = open(out, 'wb') if out else None
    sizes = open(out + '.sizes', 'w') if out else None
    t0 = time.time()
    while n < frames and time.time() - t0 < seconds:
        typ, b = c.msg()
        v = c.handle(typ, b)
        if c.config != last_cfg:
            cfgs += 1
            last_cfg = c.config
        if not v:
            continue
        no, key, pq, sq, data = v
        n += 1
        keys += key
        if f:
            f.write(data)
            sizes.write(f'{len(data)}\n')
        if ack:
            c.ack(no)
    if f:
        f.close()
        sizes.close()
    return n, keys, cfgs


def decodes(path, w, h):
    r = subprocess.run([MFDEC, path, path + '.sizes', 'NUL', str(w), str(h), '0'], capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    n_in = sum(1 for _ in open(path + '.sizes'))
    import re
    m = re.search(r'入れた (\d+) 出た (\d+)', r.stdout)
    return m and int(m[1]) == n_in and int(m[2]) == n_in, r.stdout.strip()[:120]


def main():
    os.makedirs(TMP, exist_ok=True)
    if not os.path.exists(MFDEC):
        subprocess.run(['cmd', '/c', os.path.join(HERE, 'build-tools.bat')], capture_output=True)

    print('[認証]')
    t.start_server(['-testsrc', 'static'], password='s3cret', viewpassword='look')
    try:
        c = t.Conn(password='wrong')
        check(c.result == 1, '違うパスワードは断る', f'結果 {c.result}')
        c = t.Conn(password='s3cret')
        check(c.result == 0 and not (c.flags & 1), '正しいパスワードはつながる(操作できる)', f'結果 {c.result} flags {c.flags}')
        n, keys, cfgs = receive(c, 1)
        check(n == 1 and keys == 1 and c.config, '最初のフレームはキーフレーム', f'{c.config}')
        c.s.close()
        c = t.Conn(password='look')
        check(c.result == 0 and (c.flags & 1), '見るだけのパスワードは見るだけ', f'flags {c.flags}')
        c.s.close()
        c = t.Conn(password=None)
        check(c.result == 1, 'パスワードを送らない相手は断る', f'結果 {c.result}')
        c.s.close()
        # VNC のビューアのような相手(RFB の版を送ってくる)は黙って切る
        s = socket.create_connection(('127.0.0.1', t.PORT), timeout=5)
        s.sendall(b'RFB 003.008\n' + bytes(4))
        try:
            d = s.recv(100)
        except OSError:
            d = b''
        check(d == b'', 'iiv ではない相手(VNC)は黙って切る', repr(d[:20]))
        s.close()
        c = t.Conn(password='s3cret', version=99)
        check(c.result == 3, '版が違う相手は断る(版の違いを知らせる)', f'結果 {c.result}')
    finally:
        t.stop_server()

    print('[動く絵: 300 フレームを受けて、全部復号できるか]')
    for src, label in (([], '文字・四角・写真'), (['video'], '全面が毎フレーム変わる')):
        t.start_server(['-testsrc'] + src + ['-testfps', '0'])
        try:
            c = t.Conn()
            path = os.path.join(TMP, 'recv.h264')
            t0 = time.time()
            n, keys, cfgs = receive(c, 300, out=path)
            dt = time.time() - t0
            good, out = decodes(path, c.config['vw'], c.config['vh'])
            check(n == 300 and keys >= 1 and good, label, f'{n / dt:.0f} フレーム/秒 {c.bytes * 8 / dt / 1e6:.1f} Mbps キー {keys}  {out[:60]}')
        finally:
            t.stop_server()

    print('[止まった絵: 送るものが無ければ送らない]')
    t.start_server(['-testsrc', 'static'])
    try:
        c = t.Conn()
        n, keys, cfgs = receive(c, 1)
        c.s.settimeout(1.5)
        extra = 0
        try:
            while True:
                typ, b = c.msg()
                if c.handle(typ, b):
                    extra += 1
        except (socket.timeout, TimeoutError, OSError):
            pass
        check(extra <= 2, '止まった絵では、キーフレームの後ほとんど送らない', f'その後のフレーム {extra}')
    finally:
        t.stop_server()

    print('[途中で画面の大きさが変わる(100 フレーム目で 1920x1080 → 1280x720)]')
    t.start_server(['-testsrc', '-testfps', '0', '-testresize', '100'])
    try:
        c = t.Conn()
        path = os.path.join(TMP, 'resize.h264')
        n, keys, cfgs = receive(c, 250, out=path)
        check(c.config and c.config['vw'] == 1280 and c.config['vh'] == 720 and cfgs >= 2 and keys >= 2,
              '大きさを知らせ直し、キーフレームから続ける', f'設定 {cfgs} 回、キー {keys}、最後 {c.config["vw"]}x{c.config["vh"]}')
    finally:
        t.stop_server()

    print('[3 人が同時につなぐ]')
    t.start_server(['-testsrc', '-testfps', '60'])
    try:
        cs = [t.Conn() for _ in range(3)]
        res = []
        for c in cs:
            res.append(receive(c, 60, seconds=5))
        check(all(r[0] == 60 and r[1] >= 1 for r in res), '3 人とも受け取れる', str(res))
    finally:
        t.stop_server()

    print('[返事を返さない相手がいても、ほかの相手は止まらない]')
    t.start_server(['-testsrc', '-testfps', '60'])
    try:
        bad = t.Conn()
        receive(bad, 3, ack=False)
        good = t.Conn()
        t0 = time.time()
        n, keys, cfgs = receive(good, 300, seconds=12)
        dt = time.time() - t0
        check(n == 300, '返事の無い相手を待ち続けない', f'{n} フレーム {dt:.1f} 秒')
        bad.s.close()
    finally:
        t.stop_server()

    print('[でたらめなデータを送っても落ちない]')
    t.start_server(['-testsrc', 'static'])
    try:
        import random
        rnd = random.Random(7)
        for i in range(200):
            try:
                s = socket.create_connection(('127.0.0.1', t.PORT), timeout=2)
                if i % 2:
                    s.sendall(struct.pack('<IIII', t.MAGIC, 1, 2, 0))       # 正しい挨拶の後にでたらめ
                    s.recv(84)
                s.sendall(bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 300))))
                s.close()
            except OSError:
                pass
        c = t.Conn()
        n, keys, cfgs = receive(c, 1)
        check(c.result == 0 and n == 1, '200 回のでたらめな接続のあとも動いている')
    finally:
        t.stop_server()

    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
