/**
 * hs_lua.h  –  per-worker LuaJIT Lua state
 */
#pragma once
struct hs_conn; struct hs_response;
typedef struct hs_lua_state hs_lua_state_t;

hs_lua_state_t *hs_lua_state_new(const char *script);
void            hs_lua_state_free(hs_lua_state_t *ls);
int             hs_lua_call_handler(hs_lua_state_t *ls,
                                    struct hs_conn    *conn,
                                    struct hs_response *res);
