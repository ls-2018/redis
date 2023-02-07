This README is just a fast *quick start* document. You can find more detailed documentation at [redis.io](https://
redis.io).

### 64位编译器 c

```
har ：1个字节
char*(即指针变量): 8个字节
short int : 2个字节
int： 4个字节
unsigned int : 4个字节
float: 4个字节
double: 8个字节
long: 8个字节
long long: 8个字节
unsigned long: 8个字节
```

## 编码类型

- OBJ_STRING
  数字共享对象          [0,10000) & (没有开启maxmemory 、或没有使用LRU 、LFU)     
  OBJ_ENCODING_RAW    >  44 字节,redisObject 和 SDS 分开存储
  OBJ_ENCODING_EMBSTR <= 44 字节,嵌入式存储,redisObject 和 SDS 一起分配内存 redisObject与sdshdr8内存连在一块

  typedef struct redisObject {
  unsigned type : 4; // 4bit
  unsigned encoding : 4; // 4bit
  unsigned lru : LRU_BITS; // 24bit
  int refcount; // 4byte
  void *ptr; // 8byte
  } robj;

  struct __attribute__((__packed__)) sdshdr8 {
  uint8_t len; // 1byte
  uint8_t alloc; // 1byte
  unsigned char flags; // 1byte
  char buf[]; // 不占空间
  };


- OBJ_LIST
- OBJ_SET
- OBJ_ZSET
- OBJ_HASH

## 概念

```
1、
    C语言使用长度为N+1的字符数组来表示长度为N的字符串,并且字符数组的最后一个元素总是空字符'\0'
    sds [Simple dynamic string] 简单动态字符串、
      特点:
        常数复杂度获取字符串长度
        杜绝缓冲区溢出
        减少修改字符串长度所需的内存重分配次数
        二进制安全
        兼容部分C字符串函数
      功能:
        1、存储value
        2、用作缓冲区   AOF模块的AOF缓冲区;客户端状态中的输入缓冲区
2、
    __packed__  1字节对齐

3、dictRehash运行时间
    1、服务器目前没有执行BGSAVE命令或者BGREWRITERAOF命令,并且哈希表的负载因子大于等于1
    2、服务器目前正在执行BGSAVE命令或者BGREWRITERAOF命令,并且哈希表的负载因子大于等于5
    以及在rehash期间, 很多处调用的_dictRehashStep

4、跳表的应用
    1、有序集合
    2、集群节点中用作内部数据结构
   - 访问过程
    1、迭代程序首先访问跳表的第一个节点(表头),然后从第四层(why?)的前进指针移动到标志的第二个节点   
    2、在第二个节点时,程序沿着第二层(why?)的前进指针移动到表中的第三个节点 
    3、在第三个节点时,程序同样沿着第二层(why?)的前进指针移动到表中的第四个节点
    4、当程序再次沿着第四个节点的前进指针移动时,它碰到一个NULL,程序知道这是已经到达了跳跃表的表尾,于是结束这次遍历
   - 注意 
    在同一个跳跃表中,各个节点保存的成员对象必须是唯一的,但是多个节点保存的分值却可以是相同的
    
    
5、zset 实现
   1、ZIPLIST        len(v)<=64
   2、SKIPLIST       len(v)>64
   
   
6、ziplist 连锁更新
    如果我们将一个长度大于等于254字节的新节点new设置为压 缩列表的表头节点,那么new将成为e1的前置节点
    因为e1的previous_entry_length属性仅长1字节,它没办法保存新节 点new的长度,所以程序将对压缩列表执行空间重分配操作,
        并将e1节 点的previous_entry_length属性从原来的1字节长扩展为5字节长. 现在,麻烦的事情来了,e1原本的长度介于250字节至253字节之间,
        在为previous_entry_length属性新增四个字节的空间之后,e1的长度 就变成了介于254字节至257字节之间,而这种长度使用1字节长的
        previous_entry_length属性是没办法保存的. 
    因此,为了让e2的previous_entry_length属性可以记录下e1的长度,程序需要再次对压缩列表执行空间重分配操作,并将e2节点的 previous_entry_length属性从原来的1字节长扩展为5字节长.
```

```
strlen字符串长度
strdup字符串拷贝库函数
isatty判断文件描述词是否是为终端机
strchr(const char *str, int c),即在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
int memcmp(const void *str1, const void *str2, size_t n);比较内存区域buf1和buf2的前count个字节。
memcpy(void *destin, void *source, unsigned n) 内存拷贝函数,函数的功能是从源内存地址的起始位置开始拷贝若干个字节到目标内存地址中
strrchr(const char *str, int c) 在参数 str 所指向的字符串中搜索最后一次出现字符 c（一个无符号字符）的位置.
strstr(const char *haystack, const char *needle) 在字符串 haystack 中查找第一次出现字符串 needle 的位置,不包含终止符 '\0'.
localtime_r(&ut, &tm);  用来将参数tm所指的tm结构数据转换成从公元1970年1月1日0时0分0 秒算起至今的UTC时间所经过的秒数.并存储到ut
sysconf  用来获取系统执行的配置信息
如果在定义函数时、结构体,最左端加关键字extern,则此函数是外部函数、结构体,可供其它文件调用
 
ntohs  作用是将一个16位数由网络字节顺序转换为主机字节顺序.
inet_ntop 从数值格式(addrptr)转换到表达式(strptr)
```

Building Redis
--------------

    % make
    % make BUILD_TLS=yes
    % make USE_SYSTEMD=yes
    % make PROG_SUFFIX="-alt"
    % make 32bit
    % make test

    % ./utils/gen-test-certs.sh
    % ./runtest --tls

    % make distclean

    % make CFLAGS="-m32 -march=native" LDFLAGS="-m32"

    % make MALLOC=libc
    % make MALLOC=jemalloc

    % make V=1
    % ./redis-server /path/to/redis.conf

    % ./redis-server --port 9999 --replicaof 127.0.0.1 6379
    % ./redis-server /etc/redis/6379.conf --loglevel debug

    % make PREFIX=/some/other/directory install

    % cd utils
    % ./install_server.sh

