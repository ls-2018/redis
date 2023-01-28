import os
import shutil
import tempfile


def eq(a1: list, b1: list):
    if len(a1) != len(b1):
        # print(a1, b1)
        return False
    for i in range(len(a1)):
        if a1[i] != b1[i]:
            # print(a1[i], b1[i])
            return False
    return True


td = tempfile.TemporaryDirectory()

path = os.path.join(td.name, 'redis')
shutil.copytree(os.path.dirname(os.path.dirname(__file__)), path)
print(path)
file_set = set()
os.system(f'cd {path} && git add . && git reset --hard d375595d5 ')
for current_dir, dirs, files in os.walk(path):
    for file in files:
        fpath = os.path.join(current_dir, file)
        if 'deps' in fpath:
            continue
        if fpath.endswith('.c') or fpath.endswith('.h'):
            file_set.add(fpath)

for item in file_set:
    s = os.path.abspath(item.replace(path, os.path.dirname(os.path.dirname(__file__))))
    ss = []
    dd = []
    if 'src/ae.h' in item:
        print(123)
    with open(s, 'r', encoding='utf8') as f:
        for line in f.readlines():
            if line.startswith('#include'):
                ss.append(line.strip().split('/*')[0].strip())
    with open(item, 'r', encoding='utf8') as f:
        for line in f.readlines():
            if line.startswith('#include'):
                dd.append(line.strip().split('/*')[0].strip())

    if not eq(ss, dd):
        print(item)
shutil.rmtree(td.name, ignore_errors=True)
