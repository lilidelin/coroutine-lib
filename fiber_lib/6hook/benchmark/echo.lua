-- wrk 压测 HTTP server 用的 lua 脚本
-- server 返回固定 HTTP 响应，wrk 普通请求即可
-- 用法：wrk -t4 -c2000 -d30s -s echo.lua http://127.0.0.1:PORT/

-- 用小 body 的 POST，让 server recv 到数据后返回
wrk.method = "POST"
wrk.body   = "hello"
wrk.headers["Content-Type"] = "text/plain"