## troubleshoot

- ```获取 Redis 实例在当前环境下的基线性能.```
- ```
  是否用了慢查询命令？如果是的话,就使用其他命令替代慢查询命令,或者把聚合计算命令放在客户端做.
  ```
- ```
  是否对过期 key 设置了相同的过期时间？对于批量删除的 key,可以在每个 key 的过期时间上加一个随机数,避免同时删除.
  ```
- ```
  是否存在 bigkey？ 对于 bigkey 的删除操作,如果你的 Redis 是 4.0 及以上的版本,
  可以直接利用异步线程机制减少主线程阻塞;如果是 Redis 4.0 以前的版本,可以使用 SCAN 命令迭代删除;
  对于 bigkey 的集合查询和聚合操作,可以使用 SCAN 命令在客户端完成.
  ```
- ```
  Redis AOF 配置级别是什么？业务层面是否的确需要这一可靠性级别？如果我们需要高性能,同时也允许数据丢失,
  可以将配置项 no-appendfsync-on-rewrite 设置为 yes,避免 AOF 重写和 fsync 竞争磁盘 IO 资源,
  导致 Redis 延迟增加.当然, 如果既需要memcpy高性能又需要高可靠性,最好使用高速固态盘作为 AOF 日志的写入盘.
  ``` 

- Redis 实例的内存使用是否过大？发生 swap 了吗？如果是的话,就增加机器内存,或者是使用 Redis 集群, 分摊单机 Redis
  的键值对数量和内存压力.同时,要避免出现 Redis 和其他内存需求大的应用共享机器的情况.

- 在 Redis 实例的运行环境中,是否启用了透明大页机制？如果是的话,直接关闭内存大页机制就行了.

- 是否运行了 Redis 主从集群？如果是的话,把主库实例的数据量大小控制在 2~4GB,以免主从复制时,从库因加载大的 RDB 文件而阻塞.

- 是否使用了多核 CPU 或 NUMA 架构的机器运行 Redis 实例？使用多核 CPU 时,可以给 Redis 实例绑定物理核; 使用 NUMA 架构时,注意把
  Redis 实例和网络中断处理程序运行在同一个 CPU Socket
  上.

使用复杂度过高的命令或一次查询全量数据; 操作 bigkey; 大量 key 集中过期; 内存达到 maxmemory; 客户端使用短连接和 Redis 相连;
当 Redis 实例的数据量大时,无论是生成 RDB,还是 AOF
重写,都会导致 fork 耗时严重; AOF 的写回策略为 always,导致每个操作都要同步刷回磁盘; Redis 实例运行机器的内存不足,导致
swap 发生,Redis 需要到 swap 分区读取数据; 进程绑定 CPU 不合理;
Redis 实例运行机器上开启了透明内存大页机制;网卡压力过大.

## 运维

- 在bgsave中,最好不要主动让其重启 Issue:

- https://juejin.cn/post/6982159060187496462
- https://www.cnbugs.com/post-1833.html

```
127.0.0.1:6379> RPUSH profile 1 3 5 10086 hello world
(integer) 6
127.0.0.1:6379> OBJECT ENCODING profile
"quicklist"
127.0.0.1:6379> DEL profile
(integer) 1
127.0.0.1:6379>
127.0.0.1:6379>
127.0.0.1:6379> HMSET profile name Jack age 28 job programmer
OK
127.0.0.1:6379> OBJECT ENCODING profile
"listpack"
127.0.0.1:6379> DEL profile
(integer) 1

```

``` 
- k:v 
  - encoding
    - int
    - embstr
    - raw
  - command 
    - set
    - get
    - append
    - incrbyfloat
    - incrby
    - decrby
    - strlen
    - setrange
    - getrange
    
- k:[]
  - encoding
    - list       弃用
    - ziplist
    - linkedlist 弃用
    - quicklist
  - command
    - LPUSH
    - RPUSH
    - LPOP
    - RPOP
    - LINDEX
    - LLEN
    - LINSERT
    - LREM
    - LTRIM
    - LSET
    - BRPOPLPUSH mq mqback 3
  - demo
    - EVAL "for i=1, 512 do redis.call('RPUSH', KEYS[1],i)end" 1 "integers"

- k:{}
  - encoding
    - ziplist
    - hashtable
    - listpack 【内存紧凑】
  - command
    - HSET
    - HGET
    - HEXISTS
    - HDEL
    - HLEN
    - HGETALL
  - demo
    - EVAL "for i=1, 5120 do redis.call('HSET', KEYS[1], i, i)end" 1 "numbers"

- k:set()
  - encoding
    - intset 【节省内存、但不会降级32->16】
    - hashtable
  - command
    - SADD
    - SCARD
    - SISMEMBER
    - SMEMBERS
    - SRANDMENBER
    - SPOP
    - SREM
    - SINTER
  - demo
    - EVAL "for i=1, 512 do redis.call('SADD', KEYS[1], i) end" 1 integers
    
- k:zset()
  - encoding
    - listpack 【内存紧凑】  
      - v,score使用两个entry保存
    - skiplist
      - dict        v:->score的映射
      - skiplist    按照score从小到大排序
      - 这两种数据结构都会通过指针来共享相同元素的成员和分值
  - command
    - ZADD
    - ZCARD
    - ZCOUNT
    - ZRANGE
    - ZREVRANGE
    - ZREVRANK
    - ZREM
    - ZSCORE
  - demo
    - EVAL "for i=1, 128 do redis.call('ZADD', KEYS[1], i, i) end" 1 numbers   

- Bitmap 
- HyperLogLog 
- GEO
  - GEOADD cars:locations 116.034579 39.030452 33
  - GEOADD cars:locations CH 116.034579 39.030452 33
  - GEORADIUS cars:locations 116.054579 39.030452 5 km ASC COUNT 10
  

- Stream    Radix Tree、listpack
  List没有ack机制和不支持多个消费者
  PubSub就是简单的基于内存的多播机制

  
  - XADD mqstream * repo 5
  - XREAD BLOCK 100 STREAMS mqstream 1599203861727-0   
  - XREAD BLOCK 100 STREAMS mqstream $    最新的一条,100毫秒
  - XGROUP create mqstream group1 0
  - XREADGROUP group group1 consumer1 streams mqstream >
  - 为了保证消费者在发生故障或宕机再次重启后,仍然可以读取未处理完的消息,Streams 会自动使用内部队列（也称为 PENDING List）留存消费组里每个消费者读取的消息,
  - 直到消费者使用 XACK 命令通知 Streams“消息已经处理完成”.如果消费者没有成功处理消息,它就不会给 Streams 发送 XACK 命令,消息仍然会留存.
  - 此时,消费者可以在重启后,用 XPENDING 命令查看已读取、但尚未确认处理完成的消息.
  - XPENDING mqstream group2                    查看一下 group2 中各个消费者已读取、但尚未确认的消息个数.
  - XPENDING mqstream group2 - + 10 consumer2   查看某个消费者具体读取了哪些数据
  - XACK mqstream group2 1599274912765-0


list -> ziplist -> quicklist -> listpack
list:
  内存不紧凑,无法使用缓存;每个节点都需要结构头,内存开销较大
ziplist 
  内存紧凑;存在连锁更新问题   【每个key包含前一项的长度】
quicklist
  内存紧凑;部分解决连锁更新
listpack
  内存紧凑;解决连锁更新   【只记录当前节点长度】
  
```

