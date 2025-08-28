
1. 构建并运行：
```
mkdir build && cd build
cmake ..
make -j
./service_discovery
```

2. HTTP 注册：
```
curl -X POST http://localhost:8080/register -d '{"service":"user","address":"127.0.0.1","port":9001,"id":"user-http-1","weight":3}' -H 'Content-Type: application/json'
```

3. TCP 长连接注册（保持连接并发送心跳）：
```
# 建议写个小脚本或使用 Python 的 socket 来保持连接并定期发送注册/心跳
# 示例：用 netcat 发送一次注册（不保持）
printf '{"service":"user","address":"10.0.0.5","port":9001,"id":"tcp-user-1","weight":2}
' | nc localhost 9090

# 心跳示例（若使用持久连接的客户端）发送：
printf '{"type":"heartbeat","id":"tcp-user-1","ttl":300}
' | nc localhost 9090
```

4. 查看实例列表：
```
curl 'http://localhost:8080/list?service=user'
```

5. 使用平滑加权 LB 或延迟感知 LB 进行发现：
```
curl 'http://localhost:8080/discover?service=user&strategy=smooth_weighted'
curl 'http://localhost:8080/discover?service=user&strategy=latency_aware'
```

6. 使用 server-side proxy 并观察延迟被记录：
```
curl 'http://localhost:8080/proxy/user/health?strategy=smooth_weighted'
# 代理会在代理完成后把实例的 avg_latency_us 更新，可通过 /list 查看
curl 'http://localhost:8080/list?service=user'
```