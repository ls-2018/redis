import time

from redis import Redis

r = Redis(host='127.0.0.1', port=6379)

for i in range(150):
    print(i)
    time.sleep(0.5)
    r.set("a" + str(i), i)

time.sleep(100000)
