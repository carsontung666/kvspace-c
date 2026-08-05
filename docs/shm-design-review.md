# About kvspace-c SHM ArtBump 方案：

## 1. 目录索引: section-7 和 section-9 是两套方案
section-7.2 说目录不用存名单,树能推出来。section-9 目录存名单。有矛盾。
- 方案：目录名单不要每次重新分配,改成预留 capacity + 末尾追加(像c++ vector 的 push_back),满了翻倍。每加一个子项都要把整份名单读出来、拷到一块更大的内存、再写回去,单次 O(k)。

## 2. section-6 的扩容代码有并发 bug
bump_alloc 的锁只保护了扩容,没保护发地址。
- 方案:删掉这把 resize_mutex,靠 section-8 已有的全局锁。

## 3. section-8 的锁写了两种，其中一种没有崩溃恢复
进程拿着锁死了,锁就永远锁着,后面全卡死,region 报废。
- 方案：都用robust mutex。读并发以后靠版本号 + 乐观读,不靠 rwlock。

## 4. Notify 的语义和 redis 对不上
Notify 是进程间传消息的。A 发,B 等着收。
redis 那边的行为:发出去的消息进队列。没人等就留着,下一个人来取还能拿到。发多条就排队,一人取一条。（就是说 发的人和收的人不用同时在场，B 早到晚到都行。早到就阻塞等着,晚到就直接把已经在那儿的拿走。）
- 方案：环形队列 + futex 唤醒


  
