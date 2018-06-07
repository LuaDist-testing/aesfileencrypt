#include "lua.h"
#include "fileenc/fileenc.h"
#include "l52util.h"
#include <assert.h>

static const char *L_FCRYPT_CTX = "AES File Encription";

#define FLAG_TYPE unsigned char
#define FLAG_DESTROYED (FLAG_TYPE)1 << 0
#define FLAG_OPEN      (FLAG_TYPE)1 << 1

#define MAX_MAC_LENGTH 10

typedef void (*pfcrypt_encrypt)(unsigned char data[], unsigned int data_len, fcrypt_ctx cx[1]);

typedef struct l_fcrypt_ctx_tag{
  fcrypt_ctx ctx[1];
  FLAG_TYPE flags;
  int ud_ref;
  int cb_ref;
  size_t buf_len;
  char buf[1];
}l_fcrypt_ctx ;

static l_fcrypt_ctx *l_getctx_at (lua_State *L, int i) {
  l_fcrypt_ctx *ctx = (l_fcrypt_ctx *)lutil_checkudatap (L, i, L_FCRYPT_CTX);
  luaL_argcheck (L, ctx != NULL, 1, "AES File Encription expected");
  luaL_argcheck (L, !(ctx->flags & FLAG_DESTROYED), 1, "AES File Encription is destroyed");
  return ctx;
}

static int l_fcrypt_new(lua_State *L){
  size_t buf_len = luaL_optint(L, 1, 4096);
  l_fcrypt_ctx *ctx = (l_fcrypt_ctx *)lutil_newudatap_impl(L, sizeof(l_fcrypt_ctx) + buf_len - 1, L_FCRYPT_CTX);
  memset(ctx, 0, sizeof(l_fcrypt_ctx) + buf_len - 1);
  ctx->buf_len = buf_len;
  ctx->cb_ref = LUA_NOREF;
  ctx->ud_ref = LUA_NOREF;
  return 1;
}

static int l_fcrypt_destroy(lua_State *L){
  l_fcrypt_ctx *ctx = (l_fcrypt_ctx *)lutil_checkudatap (L, 1, L_FCRYPT_CTX);
  luaL_argcheck (L, ctx != NULL, 1, "AES File Encription expected");

  if(ctx->flags & FLAG_DESTROYED) return 0;

  luaL_unref(L, LUA_REGISTRYINDEX, ctx->cb_ref);
  luaL_unref(L, LUA_REGISTRYINDEX, ctx->ud_ref);
  ctx->ud_ref = ctx->cb_ref = LUA_NOREF;

  if(ctx->flags & FLAG_OPEN){
    unsigned char mac[MAX_MAC_LENGTH];
    assert(MAX_MAC_LENGTH >= MAC_LENGTH(ctx->ctx->mode));
    fcrypt_end(mac, ctx->ctx);
    ctx->flags &= ~FLAG_OPEN;
  }

  ctx->flags |= FLAG_DESTROYED;
  return 0;
}

static int l_fcrypt_destroyed(lua_State *L){
  l_fcrypt_ctx *ctx = (l_fcrypt_ctx *)lutil_checkudatap (L, 1, L_FCRYPT_CTX);
  luaL_argcheck (L, ctx != NULL, 1, "AES File Encription expected");
  lua_pushboolean(L, ctx->flags & FLAG_DESTROYED);
  return 1;
}

static int l_fcrypt_open(lua_State *L){
  l_fcrypt_ctx *ctx = l_getctx_at(L, 1);
  int mode = luaL_checkint(L, 2);
  size_t pwd_len; const char *pwd = luaL_checklstring(L, 3, &pwd_len);
  size_t slt_len; const char *slt = luaL_checklstring(L, 4, &slt_len);
  unsigned char pver[PWD_VER_LENGTH]; int ret;
  lua_settop(L, 4);

  luaL_argcheck(L, !(ctx->flags & FLAG_OPEN),    1, "context already open");
  luaL_argcheck(L, (mode >= 1) && (mode <= 3),   2, "invalid mode");
  luaL_argcheck(L, slt_len >= SALT_LENGTH(mode), 4, "invalid salt length");

  ret = fcrypt_init(mode, pwd, pwd_len, slt, pver, ctx->ctx);
  if(ret != GOOD_RETURN){
    lua_pushnil(L);
    lua_pushnumber(L, ret);
    return 2;
  }

  lua_pushlstring(L, pver, PWD_VER_LENGTH);
  ctx->flags |= FLAG_OPEN;
  return 2;
}

static int l_fcrypt_opened(lua_State *L){
  l_fcrypt_ctx *ctx = l_getctx_at(L, 1);
  lua_pushboolean(L, ctx->flags & FLAG_OPEN);
  return 1;
}

