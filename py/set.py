import time

from redis import Redis

r = Redis(host='127.0.0.1', port=6379)

for i in range(150):
    print(i)
    r.set("a", i)

time.sleep(100000)
