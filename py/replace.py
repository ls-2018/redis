import os.path

for current_dir, dirs, files in os.walk(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))):
    for file in files:
        p = os.path.join(current_dir, file)
        if 'deps' in p:
            continue
        if os.path.splitext(p)[1] not in ['.c', '.h', '.md']:
            continue
        with open(p, 'r', encoding='utf8') as f:
            data = f.read()

        for item in [
            ["，", ","],
            ["。", "."],
            ["；", ";"],
        ]:
            data = data.replace(item[0], item[1])

        with open(p, 'w', encoding='utf8') as f:
            f.write(data)
