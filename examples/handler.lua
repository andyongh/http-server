--[[  handler.lua  –  Lua HTTP handler (one lua_State per worker thread)  ]]

local hits = 0   -- thread-local counter

local routes = {}

routes["GET /"] = function(req, res)
    hits = hits + 1
    res:status(200)
    res:header("Content-Type", "text/plain")
    res:body("hello from LuaJIT! (thread hits: " .. hits .. ")\n")
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
    res:status(200); res:body("pong\n"); res:send()
end

function handle(req, res)
    local key = req.method .. " " .. (req.url:match("^[^?]+") or req.url)
    local fn  = routes[key]
    if fn then
        local ok, err = pcall(fn, req, res)
        if not ok then
            io.stderr:write("[lua] " .. tostring(err) .. "\n")
            res:status(500); res:body("Internal Server Error\n"); res:send()
        end
    else
        res:status(404)
        res:header("Content-Type", "text/plain")
        res:body("Not Found: " .. req.url .. "\n")
        res:send()
    end
end
