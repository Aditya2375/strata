#!/usr/bin/env python3
# differential model test: random op sequences against strata vs a python
# model; compares a deterministic full-tree dump after every 25 ops.
import subprocess, random, sys, posixpath

BIN = sys.argv[1]

def fnv(s):
    h = 1469598103934665603
    for ch in s.encode():
        h ^= ch; h = (h * 1099511628211) % (1 << 64)
    return h

class M:
    def __init__(self):
        self.t = {'/': ('d', None), '/lost+found': ('d', None)}
        self.data = {}
        self.next_ino = 1
    def norm(self, p): return posixpath.normpath(p)
    def write(self, p, data, append):
        p = self.norm(p); d = posixpath.dirname(p)
        if d not in self.t or self.t[d][0] != 'd': return
        if p in self.t and self.t[p][0] == 'd': return
        if p in self.t:
            ino = self.t[p][1]
            self.data[ino] = (self.data.get(ino, '') if append else '') + data
        else:
            ino = self.next_ino; self.next_ino += 1
            self.data[ino] = data; self.t[p] = ('f', ino)
    def mkdir(self, p):
        p = self.norm(p); d = posixpath.dirname(p)
        if p in self.t or d not in self.t or self.t[d][0] != 'd': return
        self.t[p] = ('d', None)
    def rm(self, p):
        p = self.norm(p)
        if p not in self.t or self.t[p][0] != 'f': return
        ino = self.t[p][1]; del self.t[p]
        if not any(self.t[q][0] == 'f' and self.t[q][1] == ino for q in self.t):
            self.data.pop(ino, None)
    def rmdir(self, p):
        p = self.norm(p)
        if p not in self.t or self.t[p][0] != 'd' or p == '/': return
        if any(q != p and q.startswith(p + '/') for q in self.t): return
        del self.t[p]
    def mv(self, a, b):
        a, b = self.norm(a), self.norm(b)
        if a not in self.t: return
        if a == b: return
        if a.startswith(b + '/') or b.startswith(a + '/'): return
        if b in self.t and self.t[a][0] == 'f' and self.t[b][0] == 'f' and self.t[a][1] == self.t[b][1]:
            return  # rename(2): hard links to the same file - success, no-op
        if b in self.t:
            if self.t[b][0] == 'd':
                if any(q.startswith(b + '/') for q in self.t if q != b): return
                for q in [q for q in self.t if q.startswith(b + '/')]:
                    if m.t[q][0] == 'f':
                        ino = m.t[q][1]
                        del m.t[q]
                        if not any(r in m.t and m.t[r][0] == 'f' and m.t[r][1] == ino for r in m.t):
                            m.data.pop(ino, None)
                    else: del m.t[q]
            else:
                ino = self.t[b][1]; del self.t[b]
                if not any(self.t[q][0] == 'f' and self.t[q][1] == ino for q in self.t):
                    self.data.pop(ino, None)
        bd = posixpath.dirname(b)
        if bd not in self.t or self.t[bd][0] != 'd': return
        moves = {q: self.t[q] for q in self.t if q == a or q.startswith(a + '/')}
        for q in moves: del self.t[q]
        for q, v in moves.items(): self.t[b + q[len(a):]] = v
    def ln(self, a, b):
        a, b = self.norm(a), self.norm(b)
        if a not in self.t or self.t[a][0] != 'f' or b in self.t: return
        bd = posixpath.dirname(b)
        if bd not in self.t or self.t[bd][0] != 'd': return
        self.t[b] = ('f', self.t[a][1])
    def dump(self):
        out = []
        for p in sorted(self.t):
            if p == '/': continue
            if self.t[p][0] == 'd': out.append(f'D {p}')
            else:
                d = self.data.get(self.t[p][1], '')
                out.append(f'F {p} {len(d)} {fnv(d):016x}')
        # sort like the shell's recursive walk: by full path string
        return sorted(out)

fails = 0
for seed in range(8):
    random.seed(4242 + seed)
    img = f'/tmp/model{seed}.img'
    m = M()
    names = ['a', 'b', 'c', 'x', 'y', 'notes', 'tmp']
    def randpath(): return '/' + '/'.join(random.choice(names) for _ in range(random.randint(1, 3)))
    ops = []
    for step in range(600):
        op = random.choice(['write', 'append', 'mkdir', 'rm', 'rmdir', 'mv', 'ln'])
        if op in ('write', 'append'): ops.append((op, randpath(), f'd{seed}-{step}-' + 'z' * random.randint(0, 30)))
        elif op == 'mkdir': ops.append((op, randpath()))
        elif op == 'rm': ops.append((op, randpath()))
        elif op == 'rmdir': ops.append((op, randpath()))
        elif op == 'mv': ops.append((op, randpath(), randpath()))
        else: ops.append((op, randpath(), randpath()))
    for chunk in range(0, 600, 25):
        lines = []
        for op in ops[chunk:chunk+25]:
            if op[0] in ('write', 'append'): m.write(op[1], op[2], op[0] == 'append'); lines.append(f'{op[0]} {op[1]} {op[2]}')
            elif op[0] == 'mkdir': m.mkdir(op[1]); lines.append(f'mkdir {op[1]}')
            elif op[0] == 'rm': m.rm(op[1]); lines.append(f'rm {op[1]}')
            elif op[0] == 'rmdir': m.rmdir(op[1]); lines.append(f'rmdir {op[1]}')
            elif op[0] == 'mv': m.mv(op[1], op[2]); lines.append(f'mv {op[1]} {op[2]}')
            else: m.ln(op[1], op[2]); lines.append(f'ln {op[1]} {op[2]}')
        subprocess.run([BIN, 'format', img, '2048', '512'], capture_output=True)
        # replay ALL ops so far, then dump
        all_lines = []
        mm = M()
        for op in ops[:chunk+25]:
            if op[0] in ('write', 'append'): all_lines.append(f'{op[0]} {op[1]} {op[2]}')
            elif op[0] == 'mkdir': all_lines.append(f'mkdir {op[1]}')
            elif op[0] == 'rm': all_lines.append(f'rm {op[1]}')
            elif op[0] == 'rmdir': all_lines.append(f'rmdir {op[1]}')
            elif op[0] == 'mv': all_lines.append(f'mv {op[1]} {op[2]}')
            else: all_lines.append(f'ln {op[1]} {op[2]}')
        all_lines.append('dump')
        out = subprocess.run([BIN, 'shell', img], input=('\n'.join(all_lines) + '\n').encode(), capture_output=True)
        if out.returncode != 0:
            print(f'FAIL seed={seed} chunk={chunk}: shell crashed rc={out.returncode}')
            fails = 1; break
        got = sorted(l for l in out.stdout.decode().splitlines() if l.startswith('F ') or l.startswith('D '))
        want = m.dump()
        if got != want:
            print(f'FAIL seed={seed} after {chunk+25} ops')
            for a, b in zip(want + ['<none>'] * 5, got + ['<none>'] * 5):
                if a != b: print(f'  want {a[:70]}\n  got  {b[:70]}')
            fails = 1; break
    if fails: break
    print(f'model: seed {seed} ok (600 ops, 24 dump checks)')
sys.exit(fails)
