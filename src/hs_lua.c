/**
 * hs_lua.c  –  LuaJIT bindings for request and response
 *
 * Each worker thread owns one lua_State.
 * The handler is called as:  handle(req, res)
 *   req.method, req.url  (fields via __index)
 *   req:header(name)     (method)
 *   req:body()           (method)
 *   res:status(code)
 *   res:header(name,val)
 *   res:body(str)
 *   res:send()           ← MUST be called
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <jemalloc/jemalloc.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <llhttp.h>

#include "hs_lua.h"
#include "hs_conn.h"
#include "hs_http.h"
#include "httpserver.h"

struct hs_lua_state { lua_State *L; };

#define MT_REQ "hs.Request"
#define MT_RES "hs.Response"

/* ── request methods ─────────────────────────────────────────────────────── */
static int lreq_header(lua_State *L)
{
    hs_conn_t  **pp = (hs_conn_t **)luaL_checkudata(L, 1, MT_REQ);
    const char  *n  = luaL_checkstring(L, 2);
    hs_parsed_req_t *r = &(*pp)->req;
    for (int i = 0; i < r->nheaders; i++)
        if (strcasecmp(r->headers[i].name, n) == 0)
            { lua_pushstring(L, r->headers[i].value); return 1; }
    lua_pushnil(L); return 1;
}
static int lreq_body(lua_State *L)
{
    hs_conn_t **pp = (hs_conn_t **)luaL_checkudata(L, 1, MT_REQ);
    size_t len = 0;
    const char *b = hs_req_body((hs_request_t *)*pp, &len);
    lua_pushlstring(L, b, len); return 1;
}
static int lreq_index(lua_State *L)
{
    const char *k = lua_tostring(L, 2);
    if (!k) { lua_pushnil(L); return 1; }
    if (!strcmp(k, "method")) {
        hs_conn_t **pp = (hs_conn_t **)luaL_checkudata(L, 1, MT_REQ);
        lua_pushstring(L, llhttp_method_name((llhttp_method_t)(*pp)->req.method));
        return 1;
    }
    if (!strcmp(k, "url")) {
        hs_conn_t **pp = (hs_conn_t **)luaL_checkudata(L, 1, MT_REQ);
        lua_pushstring(L, (*pp)->req.url); return 1;
    }
    luaL_getmetatable(L, MT_REQ); lua_getfield(L, -1, k); return 1;
}

/* ── response methods ────────────────────────────────────────────────────── */
static int lres_status(lua_State *L)
{
    hs_response_t **pp = (hs_response_t **)luaL_checkudata(L, 1, MT_RES);
    hs_res_status(*pp, (int)luaL_checkinteger(L, 2)); return 0;
}
static int lres_header(lua_State *L)
{
    hs_response_t **pp = (hs_response_t **)luaL_checkudata(L, 1, MT_RES);
    hs_res_header(*pp, luaL_checkstring(L,2), luaL_checkstring(L,3)); return 0;
}
static int lres_body(lua_State *L)
{
    hs_response_t **pp = (hs_response_t **)luaL_checkudata(L, 1, MT_RES);
    size_t len; const char *s = luaL_checklstring(L, 2, &len);
    hs_res_body(*pp, s, len); return 0;
}
static int lres_send(lua_State *L)
{
    hs_response_t **pp = (hs_response_t **)luaL_checkudata(L, 1, MT_RES);
    hs_res_send(*pp); return 0;
}

static const luaL_Reg lreq_m[] = {{"header",lreq_header},{"body",lreq_body},{NULL,NULL}};
static const luaL_Reg lres_m[] = {{"status",lres_status},{"header",lres_header},
                                    {"body",lres_body},{"send",lres_send},{NULL,NULL}};

static void reg_types(lua_State *L)
{
    luaL_newmetatable(L, MT_REQ);
    lua_pushcfunction(L, lreq_index); lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, lreq_m); lua_pop(L, 1);

    luaL_newmetatable(L, MT_RES);
    lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, lres_m); lua_pop(L, 1);
}

/* ── public API ──────────────────────────────────────────────────────────── */
hs_lua_state_t *hs_lua_state_new(const char *script)
{
    hs_lua_state_t *ls = (hs_lua_state_t *)je_calloc(1, sizeof(*ls));
    if (!ls) return NULL;
    ls->L = luaL_newstate();
    if (!ls->L) { je_free(ls); return NULL; }
    luaL_openlibs(ls->L);
    reg_types(ls->L);
    if (luaL_loadfile(ls->L, script) || lua_pcall(ls->L, 0, 0, 0)) {
        fprintf(stderr, "[lua] load error: %s\n", lua_tostring(ls->L, -1));
        lua_close(ls->L); je_free(ls); return NULL;
    }
    lua_getglobal(ls->L, "handle");
    if (!lua_isfunction(ls->L, -1)) {
        fprintf(stderr, "[lua] missing global 'handle'\n");
        lua_close(ls->L); je_free(ls); return NULL;
    }
    lua_pop(ls->L, 1);
    return ls;
}

void hs_lua_state_free(hs_lua_state_t *ls)
{
    if (ls) { if (ls->L) lua_close(ls->L); je_free(ls); }
}

int hs_lua_call_handler(hs_lua_state_t *ls,
                         struct hs_conn *conn, struct hs_response *res)
{
    lua_State *L = ls->L;
    lua_getglobal(L, "handle");

    hs_conn_t     **rp = (hs_conn_t     **)lua_newuserdata(L, sizeof(void *));
    *rp = conn;
    luaL_getmetatable(L, MT_REQ); lua_setmetatable(L, -2);

    hs_response_t **wp = (hs_response_t **)lua_newuserdata(L, sizeof(void *));
    *wp = res;
    luaL_getmetatable(L, MT_RES); lua_setmetatable(L, -2);

    if (lua_pcall(L, 2, 0, 0)) {
        fprintf(stderr, "[lua] runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}
