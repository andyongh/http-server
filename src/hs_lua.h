/**
 * hs_lua.h  –  per-worker LuaJIT Lua state  (v0.4-lite)
 */
#pragma once
struct hs_conn; struct hs_response;
typedef struct hs_lua_state hs_lua_state_t;

hs_lua_state_t *hs_lua_state_new(const char *script);
void            hs_lua_state_free(hs_lua_state_t *ls);
void            hs_lua_state_tick(hs_lua_state_t *ls);  /* tick coro queue */
int             hs_lua_call_handler(hs_lua_state_t *ls,
                                    struct hs_conn    *conn,
                                    struct hs_response *res);
