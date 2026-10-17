"""[Recomps#1] Register caching for lifted functions: guest registers in C locals instead of the Ctx.

recomp.py's statements read and write every guest register through `c->x[n]` / `c->v[n]` and the flags through
`adds32(c, ...)`, `cond(c, ...)`. Guest memory is accessed through pointers made from integers and `c` is passed to
every callee, so the C compiler must assume that each guest store may change the Ctx: after every one it reloads the
registers it needs (the stack pointer after each push, for example) and it keeps every register write. This pass
rewrites one function's statements so the registers it uses live in locals (R_x<n>, R_v<n>, R_f = NZCV, an RcFlags),
which clang keeps in host registers:

  - entry: every register the function mentions is loaded from the Ctx (loads that end up unused are dropped by clang);
  - before anything that can look at the Ctx (a call to a lifted function or import, call_indirect, v_tbl / v_ldn,
    the jump-table dispatch, return, guest_unimpl): the registers that may have changed since the Ctx was last exact
    are written back. Which ones is a forward data flow over the statements ("dirty" sets, union at joins);
  - after a call: every register the function mentions is reloaded (the callee may change any of them; reloads that
    are overwritten before use are dropped by clang).

Status (finding 91): OFF by default (recomp_eh.py, RECOMPS_REGCACHE=1). At -O2 it measured no clear gain in the lifted
threads (6.4-6.7 vs 6.6-6.7 ms/frame in the scripted tutorial), costs +13 % DLL size and a 4x slower lift. Functions
with a jump table are left alone even when on (their code grows 2.6-3.5x: every label is reachable from the dispatch).
A boot + tutorial run with it on (-O2 and -O1) showed the same pictures and no traps.

So the Ctx is exact at every call, return and trap: everything the runtime, the unwinder, crash reports and other
threads look at. Statements are matched by the forms recomp.py emits; a function with anything unexpected is left as
it was (transform returns None).

    transform(lines, falls_to) -> None or (declaration lines, [(pc, statement)], trailer statement, dispatch reload)

The jump-table dispatch (L_dispatch, which can reach every statement) starts with the reload: the Ctx is exact there
(written back before its goto), so the values a label sees through it do not reach back to the entry loads.
"""
import re

FLAG_WRITERS = ('adds64', 'adds32', 'logic64', 'logic32', 'set_nzcv', 'fcmp')
NO_SYNC = set(FLAG_WRITERS) | {'cond', 'stx8', 'stx16', 'stx32', 'stx64', 'stx128'}   # take c, touch only flags / ex_*

REG = re.compile(r'c->([xv])\[(\d+)\]')
CALL_C = re.compile(r'\b(\w+)\(c([,)])')
FLAG_CALL = re.compile(r'\b(%s|cond)\(c,' % '|'.join(FLAG_WRITERS))
ASSIGN = re.compile(r'\s*(=(?!=)|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|\+\+|--)')
MEMBER = re.compile(r'(\.\w+(\[[^\]]*\])?)*')
GOTO = re.compile(r'goto L_([0-9a-f]+);')
CONTROL = re.compile(r'\b(if|switch|while|for|do)\b|\?')
TERMINAL = re.compile(r'(\breturn;|\bgoto L_\w+;)\s*\}?\s*$')
EXIT = re.compile(r'\breturn;|\bgoto L_dispatch;')
TRAP = re.compile(r'\b(guest_unimpl|guest_trap)\(')
BARE_C = re.compile(r'\bc\b(?!->(fn|ex_addr|ex_val2?|ex_fail)\b)')
FLAGS = 64                                               # bit for NZCV; x<n> = bit n, v<n> = bit 32 + n
ALL = (1 << 65) - 1
X_FLAGS = (1 << 32) - 1 | FLAGS


class Unexpected(Exception):
    pass


def _bit(kind, n):
    return 1 << (n if kind == 'x' else 32 + n)


def _ref_events(stmt, cache):
    """[(position, bit, is_write)] for every register / flags reference; writes over-approximated
    (any &c->v[n] counts as a write unless it is the source of a wrv)"""
    out = []
    for m in REG.finditer(stmt):
        b = _bit(m.group(1), int(m.group(2)))
        rest = stmt[m.end():]
        if m.group(1) == 'v':
            rest = rest[MEMBER.match(rest).end():]
        before = stmt[:m.start()]
        w = bool(ASSIGN.match(rest)) or before.endswith(('++', '--')) or \
            (before.endswith('&') and not re.search(r'\bwrv\([^,()]+, &$', before))
        out.append((m.start(), b, w))
    for m in FLAG_CALL.finditer(stmt):
        out.append((m.start(), FLAGS, m.group(1) != 'cond'))
    out = [r for r in out if r[1] & cache]
    out.sort()
    return out


def _close(s, i):
    """index just past the ')' matching the '(' at s[i]"""
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '(':
            depth += 1
        elif s[j] == ')':
            depth -= 1
            if not depth:
                return j + 1
    return -1


def _sync_events(stmt):
    """sync points in text order: (start, end, kind); 'call' = may read and change the Ctx, 'exit' = reads it
    (return, goto L_dispatch, guest_unimpl)"""
    ev, bare = [], 0
    for m in CALL_C.finditer(stmt):
        bare += 1
        if m.group(1) in NO_SYNC:
            continue
        end = _close(stmt, m.start() + len(m.group(1)))
        if end < 0 or stmt[end:end + 1] != ';':
            raise Unexpected(stmt)                       # a call inside an expression
        ev.append((m.start(), end + 1, 'call'))
    if len(BARE_C.findall(re.sub(REG, '', stmt))) != bare:
        raise Unexpected(stmt)                           # c used some other way (c->N, a pointer to it, ...)
    for m in EXIT.finditer(stmt):
        ev.append((m.start(), m.end(), 'exit'))
    for m in TRAP.finditer(stmt):
        end = _close(stmt, m.end() - 1)
        if end < 0 or stmt[end:end + 1] != ';':
            raise Unexpected(stmt)
        ev.append((m.start(), end + 1, 'exit'))
    ev.sort()
    for a, b in zip(ev, ev[1:]):
        if a[1] > b[0]:
            raise Unexpected(stmt)
    return ev