static int l_fcrypt_set_writer(lua_State *L){
  l_fcrypt_ctx *ctx = l_getctx_at(L, 1);

  if(ctx->ud_ref != LUA_NOREF){
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->ud_ref);
    ctx->ud_ref = LUA_NOREF;
  }

  if(ctx->cb_ref != LUA_NOREF){
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->cb_ref);
    ctx->cb_ref = LUA_NOREF;
  }

  if(lua_gettop(L) >= 3){// reader + context
    lua_settop(L, 3);
    luaL_argcheck(L, !lua_isnil(L, 2), 2, "no writer present");
    ctx->ud_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctx->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    assert(1 == lua_gettop(L));
    return 1;
  }

  lua_settop(L, 2);

  if( lua_isnoneornil(L, 2) ){
    lua_pop(L, 1);
    assert(1 == lua_gettop(L));
    return 1;
  }

  if(lua_isfunction(L, 2)){
    ctx->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    assert(1 == lua_gettop(L));
    return 1;
  }

  if(lua_isuserdata(L, 2) || lua_istable(L, 2)){
    lua_getfield(L, 2, "write");
    luaL_argcheck(L, lua_isfunction(L, -1), 2, "write method not found in object");
    ctx->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctx->ud_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    assert(1 == lua_gettop(L));
    return 1;
  }

  lua_pushliteral(L, "invalid writer type");
  return lua_error(L);
}

static int l_fcrypt_get_writer(lua_State *L){
  l_fcrypt_ctx *ctx = l_getctx_at(L, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->cb_ref);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->ud_ref);
  return 2;
}

static int l_fcrypt_push_cb(lua_State *L, l_fcrypt_ctx *ctx){
  assert(ctx->cb_ref != LUA_NOREF);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->cb_ref);
  if(ctx->ud_ref != LUA_NOREF){
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->ud_ref);
    return 2;
  }
  return 1;
}

static int l_fcrypt_close(lua_State *L){
  l_fcrypt_ctx *ctx = l_getctx_at(L, 1);
  unsigned char mac[MAX_MAC_LENGTH];
  luaL_argcheck(L, ctx->flags & FLAG_OPEN, 1, "context is close");
  assert(MAX_MAC_LENGTH >= MAC_LENGTH(ctx->ctx->mode));

  fcrypt_end(mac, ctx->ctx);
  ctx->flags &= ~FLAG_OPEN;
  lua_pushlstring(L, mac, MAC_LENGTH(ctx->ctx->mode));
  return 1;
}

static int l_fcrypt_encrypt_impl(lua_State *L, pfcrypt_encrypt encrypt){
  l_fcrypt_ctx *ctx = l_getctx_at(L, 1);
  size_t len; const char *data = luaL_checklstring(L, 2, &len);
  luaL_Buffer buffer; int n;
  const char *b, *e; int use_buffer = (ctx->cb_ref == LUA_NOREF)?1:0;
  luaL_argcheck(L, ctx->flags & FLAG_OPEN, 1, "context is close");
  if(use_buffer) luaL_buffinit(L, &buffer);
  else n = l_fcrypt_push_cb(L, ctx);
  for(b = data, e = data + len; b < e; b += ctx->buf_len){
    size_t left = e - b;
    if(left > ctx->buf_len) left = ctx->buf_len;

    memcpy(ctx->buf, b, left);
    encrypt(ctx->buf, left, ctx->ctx);
    if(use_buffer) luaL_addlstring(&buffer, ctx->buf, left);
    else{
      int i, top = lua_gettop(L);
      for(i = n; i > 0; --i) lua_pushvalue(L, top - i + 1);
      lua_pushlstring(L, ctx->buf, left);
      lua_call(L, n, 0);
    }
  }

  if(use_buffer){
    luaL_pushresult(&buffer);
    return 1;
  }
  return 0;
}

static int l_fcrypt_encrypt(lua_State *L){
  return l_fcrypt_encrypt_impl(L, fcrypt_encrypt);
}

static int l_fcrypt_decrypt(lua_State *L){
  return l_fcrypt_encrypt_impl(L, fcrypt_decrypt);
}

static const struct luaL_Reg l_fcrypt_lib[] = {
  "new", l_fcrypt_new,
  NULL, NULL
};

static const struct luaL_Reg l_fcrypt_meth[] = {
  "__gc",       l_fcrypt_destroy,
  "open",       l_fcrypt_open,
  "destroy",    l_fcrypt_destroy,
  "opened",     l_fcrypt_opened,
  "destroyed",  l_fcrypt_destroyed,
  "set_writer", l_fcrypt_set_writer,
  "get_writer", l_fcrypt_get_writer,
  "encrypt",    l_fcrypt_encrypt,
  "decrypt",    l_fcrypt_decrypt,
  "close",      l_fcrypt_close,
  NULL, NULL
};

int luaopen_AesFileEncrypt(lua_State*L){
  lutil_createmetap(L, L_FCRYPT_CTX, l_fcrypt_meth, 0);
  lua_newtable(L);
  luaL_setfuncs(L, l_fcrypt_lib, 0);
  return 1;
}