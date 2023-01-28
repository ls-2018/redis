a = "id=%U addr=%s laddr=%s %s name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i " + "multi=%i qbuf=%U qbuf-free=%U argv-mem=%U multi-mem=%U rbs=%U rbp=%U obl=%U oll=%U omem=%U" + " tot-mem=%U events=%s cmd=%s user=%s redir=%I resp=%i"

res = ''
lines = []
with open('./data.txt', 'r', encoding='utf8') as f:
    for line in f.readlines():
        line = line.strip(' \n,/')
        lines.append(line)

for i, item in enumerate(a.split(' ')):
    t = 'ret = sdscatfmt(s,"%s",%s);\n' % (item, lines[i])
    res += t
print(res)