```
# 查看键的空转时长,不会改变lru属性
OBJECT IDLETIME msg  
```

## 删除策略

- 1、定时删除：
  ```
    在设置键的过期时间的同时,创建一个定时器（timer）,让定时器在键的过期时间来临时,立即执行对键的删除操作.
    定时删除占用太多CPU时间,影响服务器的响应时间和吞吐量
  ```
- 2、惰性删除：
  ```
    放任键过期不管,但是每次从键空间中获取键时,都检查取得的键是否过期,如果过期的话,就删除该键;如果没有过期,就返回该键.
    惰性删除浪费太多内存,有内存泄漏的危险
  ```
- 3、定期删除：
  ```
    每隔一段时间,程序就对数据库进行一次检查,删除 里面的过期键.至于要删除多少过期键,以及要检查多少个数据库,则由算法决定.
  ```

#### ziplist

```
  设计的目的不是为了查询,而是为了减少内存的使用和内存的碎片化

1、内存利用率,数组和压缩列表都是非常紧凑的数据结构,它比链表占用的内存要更少.Redis是内存数据库,大量数据存到内存中,此时需要做尽可能的优化,提高内存的利用率. 
2、数组对CPU高速缓存支持更友好,所以Redis在设计时,集合数据元素较少情况下,默认采用内存紧凑排列的方式存储,同时利用CPU高速缓存不会降低访问速度.
当数据元素超过设定阈值后,避免查询时间复杂度太高,转为哈希和跳表数据结构存储,保证查询效率.
```

ziplist 设计的目的不是为了查询,而是为了减少内存的使用和内存的碎片化

1、内存利用率,数组和压缩列表都是非常紧凑的数据结构,它比链表占用的内存要更少.Redis是内存数据库,大量数据存到内存中,此时需要做尽可能的优化,提高内存的利用率.
2、数组对CPU高速缓存支持更友好,所以Redis在设计时,集合数据元素较少情况下,默认采用内存紧凑排列的方式存储,同时利用CPU高速缓存不会降低访问速度.
当数据元素超过设定阈值后,避免查询时间复杂度太高,转为哈希和跳表数据结构存储,保证查询效率.

Redis 是单线程,主要是指 Redis 的网络 IO 和键值对读写是由一个线程来完成的,这也是 Redis 对外提供键值存储服务的主要流程.但
Redis 的其他功能,比如持久化、异步删除、集群数据同步等,其实是由额外的线程执行的.

#### 为什么用单线程？

```
Redis 是单线程,主要是指 Redis 的网络 IO 和键值对读写是由一个线程来完成的,这也是 Redis 对外提供键值存储服务的主要流程.但 Redis 的其他功能,比如持久化、异步删除、集群数据同步等,其实是由额外的线程执行的.

Redis 的单线程指 Redis 的网络 IO 和键值对读写由一个线程来完成的（这是 Redis 对外提供键值对存储服务的主要流程）
Redis 的持久化、异步删除、集群数据同步等功能是由其他线程而不是主线程来执行的,所以严格来说,Redis 并不是单线程

多线程会有共享资源的并发访问控制问题,为了避免这些问题,Redis 采用了单线程的模式,而且采用单线程对于 Redis 的内部实现的复杂度大大降低
```

#### 为什么单线程就挺快？

```

1.Redis 大部分操作是在内存上完成,并且采用了高效的数据结构如哈希表和跳表
2.Redis 采用多路复用,能保证在网络 IO 中可以并发处理大量的客户端请求,实现高吞吐率
```

#### Redis 6.0 版本为什么又引入了多线程？

```

Redis 的瓶颈不在 CPU ,而在内存和网络,内存不够可以增加内存或通过数据结构等进行优化
但 Redis 的网络 IO 的读写占用了发部分 CPU 的时间,如果可以把网络处理改成多线程的方式,性能会有很大提升
所以总结下 Redis 6.0 版本引入多线程有两个原因
1.充分利用服务器的多核资源
2.多线程分摊 Redis 同步 IO 读写负荷

执行命令还是由单线程顺序执行,只是处理网络数据读写采用了多线程,而且 IO 线程要么同时读 Socket ,要么同时写 Socket ,不会同时读写
```

Redis 的单线程指 Redis 的网络 IO 和键值对读写由一个线程来完成的（这是 Redis 对外提供键值对存储服务的主要流程） Redis
的持久化、异步删除、集群数据同步等功能是由其他线程而不是主线程来执行的,所以严格来说,Redis 并不是单线程

