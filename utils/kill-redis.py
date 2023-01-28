#!/usr/bin/env python3
import platform
import os
import time


def kill_port_owner_windows(portnum):
    import os
    fp = os.popen("netstat -aon | findstr :%s" % portnum)
    lines = fp.readlines()
    fp.close()
    pid = None
    if len(lines) >= 2:
        pid = int(lines[1].split()[-1])
        os.popen('taskkill /pid %s' % pid)
        time.sleep(1)

    return pid


def kill_port_owner_linux(portnum):
    import os
    fp = os.popen("lsof -i :%s" % portnum)
    lines = fp.readlines()
    fp.close()
    pid = None
    if len(lines) >= 2:
        pid = int(lines[1].split()[1])
        os.popen('kill -9 %s' % pid)
        time.sleep(1)

    return pid


port_finders = {
    'Windows': kill_port_owner_windows,
    'Linux': kill_port_owner_linux,
    'Darwin': kill_port_owner_linux
}

try:
    kill_port_owner = port_finders[platform.uname().system]
except KeyError:
    raise RuntimeError("No known port finder for your OS (%s)" % os.name)

if __name__ == '__main__':
    print("pid:-> %s" % kill_port_owner(6379))
    try:
        os.remove(os.path.join(os.path.dirname(os.path.dirname(__file__)), 'src', 'dump.rdb'))
    except Exception:
        pass
