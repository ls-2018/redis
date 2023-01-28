# !/bin/bash

echo 'update-jemalloc.sh 5.2.1'

URL="https:// github.com/jemalloc/jemalloc/releases/download/5.2.1/jemalloc-5.2.1.tar.bz2"
rm -rf ./jemalloc*
echo "Downloading $URL"
wget -O /tmp/jemalloc.tar.bz2 --no-check-certificate $URL tar xvjf /tmp/jemalloc.tar.bz2 rm -rf jemalloc mv
jemalloc-5.2.1 jemalloc echo "Use git status, add all files and commit changes."

hiredis 是基于 github.com/redis/hiredis 修改的

lua 比 lua5.1.5 多了几个文件