为什么用单线程？ 多线程会有共享资源的并发访问控制问题,为了避免这些问题,Redis 采用了单线程的模式,而且采用单线程对于 Redis
的内部实现的复杂度大大降低

为什么单线程就挺快？ 1.Redis 大部分操作是在内存上完成,并且采用了高效的数据结构如哈希表和跳表 2.Redis 采用多路复用,能保证在网络
IO 中可以并发处理大量的客户端请求,实现高吞吐率

Redis 6.0 版本为什么又引入了多线程？ Redis 的瓶颈不在 CPU ,而在内存和网络,内存不够可以增加内存或通过数据结构等进行优化 但
Redis 的网络 IO 的读写占用了发部分 CPU
的时间,如果可以把网络处理改成多线程的方式,性能会有很大提升 所以总结下 Redis 6.0 版本引入多线程有两个原因 1.充分利用服务器的多核资源
2.多线程分摊 Redis 同步 IO 读写负荷

执行命令还是由单线程顺序执行,只是处理网络数据读写采用了多线程,而且 IO 线程要么同时读 Socket ,要么同时写 Socket ,不会同时读写

#### Redis单线程处理IO请求性能瓶颈主要包括2个方面：

```
1、任意一个请求在server中一旦发生耗时,都会影响整个server的性能,也就是说后面的请求都要等前面这个耗时请求处理完成,自己才能被处理到.耗时的操作包括以下几种：
a、操作bigkey：写入一个bigkey在分配内存时需要消耗更多的时间,同样,删除bigkey释放内存同样会产生耗时;
b、使用复杂度过高的命令：例如SORT/SUNION/ZUNIONSTORE,或者O(N)命令,但是N很大,例如lrange key 0 -1一次查询全量数据;
c、大量key集中过期：Redis的过期机制也是在主线程中执行的,大量key集中过期会导致处理一个请求时,耗时都在删除过期key,耗时变长;
d、淘汰策略：淘汰策略也是在主线程执行的,当内存超过Redis内存上限后,每次写入都需要淘汰一些key,也会造成耗时变长;
e、AOF刷盘开启always机制：每次写入都需要把这个操作刷到磁盘,写磁盘的速度远比写内存慢,会拖慢Redis的性能;
f、主从全量同步生成RDB：虽然采用fork子进程生成数据快照,但fork这一瞬间也是会阻塞整个线程的,实例越大,阻塞时间越久;
2、并发量非常大时,单线程读写客户端IO数据存在性能瓶颈,虽然采用IO多路复用机制,但是读写客户端数据依旧是同步IO,只能单线程依次读取客户端的数据,无法利用到CPU多核.
=======
Redis单线程处理IO请求性能瓶颈主要包括2个方面：

1、任意一个请求在server中一旦发生耗时,都会影响整个server的性能,也就是说后面的请求都要等前面这个耗时请求处理完成,自己才能被处理到.耗时的操作包括以下几种：
a、操作bigkey：写入一个bigkey在分配内存时需要消耗更多的时间,同样,删除bigkey释放内存同样会产生耗时; b、使用复杂度过高的命令：例如SORT/SUNION/ZUNIONSTORE,或者O(N)
命令,但是N很大,例如lrange key 0 -1一次查询全量数据; c、大量key集中过期：Redis的过期机制也是在主线程中执行的,大量key集中过期会导致处理一个请求时,耗时都在删除过期key,耗时变长;
d、淘汰策略：淘汰策略也是在主线程执行的,当内存超过Redis内存上限后,每次写入都需要淘汰一些key,也会造成耗时变长;
e、AOF刷盘开启always机制：每次写入都需要把这个操作刷到磁盘,写磁盘的速度远比写内存慢,会拖慢Redis的性能;
f、主从全量同步生成RDB：虽然采用fork子进程生成数据快照,但fork这一瞬间也是会阻塞整个线程的,实例越大,阻塞时间越久;
2、并发量非常大时,单线程读写客户端IO数据存在性能瓶颈,虽然采用IO多路复用机制,但是读写客户端数据依旧是同步IO,只能单线程依次读取客户端的数据,无法利用到CPU多核.

针对问题1,一方面需要业务人员去规避,一方面Redis在4.0推出了lazy-free机制,把bigkey释放内存的耗时操作放在了异步线程中执行,降低对主线程的影响.

针对问题2,Redis在6.0推出了多线程,可以在高并发场景下利用CPU多核多线程读写客户端数据,进一步提升server性能,当然,只是针对客户端的读写是并行的,每个命令的真正操作依旧是单线程的.

```

#### redis为什么会用单线程？

```
作者阐述的原因有：
1. 多线程的性能并不是呈线性增长的 
2. 引入多线程需要处理“多线程下共享资源的并发访问空置” 
3. 引入多线程会增加代码的调试难度和维护难度.
个人理解是这样的redis在CPU指令在内存中处理数据速度极快,网络 IO 的读写其实是瓶颈,所以使用其他线程处理.但是Mysql主要性能瓶颈在于数据的存/取,如果使用单线程那就太慢了.
个人理解,IO多路复用简单说是IO阻塞或非阻塞的都不准确.严格来说应用程序从网络读取数据到数据可用,分两个阶段：第一阶段读网络数据到内核,第二阶段读内核数据到用户态.IO多路复用解决了第一阶段阻塞问题,而第二阶段的读取阻塞的串行读.为了进一步提高REDIS的吞吐量,REDIS6.0使用多线程利用多CPU的优势解决第二阶段的阻塞.说的不对的地方,请斧正.
```

【redis为什么会用单线程？】 作者阐述的原因有：

1. 多线程的性能并不是呈线性增长的
2. 引入多线程需要处理“多线程下共享资源的并发访问空置”
3. 引入多线程会增加代码的调试难度和维护难度.

