import os.path

file = os.path.join(os.path.dirname(os.path.dirname(__file__)), "src", "mkreleasehdr.sh")
f = open(file, 'rb')
result = f.read()
result = result.replace(b'\r\n', b'\n')
f.close()
#    需要用二进制的方式('b')重写才会OK,否则会自动按照操作系统默认方式
f = open(file, 'wb')
f.write(result)
f.close()
