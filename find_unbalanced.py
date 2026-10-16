import re
import sys

with open(sys.argv[1], encoding='utf-8') as f:
    content = f.read()

pat = re.compile(r'oappend\(SET_F\(')
positions = [m.start() for m in pat.finditer(content)]
print(f'Found {len(positions)} oappend(SET_F calls')
unbalanced = 0
for pos in positions:
    depth = 0
    i = pos
    in_str = False
    escape = False
    while i < len(content):
        c = content[i]
        if escape:
            escape = False
            i += 1
            continue
        if c == chr(92):
            escape = True
            i += 1
            continue
        if c == '"':
            in_str = not in_str
        elif not in_str:
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
        i += 1
    if depth != 0:
        line = content[:pos].count(chr(10)) + 1
        snippet = content[pos:pos+200]
        print(f'  Unbalanced at line {line}: {snippet}')
        unbalanced += 1
print(f'Total unbalanced: {unbalanced}')