个人理解是这样的redis在CPU指令在内存中处理数据速度极快,网络 IO 的读写其实是瓶颈,所以使用其他线程处理.但是Mysql主要性能瓶颈在于数据的存/取,如果使用单线程那就太慢了.

个人理解,IO多路复用简单说是IO阻塞或非阻塞的都不准确.严格来说应用程序从网络读取数据到数据可用,分两个阶段：第一阶段读网络数据到内核,第二阶段读内核数据到用户态.
IO多路复用解决了第一阶段阻塞问题,而第二阶段的读取阻塞的串行读.为了进一步提高REDIS的吞吐量,REDIS6.0使用多线程利用多CPU的优势解决第二阶段的阻塞.说的不对的地方,请斧正.

repl_backlog_buffer 环形缓冲区 repl_backlog_size 是为了从库断开之后,如何找到主从差异数据而设计的环形缓冲区,从而避免全量同步带来的性能开销.

#### repl_backlog_buffer 环形缓冲区 repl_backlog_size

```
是为了从库断开之后,如何找到主从差异数据而设计的环形缓冲区,从而避免全量同步带来的性能开销.
```

主从全量同步使用RDB而不使用AOF的原因：

1、首先RDB和AOF最大的区别就是,RDB是内存快照,而AOF记录的是数据变化的过程,在全量初始化的情况下肯定是快照更优,RDB作为数据初始化的方式也更加快
2、是针对RDB和AOF的文件大小问题,AOF是数据变化的过程（动态变化）,相比于RDB不利于压缩,使用RDB在传输文件的时候可以更好的节约网络资源
3、进行主从同步并不是只使用RDB,而是RDB +
缓冲区的方式,这样可以保证bgsave期间的数据任然能同步

在生产环境中主从复制会经常出现以下两种情况：1、从服务器重启,2、主服务器宕机
在早期Redis中每次出现意外进行重新同步都是使用RDB的方式（sync）,会导致很大的开销,于是在Redis2.8开始实现了部分重同步的功能psync
psync 的格式如下：psync <Master-Run-ID> <OFFSET> ,通过缓冲区 + offset的方式来避免每次进行完全重同步

为了保证数据的最终一致性现如今Redis也具备两种同步方式： 1、完全重同步（RDB同步） 2、部分重同步（缓冲区同步）

【完全重同步】的开销是很大的（走bgsave）,生产环境中希望尽可能的使用【部分重同步】,但是【部分重同步】的条件也比较苛刻条件如下：
1、从服务器两次执行 RUN_ID 必须相等 2、复制偏移量必须包含在复制缓冲区中

#### 主从全量同步使用RDB而不使用AOF的原因：

```
1、首先RDB和AOF最大的区别就是,RDB是内存快照,而AOF记录的是数据变化的过程,在全量初始化的情况下肯定是快照更优,RDB作为数据初始化的方式也更加快
2、是针对RDB和AOF的文件大小问题,AOF是数据变化的过程（动态变化）,相比于RDB不利于压缩,使用RDB在传输文件的时候可以更好的节约网络资源
3、进行主从同步并不是只使用RDB,而是RDB + 缓冲区的方式,这样可以保证bgsave期间的数据任然能同步

在生产环境中主从复制会经常出现以下两种情况：1、从服务器重启,2、主服务器宕机
在早期Redis中每次出现意外进行重新同步都是使用RDB的方式（sync）,会导致很大的开销,于是在Redis2.8开始实现了部分重同步的功能psync
psync 的格式如下：psync <Master-Run-ID> <OFFSET> ,通过缓冲区 + offset的方式来避免每次进行完全重同步
也由于这些原因,Redis4.0的时候提出了psync2的方式,主要改进点在两个方面： 1、RDB文件中用于存放辅助字段的 AUX_FIELD_KEY_VALUE_PAIRS
中保存主服务器的RUN_ID,当从服务器恢复的时候能避免进行完全重同步 2、在新易主的服务器中冗余上一个主服务器的RUN_ID 和 offset 分别存放在 server.replid2 和
server.second_replid_offset 这两个字段中,这样避免主从切换后发生完全重同步

如何选定新主库： 1、筛选 down-after-milliseconds 是我们认定主从库断连的最大连接超时时间. 如果在 down-after-milliseconds
毫秒内,主从节点都没有通过网络联系上,我们就可以认为主从节点断连了. 如果发生断连的次数超过了 10 次,就说明这个从库的网络状况不好,不适合作为新主库. 2、打分 1、优先级最高的得分最高 slave-priority 配置项
2、和旧主库同步程度最接近的从库得分最高 master_repl_offset 和 slave_repl_offset 的差值 3、ID号小的从库得分最高

lru是最近最少使用 lfu是最近最不频繁使用 ttl是距离到期时间长短 ramdon是随机
```

### lru

lru 只会记录key访问的时间,不会记录访问次数 LRU 策略更加关注数据的时效性

### LFU

LFU 策略更加关注数据的访问频次.

#### 基于pub\sub组成的哨兵集群

```
 
主观下线判定:   通过哨兵有quorum个节点达成一致,才会做下线操作
哨兵领导者:     票数大于等于集群节点个数的大多数
leader如何选定新主库：
①、筛选
  down-after-milliseconds 是我们认定主从库断连的最大连接超时时间.
  如果在 down-after-milliseconds 毫秒内,主从节点都没有通过网络联系上,我们就可以认为主从节点断连了.
  如果发生断连的次数超过了 10 次,就说明这个从库的网络状况不好,不适合作为新主库.
②、打分
  1、优先级最高的得分最高
  slave-priority 配置项
  2、和旧主库同步程度最接近的从库得分最高
  master_repl_offset 和 slave_repl_offset 的差值
  3、ID号小的从库得分最高

```

#### 调大down-after-milliseconds值,对减少误判是不是有好处？

```
是有好处的,适当调大down-after-milliseconds值,当哨兵与主库之间网络存在短时波动时,可以降低误判的概率.
但是调大down-after-milliseconds值也意味着主从切换的时间会变长,对业务的影响时间越久,我们需要根据实际场景进行权衡,设置合理的阈值.
```

