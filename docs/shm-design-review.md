About kvspace-c SHM 方案：

# 1. 目录索引: section-7 和 section-9 是两套方案
section-7.2 说目录不用存名单,树能推出来。section-9 目录存名单。有矛盾。

- 方案：一个子项一个 key,不要用一个 key 装整份名单。
```
现在:  /a/     →  "b\nc\nd"        整份名单打包成一个字符串

改后:  /a/#b   →  (空)
       /a/#c   →  (空)             一个子项一个 key,值为空
       /a/#d   →  (空)
```