class Stmt:
    """one statement: its references, sync points, and its transfer function dirty_out = (dirty_in & keep) | gen"""

    def __init__(self, text, cache):
        self.text = text
        self.cache = cache
        self.refs = _ref_events(text, cache)
        self.used = 0
        self.wr = 0
        for _, b, w in self.refs:
            self.used |= b
            if w:
                self.wr |= b
        self.sync = _sync_events(text)
        self.control = bool(CONTROL.search(text))
        calls = [e for e in self.sync if e[2] == 'call']
        if self.control or not calls:
            self.keep, self.gen = ~0, self.wr
        else:                                            # straight line: what is written after the last call
            last = calls[-1][1]
            self.keep, self.gen = 0, 0
            for pos, b, w in self.refs:
                if w and pos >= last:
                    self.gen |= b

    def terminal(self):
        """True when control never falls through to the next statement (an unconditional return / goto at its end)"""
        return not self.control and bool(TERMINAL.search(self.text))

    def rewrite(self, dirty_in, used):
        text = self.text
        if not self.sync:
            return _rename(text, self.cache)
        out, pos, dirty, ri = [], 0, dirty_in, 0
        for a, b, kind in self.sync:
            if self.control:
                d = dirty_in | self.wr                   # under a condition: write back all that may be dirty here
            else:
                while ri < len(self.refs) and self.refs[ri][0] < b:   # writes before the point and in its arguments
                    if self.refs[ri][2]:
                        dirty |= self.refs[ri][1]
                    ri += 1
                d = dirty
            out.append(_rename(text[pos:a], self.cache))
            body = _rename(text[a:b], self.cache)
            out.append('{ %s%s %s}' % (spill(d), body, reload(used)) if kind == 'call' else '{ %s%s }' % (spill(d), body))
            if kind == 'call' and not self.control:
                dirty = 0
            pos = b
        out.append(_rename(text[pos:], self.cache))
        return ''.join(out)


def _names(mask):
    out = [('R_x%d' % n, 'c->x[%d]' % n) for n in range(32) if mask >> n & 1]
    out += [('R_v%d' % n, 'c->v[%d]' % n) for n in range(32) if mask >> (32 + n) & 1]
    if mask & FLAGS:
        out += [('R_f.%s' % f, 'c->%s' % f) for f in 'NZCV']
    return out


def spill(mask):
    return ''.join('%s = %s; ' % (ctx, loc) for loc, ctx in _names(mask))


def reload(mask):
    return ''.join('%s = %s; ' % (loc, ctx) for loc, ctx in _names(mask))


def _rename(text, cache):
    text = REG.sub(lambda m: 'R_%s%s' % m.groups() if _bit(m.group(1), int(m.group(2))) & cache else m.group(0), text)
    return FLAG_CALL.sub(lambda m: '%s_f(&R_f,' % m.group(1), text) if cache & FLAGS else text


def transform(lines, falls_to, cache=ALL):
    """lines = [(pc, statement)] of one function; falls_to = the function the last statement falls into, or None;
    cache = which registers to keep in locals (ALL, or X_FLAGS: the V registers stay in the Ctx)"""
    try:
        return _transform(lines, falls_to, cache)
    except Unexpected:
        return None


def _transform(lines, falls_to, cache):
    n = len(lines)
    index = {pc: i for i, (pc, _) in enumerate(lines)}
    st = [Stmt(text, cache) for _, text in lines]
    used = 0
    succ = []
    for i, s in enumerate(st):
        used |= s.used
        targets = GOTO.findall(s.text)
        if any(int(t, 16) not in index for t in targets):
            raise Unexpected(s.text)                     # a goto out of the function
        nxt = [index[int(t, 16)] for t in targets]
        if not s.terminal():
            nxt.append(i + 1)                            # i + 1 == n: the trailer
        succ.append(nxt)
    if not used:
        return None
    # forward data flow: the dirty set at the entry of every statement (and of the trailer, index n). Entries from the
    # top, from a landing pad (rp) and from the jump-table dispatch (written back before its goto) start clean.
    din = [0] * (n + 1)
    work, queued = list(range(n - 1, -1, -1)), [True] * n
    while work:
        i = work.pop()
        queued[i] = False
        d = (din[i] & st[i].keep) | st[i].gen
        for j in succ[i]:
            if din[j] | d != din[j]:
                din[j] |= d
                if j < n and not queued[j]:
                    queued[j] = True
                    work.append(j)
    out = [(pc, st[i].rewrite(din[i], used)) for i, (pc, _) in enumerate(lines)]
    trailer = '{ %sf_%x(c); return; }' % (spill(din[n]), falls_to) if falls_to else '{ %sreturn; }' % spill(din[n])
    decl = []
    xs = ['R_x%d = c->x[%d]' % (k, k) for k in range(32) if used >> k & 1]
    vs = ['R_v%d = c->v[%d]' % (k, k) for k in range(32) if used >> (32 + k) & 1]
    if xs:
        decl.append('  uint64_t %s;' % ', '.join(xs))
    if vs:
        decl.append('  V128 %s;' % ', '.join(vs))
    if used & FLAGS:
        decl.append('  RcFlags R_f = { c->N, c->Z, c->C, c->V };')
    return decl, out, trailer, reload(used)