#### fork

```
fork时对内存、CPU都有影响.
fork采用的是写时复制,就是不直接复制数据过去,而是会复制父进程的页表给子进程.
这样父子进程使用不同的页表,指向相同的内存.这块儿消耗主要就在页表的数量上,数据越大,页表越多,复制就越花时间.
如果写请求过多,主线程就会进行复制,比较耗时.整个过程都会阻塞主线程.
```

#### 聚合操作

```
SUNIONSTORE、SDIFFSTORE、SINTERSTORE做并集、差集、交集 并存储为新的键值
SUNION、SDIFF、SINTER               做并集、差集、交集 不会存储为新键
注意:📢
1、如果是在集群模式使用多个key聚合计算的命令,一定要注意,因为这些key可能分布在不同的实例上,多个实例之间是无法做聚合运算的,这样操作可能会直接报错或者得到的结果是错误的！
2、当数据量非常大时,使用这些统计命令,因为复杂度较高,可能会有阻塞Redis的风险,建议把这些统计数据与在线业务数据拆分开,实例单独部署,防止在做统计操作时影响到在线业务.
```

#### mq

```

  1、生产者在发布消息时异常：  
    a) 网络故障或其他问题导致发布失败（直接返回错误,消息根本没发出去）
    b) 网络抖动导致发布超时（可能发送数据包成功,但读取响应结果超时了,不知道结果如何）
    
    情况a还好,消息根本没发出去,那么重新发一次就好了.但是情况b没办法知道到底有没有发布成功,所以也只能再发一次.
    所以这两种情况,生产者都需要重新发布消息,直到成功为止（一般设定一个最大重试次数,超过最大次数依旧失败的需要报警处理）.
    这就会导致消费者可能会收到重复消息的问题,所以消费者需要保证在收到重复消息时,依旧能保证业务的正确性（设计幂等逻辑）,一般需要根据具体业务来做,
    例如使用消息的唯一ID,或者版本号配合业务逻辑来处理.  
    
  2、消费者在处理消息时异常：
    也就是消费者把消息拿出来了,但是还没处理完,消费者就挂了.这种情况,需要消费者恢复时,依旧能处理之前没有消费成功的消息.
    使用List当作队列时,也就是利用老师文章所讲的备份队列来保证,代价是增加了维护这个备份队列的成本.而Streams则是采用ack的方式,
    消费成功后告知中间件,这种方式处理起来更优雅,成熟的队列中间件例如RabbitMQ、Kafka都是采用这种方式来保证消费者不丢消息的.

  3、消息队列中间件丢失消息
    上面2个层面都比较好处理,只要客户端和服务端配合好,就能保证生产者和消费者都不丢消息.但是,如果消息队列中间件本身就不可靠,
    也有可能会丢失消息,毕竟生产者和消费这都依赖它,如果它不可靠,那么生产者和消费者无论怎么做,都无法保证数据不丢失.
    
    a)  在用Redis当作队列或存储数据时,是有可能丢失数据的：一个场景是,如果打开AOF并且是每秒写盘,因为这个写盘过程是异步的,
        Redis宕机时会丢失1秒的数据.而如果AOF改为同步写盘,那么写入性能会下降.另一个场景是,如果采用主从集群,如果写入量比较大,
        从库同步存在延迟,此时进行主从切换,也存在丢失数据的可能（从库还未同步完成主库发来的数据就被提成主库）.总的来说,
        Redis不保证严格的数据完整性和主从切换时的一致性.我们在使用Redis时需要注意.
    b) 而采用RabbitMQ和Kafka这些专业的队列中间件时,就没有这个问题了.这些组件一般是部署一个集群,生产者在发布消息时,
        队列中间件一般会采用写多个节点+预写磁盘的方式保证消息的完整性,即便其中一个节点挂了,也能保证集群的数据不丢失.
        当然,为了做到这些,方案肯定比Redis设计的要复杂（毕竟是专们针对队列场景设计的）.

综上,Redis可以用作队列,而且性能很高,部署维护也很轻量,但缺点是无法严格保数据的完整性（个人认为这就是业界有争议要不要使用Redis当作队列的地方）.
而使用专业的队列中间件,可以严格保证数据的完整性,但缺点是,部署维护成本高,用起来比较重.

所以我们需要根据具体情况进行选择,如果对于丢数据不敏感的业务,例如发短信、发通知的场景,可以采用Redis作队列.如果是金融相关的业务场景,例如交易、
支付这类,建议还是使用专业的队列中间件.
```

#### 内存释放

```
释放内存只是第一步,为了更加高效地管理内存空间,在应用程序释放内存时,操作系统需要把释放掉的内存块插入一个空闲内存块的链表,
以便后续进行管理和再分配.这个过程本身需要一定时间,而且会阻塞当前释放内存的应用程序,所以,如果一下子释放了大量内存,
空闲内存块链表操作时间就会增加,相应地就会造成 Redis 主线程的阻塞.
```

#### lazy-free

