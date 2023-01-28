cd /tmp
rm -rf ./redis*
git clone https://github.com/redis/redis.git -b 7.0
cd -
rm -rf ./hiredis
cp -r /tmp/redis/deps/hiredis .