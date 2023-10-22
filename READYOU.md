# 编译

```
yarn build
run yarn wsrun --serial --fast-exit build
```
这是一个在package.json中指定的命令

见`package.json`
- `wsrun`: Run npm scripts or custom commands in a yarn workspace
- `fast-exit`: If at least one script exits with code > 0, abort
- `stage`: Run in stages: start with packages that have no deps
- `serial`: Same as "stages" but with no parallelism at the stage level

app/package.json 依赖中有kungfu-core，所以core应该是先编译的

# 