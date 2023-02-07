#!/bin/sh
GIT_SHA1=`(git show-ref --head --hash=8 2> /dev/null || echo 00000000) | head -n1`
GIT_DIRTY=`git diff --no-ext-diff 2> /dev/null | wc -l`
BUILD_ID=`uname -n`"-"`date +%s`
if [ -n "$SOURCE_DATE_EPOCH" ]; then
  BUILD_ID=$(date -u -d "@$SOURCE_DATE_EPOCH" +%s 2>/dev/null || date -u -r "$SOURCE_DATE_EPOCH" +%s 2>/dev/null || date -u +%s)
fi
test -f over-release.h || touch over-release.h
(cat over-release.h | grep SHA1 | grep $GIT_SHA1) && \
(cat over-release.h | grep DIRTY | grep $GIT_DIRTY) && exit 0 # Already up-to-date
echo "#define REDIS_GIT_SHA1 \"$GIT_SHA1\"" > over-release.h
echo "#define REDIS_GIT_DIRTY \"$GIT_DIRTY\"" >> over-release.h
echo "#define REDIS_BUILD_ID \"$BUILD_ID\"" >> over-release.h
touch over-release.c # Force recompile of over-release.c
