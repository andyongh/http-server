--[[  handler.lua  –  Lua HTTP handler  (v0.4-lite)

  Each worker thread has its own lua_State (no shared state between threads).
  Thread-local state (hit counter, etc.) is per-thread.

  The coroutine task queue is available as hs_queue_push(coroutine).
  Coroutines are resumed between requests by hs_lua_state_tick().

  Hot-reload:
    When using lua_dir, this file is automatically reloaded when any .lua
    file in the directory changes.  All previous state is discarded.
]]

local hits = 0   -- thread-local request counter

local routes = {}

routes["GET /"] = function(req, res)
    hits = hits + 1
    res:status(200)
    res:header("Content-Type", "text/plain")
    res:body("hello from LuaJIT v0.4-lite! (thread hits: " .. hits .. ")\n")
    res:send()
end

routes["POST /echo"] = function(req, res)
    local b = req:body()
    res:status(200)
    res:header("Content-Type", "text/plain")
    res:body(#b > 0 and b or "(empty)\n")
    res:send()
end

routes["GET /ping"] = function(req, res)
    res:status(200)
    res:header("Content-Type", "text/plain")
    res:body("pong\n")
    res:send()
end

routes["GET /compute"] = function(req, res)
    -- Simulate CPU-heavy work
    local sum = 0
    for i = 1, 1000000 do
        sum = sum + i
    end
    res:status(200)
    res:header("Content-Type", "text/plain")
    res:body("computed sum: " .. sum .. " (in_worker: " .. tostring(req:in_worker()) .. ")\n")
    res:send()
end

--[[ Coroutine task queue demo:
     GET /async returns 200 immediately and schedules a background task.
     The task is resumed by hs_lua_state_tick() on the next request.
]]
routes["GET /async"] = function(req, res)
    -- Schedule a background coroutine
    local task = coroutine.create(function()
        -- step 1
        io.stderr:write("[async] task step 1\n")
        coroutine.yield()
        -- step 2 (resumed on next tick)
        io.stderr:write("[async] task step 2\n")
    end)
    hs_queue_push(task)

    res:status(200)
    res:header("Content-Type", "text/plain")
    res:body("async task scheduled\n")
    res:send()
end

function handle(req, res)
    local path = req.url:match("^[^?]+") or req.url
    if path == "/compute" and not req:in_worker() then
        return "worker"
    end

    local key = req.method .. " " .. path
    local fn  = routes[key]
    if fn then
        local ok, err = pcall(fn, req, res)
        if not ok then
            io.stderr:write("[lua] " .. tostring(err) .. "\n")
            res:status(500)
            res:body("Internal Server Error\n")
            res:send()
        end
    else
        res:status(404)
        res:header("Content-Type", "text/plain")
        res:body("Not Found: " .. req.url .. "\n")
        res:send()
    end
    return "inline"
end
