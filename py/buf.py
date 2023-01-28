src = [
    'declared_name',
    'summary',
    'complexity',
    'since',
    'doc_flags',
    'replaced_by',
    'deprecated_since',
    'group',
    'history',
    'tips',
    'proc',
    'arity',
    'flags',
    'acl_categories',
]


class A:
    def __init__(self):
        self.xxxx = []
        self.stack = []

    def f3(self, temp):

        for i, x in enumerate(temp):
            temp[i] = temp[i].strip(' ,')
        for i, x in enumerate(src):
            if temp[i].startswith('.%s' % x):
                pass
            else:
                temp[i] = '.%s=%s' % (x, temp[i])
        x = ','.join(temp)
        print(x.count('{'), x.count('}'), x.count('{') == x.count('}'))
        if x.count('{') != x.count('}'):
            print(temp)
        self.xxxx.append('{%s},' % (', '.join(temp)))

    def f2(self):
        self.xx=False
        for item in self.stack:
            # if 'pfdebug' in item:
            #     self.xx=True
            #
            # if not self.xx:
            #     continue

            temp = []
            item = item.strip(' ,')[1:-1]
            res = ''
            c_count = 0
            x_count = 0
            for char in list(item):
                if c_count == x_count == 0 and char == ',' and res.count('"') % 2 == 0 and len(res) > 0:
                    temp.append(res.strip())
                    res = ''
                    c_count = 0
                    x_count = 0
                    continue
                else:
                    if len(res) == 0 and char == ',':
                        continue
                    res += char

                if (res.startswith('"') and res.count('"') % 2 != 1  ) :
                    temp.append(res.strip())
                    c_count = 0
                    x_count = 0
                    res = ''
                elif (len(res) == 0 and char == ','):
                    continue
                elif char == '{':
                    c_count += 1
                elif char == '}':
                    x_count += 1
                elif (  c_count == x_count and c_count > 0 ):
                    res.strip(',')
                    temp.append(res.strip())
                    res = ''
                    c_count = 0
                    x_count = 0
            temp.append(res)
            # print(temp)
            # print(item)
            # print(len(temp))

            self.f3(temp)

    def f1(self, ):

        with open('./a.c', 'r', encoding='utf8') as f:
            res = ''
            c_count = 0
            x_count = 0
            for line in f.readlines():

                # line = '"xpending","Return information and entries from a stream consumer group pending entries list, that are messages fetched but  never acknowledged.","O(N) with N being the number of elements returned, so asking for a small fixed number of entries per call is  O(1). O(M), where M is the total number of entries scanned when used with the IDLE filter. When the command  returns just the summary and the list of consumers is small, it runs in O(1) time; otherwise, an additional O(N)  time for iterating  every consumer.","5.0.0",CMD_DOC_NONE,NULL,NULL,COMMAND_GROUP_STREAM,XPENDING_History,XPENDING_tips,xpendingCommand,-3,CMD_READONLY,ACL_CATEGORY_STREAM,{{NULL, CMD_KEY_RO | CMD_KEY_ACCESS, KSPEC_BS_INDEX, .bs.index = {1}, KSPEC_FK_RANGE, .fk.range = {0, 1, 0}}},.args = XPENDING_Args'
                line = line.strip()
                if 'pfdebug' in line:
                    print(1)
                for char in list(line):
                    if c_count == x_count == 0 and char == ',':
                        continue
                    res += char
                    if char == '{':
                        c_count += 1
                    elif char == '}':
                        x_count += 1
                    elif c_count == x_count and char == ',' and len(res) != 1:
                        res = res.replace('""', ' ')
                        res = res.replace('" "', ' ')
                        self.stack.append(res)
                        res = ''
                        c_count = 0
                        x_count = 0

    def f4(self):
        with open('./v.c', 'w', encoding='utf8') as f:
            for i, item in enumerate(self.xxxx):
                f.write(item + '\n')
                if i % 10 == 0:
                    f.write('\n' * 2)


if __name__ == '__main__':
    a = A()
    a.stack.sort()
    a.f1()
    a.f2()
    a.f4()