```
1、lazy-free是4.0新增的功能,但是默认是关闭的,需要手动开启.

2、手动开启lazy-free时,有4个选项可以控制,分别对应不同场景下,要不要开启异步释放内存机制：
a) lazyfree-lazy-expire：key在过期删除时尝试异步释放内存
b) lazyfree-lazy-eviction：内存达到maxmemory并设置了淘汰策略时尝试异步释放内存
c) lazyfree-lazy-server-del：执行RENAME/MOVE等命令或需要覆盖一个key时,删除旧key尝试异步释放内存
d) replica-lazy-flush：主从全量同步,从库清空数据库时异步释放内存
e) lazyfree-lazy-user-del: 打开这个选项后,使用DEL和UNLINK就没有区别了.

3、即使开启了lazy-free,如果直接使用DEL命令还是会同步删除key,只有使用UNLINK命令才会可能异步删除key.

4、这也是最关键的一点,上面提到开启lazy-free的场景,除了replica-lazy-flush之外,其他情况都只是*可能*去异步释放key的内存,并不是每次必定异步释放内存的.

开启lazy-free后,Redis在释放一个key的内存时,首先会评估代价,如果释放内存的代价很小,那么就直接在主线程中操作了,没必要放到异步线程中执行（不同线程传递数据也会有性能消耗）.

什么情况才会真正异步释放内存？这和key的类型、编码方式、元素数量都有关系（详细可参考源码中的lazyfreeGetFreeEffort函数）：

a) 当Hash/Set底层采用哈希表存储（非ziplist/int编码存储）时,并且元素数量超过64个
b) 当ZSet底层采用跳表存储（非ziplist编码存储）时,并且元素数量超过64个
c) 当List链表节点数量超过64个（注意,不是元素数量,而是链表节点的数量,List的实现是在每个节点包含了若干个元素的数据,这些元素采用ziplist存储）

只有以上这些情况,在删除key释放内存时,才会真正放到异步线程中执行,其他情况一律还是在主线程操作.

也就是说String（不管内存占用多大）、List（少量元素）、Set（int编码存储）、Hash/ZSet（ziplist编码存储）这些情况下的key在释放内存时,依旧在主线程中操作.

可见,即使开启了lazy-free,String类型的bigkey,在删除时依旧有阻塞主线程的风险.所以,即便Redis提供了lazy-free,我建议还是尽量不要在Redis中存储bigkey.

个人理解Redis在设计评估释放内存的代价时,不是看key的内存占用有多少,而是关注释放内存时的工作量有多大.从上面分析基本能看出,如果需要释放的内存是连续的,
Redis作者认为释放内存的代价比较低,就放在主线程做.如果释放的内存不连续（大量指针类型的数据）,这个代价就比较高,所以才会放在异步线程中去执行.


```

```
redis-cli --intrinsic-latency 120       用来监测和统计测试期间内的最大延迟  服务端运行 观察到的延迟只是服务器本地直接运行的结果, 没有考虑到网络环境带来的延时
redis-cli --latency-history -i 1  
redis-cli --latency  

```

#### scan 数据一致保证

```
采用*高位进位法*的方式遍历哈希桶
扩容 无影响
缩影 有可能键会重复获取到


SCAN $cursor COUNT $count    执行SCAN $cursor COUNT $count时一次最多返回count个数的key,数量不会超过count.

使用HSCAN/SSCAN/ZSCAN命令,返回的元素数量与执行SCAN逻辑可能不同.
但Hash/Set/Sorted Set元素数量比较少时,底层会采用intset/ziplist方式存储,如果以这种方式存储,在执行HSCAN/SSCAN/ZSCAN命令时,会无视count参数,
直接把所有元素一次性返回,也就是说,得到的元素数量是会大于count参数的.当底层转为哈希表或跳表存储时,才会真正使用发count参数,最多返回count个元素.


```

#### AOF

```
AOF写回策略       执行的系统调用
no                  调用write写日志文件,由操作系统周期性地将日志写回磁盘
everysec            每秒调用一次fsync,将日志写回磁盘,子进程 干活
always              每秒执行一个操作,就调用一次fsync[磁盘压力大时,会阻塞主线程] 将日志写回磁盘  子进程 干活         

以及周期性地AOF重写 子进程
```

#### 操作系统：内存大页

```
除了内存 swap,还有一个和内存相关的因素,即内存大页机制（Transparent Huge Page, THP）,也会影响 Redis 性能.

Linux 内核从 2.6.38 开始支持内存大页机制,该机制支持 2MB 大小的内存页分配,而常规的内存页分配是按 4KB 的粒度来执行的.

很多人都觉得：“Redis 是内存数据库,内存大页不正好可以满足 Redis 的需求吗？而且在分配相同的内存量时,内存大页还能减少分配次数,
不也是对 Redis 友好吗?”

其实,系统的设计通常是一个取舍过程,我们称之为 trade-off.很多机制通常都是优势和劣势并存的.Redis 使用内存大页就是一个典型的例子.

虽然内存大页可以给 Redis 带来内存分配方面的收益,但是,不要忘了,Redis 为了提供数据可靠性保证,需要将数据做持久化保存.这个写入过程由额外的线程执行,
所以,此时,Redis 主线程仍然可以接收客户端写请求.客户端的写请求可能会修改正在进行持久化的数据.
在这一过程中,Redis 就会采用写时复制机制,也就是说,一旦有数据要被修改,Redis 并不会直接修改内存中的数据,而是将这些数据拷贝一份,
然后再进行修改.

如果采用了内存大页,那么,即使客户端请求只修改 100B 的数据,Redis 也需要拷贝 2MB 的大页.
相反,如果是常规内存页机制,只用拷贝 4KB.两者相比,你可以看到,当客户端请求修改或新写入数据较多时,内存大页机制将导致大量的拷贝,
这就会影响 Redis 正常的访存操作,最终导致性能变慢.

那该怎么办呢？很简单,关闭内存大页,就行了.
首先,我们要先排查下内存大页.方法是：在 Redis 实例运行的机器上执行如下命令:

cat /sys/kernel/mm/transparent_hugepage/enabled

如果执行结果是 always,就表明内存大页机制被启动了;如果是 never,就表示,内存大页机制被禁止.
在实际生产环境中部署时,我建议你不要使用内存大页机制,操作也很简单,只需要执行下面的命令就可以了：

echo never > /sys/kernel/mm/transparent_hugepage/enabled

```

rss 表示 resident set size

主库上的从库输出缓冲区（slave client-output-buffer）不算进maxmemory repl_backlog算进maxmemory

#### key淘汰

- 在设置了过期时间的数据中进行淘汰
    - volatile-random
    - volatile-ttl
    - volatile-lru
    - volatile-lfu 如果一个键值对被删除策略选中了,即使它的过期时间还没到,也需要被删除. 如果它的过期时间到了但未被策略选中,同样也会被删除.
- 在所有数据范围内进行淘汰
    - allkeys-lru
    - allkeys-random
    - allkeys-lfu

