-- tests/wrk_verify.lua
-- ─────────────────────────────────────────────────────────────────────────────
-- A wrk Lua script to verify request/response isolation under high concurrency.
-- It generates unique tokens per request, sends them via POST /echo,
-- and verifies that the echoed response body exactly matches an outstanding
-- token sent by the same thread.
-- ─────────────────────────────────────────────────────────────────────────────

local thread_id = 0
local req_counter = 0
local outstanding = {}
local errors = 0
local responses_count = 0

setup = function(thread)
    -- Assign a unique ID to each thread
    thread:set("thread_id", thread.id)
end

init = function(args)
    req_counter = 0
    outstanding = {}
    errors = 0
    responses_count = 0
end

request = function()
    req_counter = req_counter + 1
    -- Generate a unique token
    local token = "t_" .. thread_id .. "_" .. req_counter
    outstanding[token] = true
    
    -- We can print a debug message every 10000 requests to show progress
    if req_counter % 10000 == 0 then
        -- io.write(string.format("[thread %d] sent %d requests\n", thread_id, req_counter))
    end

    return wrk.format("POST", "/echo", nil, token)
end

response = function(status, headers, body)
    responses_count = responses_count + 1
    
    if status ~= 200 then
        errors = errors + 1
        io.stderr:write(string.format("ERROR: non-200 status %d\n", status))
        return
    end

    if not body or body == "" then
        errors = errors + 1
        io.stderr:write("ERROR: empty response body\n")
        return
    end

    -- Trim any trailing newline from the body if the server appends one
    -- (Our C /echo handler doesn't append a newline, but Lua echo might, so let's handle it)
    local cleaned_body = body:gsub("\r", ""):gsub("\n", "")

    if outstanding[cleaned_body] then
        -- Valid response, remove from outstanding set
        outstanding[cleaned_body] = nil
    else
        errors = errors + 1
        io.stderr:write(string.format(
            "ERROR: mismatch/cross-talk! Received token '%s' which is not outstanding in thread %d.\n",
            cleaned_body, thread_id
        ))
    end
end

done = function(summary, latency, requests)
    -- Check if there are any outstanding requests that were never answered
    local left_over = 0
    for k, v in pairs(outstanding) do
        left_over = left_over + 1
    end

    print("")
    print(string.format("=== Verification Thread %d Summary ===", thread_id))
    print(string.format("  Requests sent:      %d", req_counter))
    print(string.format("  Responses checked:  %d", responses_count))
    print(string.format("  Errors/Mismatches:  %d", errors))
    print(string.format("  Unanswered requests: %d", left_over))
    
    if errors > 0 then
        print("  STATUS: FAILED")
    else
        print("  STATUS: SUCCESS")
    end
    print("=======================================")
end
