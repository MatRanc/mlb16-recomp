#!/usr/bin/env python3
"""Prototype: keep SPU registers in C locals inside each lifted function.

Rewrites ctx->gpr[N] to a local rN per function: load at entry, write the
registers the function assigns back to ctx before anything that leaves the
function or can see the register file (tail calls, returns, calls, channel
ops, stops, halts), and reload after a call returns. Local-store stores go
through a char pointer, which may alias ctx->gpr; locals cannot be aliased, so
the compiler can keep them in machine registers.

usage: spu_regcache.py in.c out.c
"""
import re
import sys

FUNC_RE = re.compile(r'^void (\w+)\(spu_context\* ctx\) \{$')
GPR_RE = re.compile(r'ctx->gpr\[(\d+)\]')
ASSIGN_RE = re.compile(r'\br(\d+) = ')
# Statements that leave the function or let other code see ctx->gpr.
EXIT_TOKENS = ('SPU_DIRECT(', 'SPU_RET(', 'SPU_RET_REG(', 'spu_stop(ctx)',
               'g_spu_trampoline_fn', 'spu_halt(ctx)')
CALL_OPEN = '{ int32_t _si = (int32_t)ctx->image_id;'
CALL_CLOSE = 'spu_img_restore(ctx, _si); }'
CHAN_RE = re.compile(r'(?:(r\d+) = )?((?:\w+\()*spu_(?:wrch|rdch|rchcnt)\(ctx[^;]*\));')


SIMPLE_ONLY = True   # prototype: leave functions with calls, channel ops, stops, halts untouched
COMPLEX = ('spu_wrch(', 'spu_rdch(', 'spu_rchcnt(', 'spu_drain_call(', 'spu_stop(', 'spu_halt(',
           'SPU_RET_REG(', 'spu_indirect_branch')


def transform(name, body):
    text = '\n'.join(body)
    if SIMPLE_ONLY and any(t in text for t in COMPLEX):
        return body
    regs = sorted({int(n) for n in GPR_RE.findall(text)})
    if not regs:
        return body
    text = GPR_RE.sub(lambda m: 'r' + m.group(1), text)
    dirty = sorted({int(n) for n in ASSIGN_RE.findall(text)})
    wb = ' '.join(f'ctx->gpr[{n}] = r{n};' for n in dirty)
    rl = ' '.join(f'r{n} = ctx->gpr[{n}];' for n in regs)
    out = []
    for line in text.split('\n'):
        # Calls into other lifted functions: write back first, reload after.
        if CALL_OPEN in line:
            line = line.replace(CALL_OPEN, f'{wb} {CALL_OPEN}')
            line = line.replace(CALL_CLOSE, f'{CALL_CLOSE} {rl}')
        # Channel ops can block, take interrupts or run DMA: same treatment.
        def chan(m):
            dst, call = m.group(1), m.group(2)
            if dst:
                return f'{wb} {{ u128 _ch = {call}; {rl} {dst} = _ch; }}'
            return f'{wb} {call}; {rl}'
        line = CHAN_RE.sub(chan, line)
        # Exits: write back right before the first exit token on the line.
        for tok in EXIT_TOKENS:
            i = line.find(tok)
            if i < 0:
                continue
            if tok == 'g_spu_trampoline_fn':
                # "ctx->int_enable = ..; ctx->pc = ..; g_spu_trampoline_fn = ..; return;"
                j = line.find('ctx->int_enable')
                i = j if 0 <= j < i else i
            if tok == 'spu_halt(ctx)':
                j = line.find('fprintf(stderr, "[spu] HALT')
                i = j if 0 <= j < i else i
            line = line[:i] + wb + ' ' + line[i:]
            break
        out.append(line)
    decl = '        u128 ' + ', '.join(f'r{n} = ctx->gpr[{n}]' for n in regs) + ';'
    return [decl] + out


def main():
    src = open(sys.argv[1]).read().split('\n')
    out, i, nfun = [], 0, 0
    while i < len(src):
        line = src[i]
        out.append(line)
        if FUNC_RE.match(line):
            j = i + 1
            while src[j] != '}':
                j += 1
            out.extend(transform(FUNC_RE.match(line).group(1), src[i + 1:j]))
            out.append('}')
            nfun += 1
            i = j + 1
            continue
        i += 1
    open(sys.argv[2], 'w').write('\n'.join(out))
    print(f'{nfun} functions rewritten')


if __name__ == '__main__':
    main()