简单举个例子,假设 lfu_decay_time 取值为 1,如果数据在 N 分钟内没有被访问,那么它的访问次数就要减 N.如果 lfu_decay_time
取值更大,
那么相应的衰减值会变小,衰减效果也会减弱.所以,如果业务应用中有短时高频访问的数据的话,建议把 lfu_decay_time 值设置为
1,这样一来, LFU
策略在它们不再被访问后,会较快地衰减它们的访问次数,尽早把它们从缓存中淘汰出去,避免缓存污染. a) counter值越大,递增概率越低
b) lfu-log-factor设置越大,递增概率越低 b)
lfu-decay-time设置越大,衰减速度越慢

### embstr类型的字符串长度为啥最大是44？

```
目前的x86体系下,一般的缓存行大小是64字节,redis为了一次能加载完成,因此采用64自己作为embstr类型(保存redisObject)的最大长度.
embstr 存储结构是redisObject、SDS连续存储
- redisObject：16 个字节
- SDS：sdshdr8（3 个字节）+ SDS 字符数组（N 字节 + \0 结束符 1 个字节）

64-16-3-1=44

```

### ziplist -> quicklist -> listpack

```
ziplist 保存 Hash 或 Sorted Set 数据时,都会在 redis.conf 文件中,
通过 hash-max-ziplist-entries 和 zset-max-ziplist-entries 两个参数,来控制保存在 ziplist 中的元素个数.


ziplist 缺点
  1、不能保存过多的元素,否则访问性能会降低
  2、新增或修改数据,ziplist 占用的内存空间还需要重新分配; 
    可能会导致后续元素的 prevlen 占用空间都发生变化,从而引起连锁更新问题. (N次重新分配內存) 
    [由ziplist的结构决定的,保存了前一个entry的长度]
  
quicklist [quicklistNode]
  实际上是一个链表,每个元素是 ziplist
  通过分段,避免连锁更新,只要在每个元素上更新就行
  增加了quicklistNode内存开销
  

listpack
  使用了多种编码方式,来表示不同长度的数据,包括整数和字符串
  与ziplist结构类型,但保存了本entry的问题,扩展内存时,不会重分配N次内存,因为其余entry长度已经固定了
```

### Stream

```
Radix Tree 基数树

消息 ID 是作为 Radix Tree 中的 key,消息具体数据是使用 listpack 保存,并作为 value 和消息 ID 一起保存到 Radix Tree 中.

```

### RDB

```od -A x -t x1c -v dump.rdb```

server.ipfd

- 1:  tcp4 0 0  *.6379                 *.*                    LISTEN
- 2:  tcp6 0 0  *.6379                 *.*                    LISTEN

# RESP

    在RESP中,总共定义了5种数据类型,分别是Simple String、Errors、Intergers、Bulk Strings和Arrays,第一个字节与数据类型的映射关系如下：
    
    对于Simple Strings类型,第一个字节为+
    Errors类型,第一个字节为-
    Integers类型,第一个字节为:
    Bulk Strings类型,第一个字节为$
    Arrays类型,第一个字节为*
    
    在 protocol 2 中, 长度均使用 "*" 
    在 protocol 3 中, ArrayLength 使用 "*", MapLength 使用 "%", SetLength 使用 "~", AttributeLength 使用 "|", PushLength 使用 ">"

https://www.cnblogs.com/phyger/p/14357609.html
https://www.cnblogs.com/phyger/p/14068735.html
https://www.cnblogs.com/phyger/p/14331064.html
https://www.cnblogs.com/phyger/p/14068656.html
https://www.cnblogs.com/coloz/p/13812845.html
https://blog.csdn.net/weixin_42282999/article/details/114806618
https://blog.csdn.net/s_p_y_s/article/details/107724399
https://blog.csdn.net/qq_29235677/article/details/121475204
https://blog.csdn.net/Linuxhus/article/details/124898697
https://xie.infoq.cn/article/cc74f9e1d190073ba7077952d
https://blog.csdn.net/qq_38571892/article/details/115626326
https://www.jianshu.com/p/e8a2c727da66
https://blog.csdn.net/yh88623131/article/details/125032023
https://www.cnblogs.com/erjiujiqing/p/13695359.html
https://blog.csdn.net/bohu83/category_6178110_2.html
https://blog.csdn.net/wanger5354/article/details/123243467
https://github.com/huangz1990/redis-3.0-annotated.git
https://blog.csdn.net/u011039332/article/details/114626174
https://blog.csdn.net/u012422440/article/details/103536687

## 事务

    当集群不可用、key 找不到对应的 slot、key 不在当前实例中、操作的 key 不在同一个 slot 中,或者 key 正在迁移等这几种情况发生时,事务的执行都会被放弃


## 
- 解析客户端命令 processInlineBuffer、processMultibulkBuffer

# todo

server.clients_to_close 操作 为啥要加锁？

beforeSleep --> handleClientsWithPendingReadsUsingThreads[将每个阻塞读的客户端分给每个线程]    --| |-->
startThreadedIO                                                          
|--> IOThreadMain 死循环,原子获取待处理数量 执行 readQueryFromClient函数

readQueryFromClient --> postponeClientRead --↑ --> processInputBuffer --> processInlineBuffer -->| -->
processMultibulkBuffer -->| --> processCommandAndResetClient --> resetClient -->| |-> processCommand -->
updateClientMemUsage |-> lookupCommand

# todo

addACLLogEntry ACLSelectorCheckCmd
- https://blog.csdn.net/qq_29235677/article/details/121475204
- https://zhuanlan.zhihu.com/p/341434214
- https://blog.csdn.net/weixin_40785301/article/details/122629052
- https://blog.csdn.net/cristianoxm/article/details/106860233
- https://blog.csdn.net/qq_45800640/article/details/119189270

PROPAGATE_AOF与PROPAGATE_REPL的区别

// lua执行时，为什么会接收新的请求 src/networking.c:2510
