"""iiv-server の受け手(検証用)。iiv の手順でつなぎ、映像を受け取って返事を返す。

    python tools/iivcheck.py [--port 5999] [--password s3cret] [--seconds 5] [--out build/test/recv.h264] [--noack]

サーバーを -testsrc(合成した絵)と検証用の ini で起動して使う(start_server / stop_server)。
フレームは復号しない。受け取った H.264(Annex B)は --out に書き、--out.sizes に 1 行 1 フレームの大きさを書く
(tools/mfdec.c で復号できるか確かめられる)。
"""
import argparse, hashlib, hmac, os, socket, struct, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'iiv-server.exe')
TEST = os.path.join(ROOT, 'build', 'test')
INI = os.path.join(TEST, 'test.ini')
PORT = 5999
MAGIC = 0x31564949

S_VIDEO_CONFIG, S_VIDEO, S_CURSOR_SHAPE, S_CURSOR_POS, S_CLIPBOARD, S_FX, S_PING = range(1, 8)
C_ACK, C_KEY, C_MOUSE, C_CLIPBOARD, C_FX, C_KEYFRAME, C_SAS, C_PONG, C_SETTINGS = range(1, 10)


def pw_text(password, iterations=100000, salt=None):
    salt = salt or os.urandom(16)
    key = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, iterations, 32)
    return f'pbkdf2-sha256${iterations}${salt.hex()}${key.hex()}'


def start_server(args=(), password=None, viewpassword=None, extra=''):
    os.makedirs(TEST, exist_ok=True)
    log = os.path.join(TEST, 'test.log')
    if os.path.exists(log):
        os.remove(log)
    with open(INI, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'port={PORT}\nlisten=127.0.0.1\nnotify=0\nlog=1\n{extra}')
        if password:
            f.write(f'password={pw_text(password)}\n')
        if viewpassword:
            f.write(f'viewpassword={pw_text(viewpassword)}\n')
    subprocess.Popen([EXE, '-ini', INI] + list(args))
    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', PORT), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit('サーバーが待ち受けない')


def stop_server():
    subprocess.run([EXE, '-ini', INI, '-exit'])
    time.sleep(0.3)


def server_log():
    try:
        return open(os.path.join(TEST, 'test.log'), encoding='utf-8').read()
    except OSError:
        return ''


class Conn:
    def __init__(self, port=PORT, password=None, host='127.0.0.1', files=False, version=1):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b''
        self.bytes = 0
        self.s.sendall(struct.pack('<IIII', MAGIC, version, 1 << 1, 1 if files else 0))
        ch = self.read(4 + 4 + 4 + 16 + 4 + 16 + 4 + 32)
        magic, ver, auth = struct.unpack('<III', ch[:12])
        assert magic == MAGIC, hex(magic)
        salt, it = ch[12:28], struct.unpack('<I', ch[28:32])[0]
        saltv, itv = ch[32:48], struct.unpack('<I', ch[48:52])[0]
        nonce = ch[52:84]
        self.auth = auth
        if auth == 1:
            def proof(s, i):
                if not i or password is None:
                    return bytes(32)
                key = hashlib.pbkdf2_hmac('sha256', password.encode(), s, i, 32)
                return hmac.new(key, nonce + b'iiv-auth', hashlib.sha256).digest()
            self.s.sendall(proof(salt, it) + proof(saltv, itv))
        w = self.read(10)
        self.result, self.flags, nlen = struct.unpack('<IIH', w)
        self.name = self.read(nlen).decode('utf-8')
        self.config = None
        self.cursor = None
        self.cursor_pos = None
        self.clip = None

    def read(self, n):
        while len(self.buf) < n:
            d = self.s.recv(1 << 20)
            if not d:
                raise EOFError('切れた')
            self.buf += d
            self.bytes += len(d)
        r, self.buf = self.buf[:n], self.buf[n:]
        return r

    def send(self, t, body=b''):
        self.s.sendall(struct.pack('<IB', 1 + len(body), t) + body)

    def msg(self):
        n = struct.unpack('<I', self.read(4))[0]
        b = self.read(n)
        return b[0], b[1:]

    def ack(self, frame, decode_us=0):
        self.send(C_ACK, struct.pack('<II', frame, decode_us))

    def handle(self, t, b):
        """映像以外を覚える。映像なら (番号, キーか, presentQpc, sendQpc, データ) を返す"""
        if t == S_VIDEO_CONFIG:
            codec, vw, vh, dx, dy, dw, dh, qf = struct.unpack('<IHHiiHHq', b[:28])
            self.config = dict(codec=codec, vw=vw, vh=vh, dx=dx, dy=dy, dw=dw, dh=dh, qpf=qf)
        elif t == S_VIDEO:
            no, fl, pq, sq = struct.unpack('<IBqq', b[:21])
            return no, bool(fl & 1), pq, sq, b[21:]
        elif t == S_CURSOR_SHAPE:
            self.cursor = struct.unpack('<HHHHB', b[:9])
        elif t == S_CURSOR_POS:
            self.cursor_pos = struct.unpack('<iiB', b[:9])
        elif t == S_CLIPBOARD:
            self.clip = b.decode('utf-8')
        elif t == S_PING:
            self.send(C_PONG, b[:8])
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=PORT)
    ap.add_argument('--password')
    ap.add_argument('--seconds', type=float, default=5)
    ap.add_argument('--out')
    ap.add_argument('--noack', action='store_true')
    a = ap.parse_args()
    c = Conn(a.port, a.password)
    print(f'結果 {c.result} 名前 {c.name} flags {c.flags}')
    if c.result:
        return 1
    out = open(a.out, 'wb') if a.out else None
    sizes = open(a.out + '.sizes', 'w') if a.out else None
    t0 = time.perf_counter()
    n = keys = 0
    lat = []
    while time.perf_counter() - t0 < a.seconds:
        t, b = c.msg()
        v = c.handle(t, b)
        if not v:
            continue
        no, key, pq, sq, data = v
        n += 1
        keys += key
        now = time.perf_counter()
        if c.config and pq:
            lat.append((now - pq / c.config['qpf']) * 1000)
        if out:
            out.write(data)
            sizes.write(f'{len(data)}\n')
        if not a.noack:
            c.ack(no)
    dt = time.perf_counter() - t0
    lat.sort()
    print(f'設定 {c.config}')
    print(f'{n} フレーム({keys} キー) {n / dt:.1f} fps {c.bytes * 8 / dt / 1e6:.1f} Mbps'
          f'  画面に出てから受け取るまで 中央 {lat[len(lat) // 2] if lat else 0:.1f}ms')
    return 0


if __name__ == '__main__':
    sys.exit(main())
