
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "jesy.h"

#ifdef JESY_USE_32BIT_NODE_DESCRIPTOR
  #define JESY_INVALID_INDEX 0xFFFFFFFF
  #define JESY_MAX_VALUE_LEN 0xFFFFFFFF
#else
  #define JESY_INVALID_INDEX 0xFFFF
  #define JESY_MAX_VALUE_LEN 0xFFFF
#endif

#define UPDATE_TOKEN(tok, type_, offset_, size_) \
  tok.type = type_; \
  tok.offset = offset_; \
  tok.length = size_;

#define LOOK_AHEAD(ctx_) ctx_->json_data[ctx_->offset + 1]
#define IS_EOF_AHEAD(ctx_) (((ctx_->offset + 1) >= ctx_->json_size) || \
                            (ctx_->json_data[ctx_->offset + 1] == '\0'))
#define IS_SPACE(c) ((c==' ') || (c=='\t') || (c=='\r') || (c=='\n'))
#define IS_DIGIT(c) ((c >= '0') && (c <= '9'))
#define IS_ESCAPE(c) ((c=='\\') || (c=='\"') || (c=='\/') || (c=='\b') || \
                      (c=='\f') || (c=='\n') || (c=='\r') || (c=='\t') || (c == '\u'))

#define HAS_PARENT(node_ptr) (node_ptr->parent < JESY_INVALID_INDEX)
#define HAS_SIBLING(node_ptr) (node_ptr->sibling < JESY_INVALID_INDEX)
#define HAS_CHILD(node_ptr) (node_ptr->first_child < JESY_INVALID_INDEX)

static struct jesy_element *jesy_find_duplicate_key(struct jesy_context *ctx,
                                                    struct jesy_element *object_node,
                                                    struct jesy_token *key_token);

static struct jesy_element* jesy_allocate(struct jesy_context *ctx)
{
  struct jesy_element *new_element = NULL;

  if (ctx->node_count < ctx->capacity) {
    if (ctx->free) {
      /* Pop the first node from free list */
      new_element = (struct jesy_element*)ctx->free;
      ctx->free = ctx->free->next;
    }
    else {
      assert(ctx->index < ctx->capacity);
      new_element = &ctx->pool[ctx->index];
      ctx->index++;
    }
    /* Setting node descriptors to their default values. */
    memset(&new_element->parent, 0xFF, sizeof(jesy_node_descriptor) * 4);
    ctx->node_count++;
  }
  else {
    ctx->status = JESY_OUT_OF_MEMORY;
  }

  return new_element;
}

static void jesy_free(struct jesy_context *ctx, struct jesy_element *element)
{
  struct jesy_free_node *free_node = (struct jesy_free_node*)element;

  assert(element >= ctx->pool);
  assert(element < (ctx->pool + ctx->capacity));
  assert(ctx->node_count > 0);

  if (ctx->node_count > 0) {
    free_node->next = NULL;
    ctx->node_count--;
    /* prepend the node to the free LIFO */
    if (ctx->free) {
      free_node->next = ctx->free->next;
    }
    ctx->free = free_node;
  }
}

static bool jesy_validate_element(struct jesy_context *ctx, struct jesy_element *element)
{
  assert(ctx);
  assert(element);

  if ((element >= ctx->pool) &&
      ((((void*)element - (void*)ctx->pool) % sizeof(*element)) == 0) &&
      ((element >= ctx->pool) < ctx->capacity)) {
    return true;
  }

  return false;
}

struct jesy_element* jesy_get_parent(struct jesy_context *ctx, struct jesy_element *element)
{
  if (ctx && element && jesy_validate_element(ctx, element)) {
    if (HAS_PARENT(element)) {
      return &ctx->pool[element->parent];
    }
  }
  return NULL;
}

struct jesy_element* jesy_get_sibling(struct jesy_context *ctx, struct jesy_element *element)
{
  if (ctx && element && jesy_validate_element(ctx, element)) {
    if (HAS_SIBLING(element)) {
      return &ctx->pool[element->sibling];
    }
  }
  return NULL;
}

struct jesy_element* jesy_get_child(struct jesy_context *ctx, struct jesy_element *element)
{
  if (ctx && element && jesy_validate_element(ctx, element)) {
    if (HAS_CHILD(element)) {
      return &ctx->pool[element->first_child];
    }
  }
  return NULL;
}

static struct jesy_element* jesy_get_parent_bytype(struct jesy_context *ctx,
                                                   struct jesy_element *element,
                                                   enum jesy_type type)
{
  struct jesy_element *parent = NULL;
  if (ctx && element && jesy_validate_element(ctx, element)) {
    while (element && HAS_PARENT(element)) {
      element = &ctx->pool[element->parent];
      if (element->type == type) {
        parent = element;
        break;
      }
    }
  }
  return parent;
}

static struct jesy_element* jesy_get_structure_parent_node(struct jesy_context *ctx,
                                                           struct jesy_element *element)
{
  struct jesy_element *parent = NULL;
  if (ctx && element && jesy_validate_element(ctx, element)) {
    while (element && HAS_PARENT(element)) {
      element = &ctx->pool[element->parent];
      if ((element->type == JESY_OBJECT) || (element->type == JESY_ARRAY)) {
        parent = element;
        break;
      }
    }
  }
  return parent;
}

static struct jesy_element* jesy_add_element(struct jesy_context *ctx,
                                             struct jesy_element *parent,
                                             uint16_t type,
                                             uint32_t offset,
                                             uint16_t length)
{
  struct jesy_element *new_element = jesy_allocate(ctx);

  if (new_element) {
    new_element->type = type;
    new_element->length = length;
    new_element->value = &ctx->json_data[offset];

    if (parent) {
      new_element->parent = (jesy_node_descriptor)(parent - ctx->pool); /* parent's index */

      if (HAS_CHILD(parent)) {
        struct jesy_element *last = &ctx->pool[parent->last_child];
        last->sibling = (jesy_node_descriptor)(new_element - ctx->pool); /* new_element's index */
      }
      else {
        parent->first_child = (jesy_node_descriptor)(new_element - ctx->pool); /* new_element's index */
      }
      parent->last_child = (jesy_node_descriptor)(new_element - ctx->pool); /* new_element's index */
    }
    else {
      assert(!ctx->root);
      ctx->root = new_element;
    }
  }

  return new_element;
}

static void jesy_delete_element(struct jesy_context *ctx, struct jesy_element *element)
{
  struct jesy_element *iter = NULL;
  struct jesy_element *parent = NULL;
  struct jesy_element *prev = NULL;

  if (!jesy_validate_element(ctx, element)) {
    return;
  }

  iter = element;
  while (true) {
    prev = NULL;
    while (HAS_CHILD(iter)) {
      iter = &ctx->pool[iter->first_child];
    }

    if (HAS_PARENT(iter)) {
      ctx->pool[iter->parent].first_child = iter->sibling;
    }

    if (!HAS_SIBLING(iter) && HAS_PARENT(iter)) {
        ctx->pool[iter->parent].last_child = JESY_INVALID_INDEX;
    }

    jesy_free(ctx, iter);
    iter = jesy_get_parent(ctx, iter);
    continue;
  }
}

static struct jesy_token jesy_get_token(struct jesy_context *ctx)
{
  struct jesy_token token = { 0 };

  while (true) {

    if ((++ctx->offset >= ctx->json_size) || (ctx->json_data[ctx->offset] == '\0')) {
      /* End of data. If token is incomplete, mark it as invalid. */
      if (token.type) {
        token.type = JESY_TOKEN_INVALID;
      }
      break;
    }

    char ch = ctx->json_data[ctx->offset];

    if (!token.type) {

      if (ch == '{') {
        UPDATE_TOKEN(token, JESY_TOKEN_OPENING_BRACKET, ctx->offset, 1);
        break;
      }

      if (ch == '}') {
        UPDATE_TOKEN(token, JESY_TOKEN_CLOSING_BRACKET, ctx->offset, 1);
        break;
      }

      if (ch == '[') {
        UPDATE_TOKEN(token, JESY_TOKEN_OPENING_BRACE, ctx->offset, 1);
        break;
      }

      if (ch == ']') {
        UPDATE_TOKEN(token, JESY_TOKEN_CLOSING_BRACE, ctx->offset, 1);
        break;
      }

      if (ch == ':') {
        UPDATE_TOKEN(token, JESY_TOKEN_COLON, ctx->offset, 1)
        break;
      }

      if (ch == ',') {
        UPDATE_TOKEN(token, JESY_TOKEN_COMMA, ctx->offset, 1)
        break;
      }

      if (ch == '\"') {
        /* Use offset of next symbol since '\"' won't be a part of token. */
        UPDATE_TOKEN(token, JESY_TOKEN_STRING, ctx->offset + 1, 0);
        continue;
      }

      if (IS_DIGIT(ch)) {
        UPDATE_TOKEN(token, JESY_TOKEN_NUMBER, ctx->offset, 1);
        /* NUMBERs do not have dedicated enclosing symbols like STRINGs.
           To prevent the tokenizer to consume too much characters, we need to
           look ahead and stop the process if the jesy_token_type_str character is one of
           EOF, ',', '}' or ']' */
        if (IS_EOF_AHEAD(ctx) ||
            (LOOK_AHEAD(ctx) == '}') ||
            (LOOK_AHEAD(ctx) == ']') ||
            (LOOK_AHEAD(ctx) == ',')) {
          break;
        }
        continue;
      }

      if (ch == '-') {
        if (!IS_EOF_AHEAD(ctx) && IS_DIGIT(LOOK_AHEAD(ctx))) {
          UPDATE_TOKEN(token, JESY_TOKEN_NUMBER, ctx->offset, 1);
          continue;
        }
        UPDATE_TOKEN(token, JESY_TOKEN_INVALID, ctx->offset, 1);
        break;
      }

      if (ch == 't') {
        UPDATE_TOKEN(token, JESY_TOKEN_TRUE, ctx->offset, 1);
        continue;
      }

      if (ch == 'f') {
        UPDATE_TOKEN(token, JESY_TOKEN_FALSE, ctx->offset, 1);
        continue;
      }

      if (ch == 'n') {
        UPDATE_TOKEN(token, JESY_TOKEN_NULL, ctx->offset, 1);
        continue;
      }

      /* Skipping space symbols including: space, tab, carriage return */
      if (IS_SPACE(ch)) {
        continue;
      }

      UPDATE_TOKEN(token, JESY_TOKEN_INVALID, ctx->offset, 1);
      break;
    }
    else if (token.type == JESY_TOKEN_STRING) {

      /* We'll not deliver '\"' symbol as a part of token. */
      if (ch == '\"') {
        break;
      }
      token.length++;
      continue;
    }
    else if (token.type == JESY_TOKEN_NUMBER) {

      if (IS_DIGIT(ch)) {
        token.length++;
        if (!IS_DIGIT(LOOK_AHEAD(ctx)) && LOOK_AHEAD(ctx) != '.') {
          break;
        }
        continue;
      }

      if (ch == '.') {
        token.length++;
        if (!IS_DIGIT(LOOK_AHEAD(ctx))) {
          token.type = JESY_TOKEN_INVALID;
          break;
        }
        continue;
      }

      if (IS_SPACE(ch)) {
        break;
      }

      token.type = JESY_TOKEN_INVALID;
      break;
    }
    else if (token.type == JESY_TOKEN_TRUE) {
      token.length++;
      if (token.length == (sizeof("true") - 1)) {
        if (0 != (strncmp(&ctx->json_data[token.offset], "true", (sizeof("true") - 1)))) {
          token.type = JESY_TOKEN_INVALID;
        }
        break;
      }
      continue;
    }
    else if (token.type == JESY_TOKEN_FALSE) {
      token.length++;
      if (token.length == (sizeof("false") - 1)) {
        if (0 != (strncmp(&ctx->json_data[token.offset], "false", (sizeof("false") - 1)))) {
          token.type = JESY_TOKEN_INVALID;
        }
        break;
      }
      continue;
    }
    else if (token.type == JESY_TOKEN_NULL) {
      token.length++;
      if (token.length == (sizeof("null") - 1)) {
        if (0 != (strncmp(&ctx->json_data[token.offset], "null", (sizeof("null") - 1)))) {
          token.type = JESY_TOKEN_INVALID;
        }
        break;
      }
      continue;
    }

    token.type = JESY_TOKEN_INVALID;
    break;
  }

  JESY_LOG_TOKEN(token.type, token.offset, token.length, &ctx->json_data[token.offset]);

  return token;
}

static struct jesy_element *jesy_find_duplicate_key(struct jesy_context *ctx,
                                                    struct jesy_element *object,
                                                    struct jesy_token *key_token)
{
  struct jesy_element *duplicate = NULL;
  struct jesy_element *iter = NULL;

  assert(object->type == JESY_OBJECT);
  if (object->type != JESY_OBJECT) {
    return NULL;
  }

  iter = HAS_CHILD(object) ? &ctx->pool[object->first_child] : NULL;
  while(iter) {
    assert(iter->type == JESY_KEY);
    if ((iter->length == key_token->length) &&
        (strncmp(iter->value, &ctx->json_data[key_token->offset], key_token->length) == 0)) {
      duplicate = iter;
      break;
    }
    iter = HAS_SIBLING(object) ? &ctx->pool[object->sibling] : NULL;
  }
  return duplicate;
}

/*
 *  Parser state machine steps
 *    1. Accept
 *    2. Append
 *    3. Iterate
 *    4. State transition
*/
static bool jesy_accept(struct jesy_context *ctx,
                        enum jesy_token_type token_type,
                        enum jesy_type element_type,
                        enum jesy_parser_state state)
{
  if (ctx->token.type == token_type) {
    struct jesy_element *new_node = NULL;
    //printf("\n     Parser State: %s", jesy_state_str[ctx->state]);
    if (element_type == JESY_KEY) {
#ifdef JESY_OVERWRITE_DUPLICATE_KEYS
      /* No duplicate keys in the same object are allowed.
         Only the last key:value will be reported if the keys are duplicated. */
      struct jesy_element *node = jesy_find_duplicate_key(ctx, ctx->iter, &ctx->token);
      if (node) {
        jesy_delete_element(ctx, jesy_get_child(ctx, node));
        ctx->iter = node;
      }
      else
#endif
      {
        new_node = jesy_add_element(ctx, ctx->iter, element_type, ctx->token.offset, ctx->token.length);
      }
    }
    else if ((element_type == JESY_OBJECT) ||
             (element_type == JESY_ARRAY)) {
      new_node = jesy_add_element(ctx, ctx->iter, element_type, ctx->token.offset, ctx->token.length);
    }
    else if (element_type == JESY_STRING) {
      new_node = jesy_add_element(ctx, ctx->iter, element_type, ctx->token.offset, ctx->token.length);
    }
    else if ((element_type == JESY_NUMBER)  ||
             (element_type == JESY_TRUE)    ||
             (element_type == JESY_FALSE)   ||
             (element_type == JESY_NULL)) {
      new_node = jesy_add_element(ctx, ctx->iter, element_type, ctx->token.offset, ctx->token.length);
    }
    else { /* JESY_NONE */
       /* None-Key/Value tokens trigger upward iteration to the parent node.
       A ']' indicates the end of an Array and consequently the end of a key:value
             pair. Go back to the parent node.
       A '}' indicates the end of an object. Go back to the parent node
       A ',' indicates the end of a value.
             if the value is a part of an array, go back parent array node.
             otherwise, go back to the parent object.
      */
      if (token_type == JESY_TOKEN_CLOSING_BRACE) {
        /* [] (empty array) is a special case that needs no iteration in the
           direction the parent node. */
        if (ctx->iter->type != JESY_ARRAY) {
          ctx->iter = jesy_get_parent_bytype(ctx, ctx->iter, JESY_ARRAY);
        }
        ctx->iter = jesy_get_structure_parent_node(ctx, ctx->iter);
      }
      else if (token_type == JESY_TOKEN_CLOSING_BRACKET) {
        /* {} (empty object)is a special case that needs no iteration in the
           direction the parent node. */
        if (ctx->iter->type != JESY_OBJECT) {
          ctx->iter = jesy_get_parent_bytype(ctx, ctx->iter, JESY_OBJECT);
        }
        ctx->iter = jesy_get_structure_parent_node(ctx, ctx->iter);
      }
      else if (token_type == JESY_TOKEN_COMMA) {
        if ((ctx->iter->type != JESY_OBJECT) &&
            (ctx->iter->type != JESY_ARRAY)) {
          ctx->iter = jesy_get_structure_parent_node(ctx, ctx->iter);
        }
      }
    }

    if (ctx->status) return true;
    if (new_node) {
      ctx->iter = new_node;
      JESY_LOG_NODE(ctx->iter - ctx->pool, ctx->iter->type,
                    ctx->iter->parent, ctx->iter->sibling, ctx->iter->first_child);
    }

    ctx->state = state;
  //printf("   --->     Parser State: %s", jesy_state_str[ctx->state]);
    ctx->token = jesy_get_token(ctx);
    return true;
  }

  return false;
}

static bool jesy_expect(struct jesy_context *ctx,
                        enum jesy_token_type token_type,
                        enum jesy_type element_type,
                        enum jesy_parser_state state)
{
  if (jesy_accept(ctx, token_type, element_type, state)) {
    return true;
  }
  if (!ctx->status) {
    ctx->status = JESY_UNEXPECTED_TOKEN; /* Keep the first error */
#ifndef NDEBUG
  printf("\nJES.Parser error! Unexpected Token. %s \"%.*s\"",
      jesy_token_type_str[ctx->token.type], ctx->token.length,
      &ctx->json_data[ctx->token.offset]);
  printf("     Parser State: %s", jesy_state_str[ctx->state]);
#endif
  }
  return false;
}

struct jesy_context* jesy_init_context(void *mem_pool, uint32_t pool_size)
{
  if (pool_size < sizeof(struct jesy_context)) {
    return NULL;
  }

  struct jesy_context *ctx = mem_pool;

  ctx->status = 0;
  ctx->node_count = 0;

  ctx->json_data = NULL;
  ctx->json_size = 0;
  ctx->offset = (uint32_t)-1;
  ctx->index = 0;
  ctx->pool = (struct jesy_element*)(ctx + 1);
  ctx->pool_size = pool_size - (uint32_t)(sizeof(struct jesy_context));
  ctx->capacity = (ctx->pool_size / sizeof(struct jesy_element)) < JESY_INVALID_INDEX
                 ? (jesy_node_descriptor)(ctx->pool_size / sizeof(struct jesy_element))
                 : JESY_INVALID_INDEX -1;

  ctx->iter = NULL;
  ctx->root = NULL;
  ctx->state = JESY_STATE_START;

#ifndef NDEBUG
  printf("\nallocator capacity is %d nodes", ctx->capacity);
#endif

  return ctx;
}

uint32_t jesy_parse(struct jesy_context *ctx, char *json_data, uint32_t json_length)
{
  ctx->json_data = json_data;
  ctx->json_size = json_length;
  /* Fetch the first token to before entering the state machine. */
  ctx->token = jesy_get_token(ctx);

  do {
    if (ctx->token.type == JESY_TOKEN_EOF) { break; }
    //if (ctx->iter)printf("\n    State: %s, node: %s", jesy_state_str[ctx->state], jesy_node_type_str[ctx->iter->type]);
    switch (ctx->state) {
      /* Only an opening bracket is acceptable in this state. */
      case JESY_STATE_START:
        jesy_expect(ctx, JESY_TOKEN_OPENING_BRACKET, JESY_OBJECT, JESY_STATE_WANT_KEY);
        break;

      /* An opening parenthesis has already been found.
         A closing bracket is allowed. Otherwise, only a KEY is acceptable. */
      case JESY_STATE_WANT_KEY:
        if (jesy_accept(ctx, JESY_TOKEN_CLOSING_BRACKET, JESY_NONE, JESY_STATE_STRUCTURE_END)) {
          break;
        }

        if (!jesy_expect(ctx, JESY_TOKEN_STRING, JESY_KEY, JESY_STATE_WANT_VALUE)) {
          break;
        }
        jesy_expect(ctx, JESY_TOKEN_COLON, JESY_NONE, JESY_STATE_WANT_VALUE);
        break;

      case JESY_STATE_WANT_VALUE:
        if (jesy_accept(ctx, JESY_TOKEN_STRING, JESY_STRING, JESY_STATE_PROPERTY_END)   ||
            jesy_accept(ctx, JESY_TOKEN_NUMBER, JESY_NUMBER, JESY_STATE_PROPERTY_END)   ||
            jesy_accept(ctx, JESY_TOKEN_TRUE, JESY_TRUE, JESY_STATE_PROPERTY_END)       ||
            jesy_accept(ctx, JESY_TOKEN_FALSE, JESY_FALSE, JESY_STATE_PROPERTY_END)     ||
            jesy_accept(ctx, JESY_TOKEN_NULL, JESY_NULL, JESY_STATE_PROPERTY_END)       ||
            jesy_accept(ctx, JESY_TOKEN_OPENING_BRACKET, JESY_OBJECT, JESY_STATE_WANT_KEY)) {
          break;
        }

        jesy_expect(ctx, JESY_TOKEN_OPENING_BRACE, JESY_ARRAY, JESY_STATE_WANT_ARRAY);
        break;

      case JESY_STATE_WANT_ARRAY:
        if (jesy_accept(ctx, JESY_TOKEN_STRING, JESY_STRING, JESY_STATE_VALUE_END)  ||
            jesy_accept(ctx, JESY_TOKEN_NUMBER, JESY_NUMBER, JESY_STATE_VALUE_END)  ||
            jesy_accept(ctx, JESY_TOKEN_TRUE, JESY_TRUE, JESY_STATE_VALUE_END)      ||
            jesy_accept(ctx, JESY_TOKEN_FALSE, JESY_FALSE, JESY_STATE_VALUE_END)    ||
            jesy_accept(ctx, JESY_TOKEN_NULL, JESY_NULL, JESY_STATE_VALUE_END)      ||
            jesy_accept(ctx, JESY_TOKEN_OPENING_BRACKET, JESY_OBJECT, JESY_STATE_WANT_KEY) ||
            jesy_accept(ctx, JESY_TOKEN_OPENING_BRACE, JESY_ARRAY, JESY_STATE_WANT_ARRAY)) {
          break;
        }

        jesy_expect(ctx, JESY_TOKEN_CLOSING_BRACE, JESY_NONE, JESY_STATE_STRUCTURE_END);
        break;
      /* A Structure can be an Object or an Array.
         When a structure is closed, another closing symbol is allowed.
         Otherwise, only a separator is acceptable. */
      case JESY_STATE_PROPERTY_END:
        if (jesy_accept(ctx, JESY_TOKEN_COMMA, JESY_NONE, JESY_STATE_WANT_KEY)) {
          continue;
        }

        jesy_expect(ctx, JESY_TOKEN_CLOSING_BRACKET, JESY_NONE, JESY_STATE_STRUCTURE_END);
        break;

      /* A Structure can be an Object or an Array.
         When a structure is closed, another closing symbol is allowed.
         Otherwise, only a separator is acceptable. */
      case JESY_STATE_VALUE_END:
        if (jesy_accept(ctx, JESY_TOKEN_COMMA, JESY_NONE, JESY_STATE_WANT_ARRAY)) {
          break;
        }

        jesy_expect(ctx, JESY_TOKEN_CLOSING_BRACE, JESY_NONE, JESY_STATE_STRUCTURE_END);
        break;
      /* A Structure can be an Object or an Array.
         When a structure is closed, another closing symbol is allowed.
         Otherwise, only a separator is acceptable. */
      case JESY_STATE_STRUCTURE_END:
        if (ctx->iter->type == JESY_OBJECT) {
          if (jesy_accept(ctx, JESY_TOKEN_CLOSING_BRACKET, JESY_NONE, JESY_STATE_STRUCTURE_END)) {
            break;
          }
          jesy_expect(ctx, JESY_TOKEN_COMMA, JESY_NONE, JESY_STATE_WANT_KEY);
        }
        else if(ctx->iter->type == JESY_ARRAY) {
          if (jesy_accept(ctx, JESY_TOKEN_CLOSING_BRACE, JESY_NONE, JESY_STATE_STRUCTURE_END)) {
            break;
          }
          jesy_expect(ctx, JESY_TOKEN_COMMA, JESY_NONE, JESY_STATE_WANT_ARRAY);
        }
        else {
          assert(0);
        }

        break;

      default:
        assert(0);
        break;
    }
  } while ((ctx->iter) && (ctx->status == 0));

  if (ctx->status == 0) {
    if (ctx->token.type != JESY_TOKEN_EOF) {
      ctx->status = JESY_UNEXPECTED_TOKEN;
    }
    else if (ctx->iter) {
      ctx->status = JESY_UNEXPECTED_EOF;
    }
  }

  ctx->iter = ctx->root;
  return ctx->status;
}

uint32_t jesy_get_dump_size(struct jesy_context *ctx)
{
  struct jesy_element *iter = ctx->root;
  uint32_t dump_size = 0;

  while (iter) {

    if (iter->type == JESY_OBJECT) {
      dump_size++; /* '{' */
    }
    else if (iter->type == JESY_KEY) {
      dump_size += (iter->length + sizeof(char) * 3);/* +1 for ':' +2 for "" */
    }
    else if (iter->type == JESY_STRING) {
      dump_size += (iter->length + sizeof(char) * 2);/* +2 for "" */
    }
    else if ((iter->type == JESY_NUMBER)  ||
             (iter->type == JESY_TRUE)    ||
             (iter->type == JESY_FALSE)   ||
             (iter->type == JESY_NULL)) {
      dump_size += iter->length;
    }
    else if (iter->type == JESY_ARRAY) {
      dump_size++; /* '[' */
    }
    else {
      assert(0);
      return 0;
    }

    if (HAS_CHILD(iter)) {
      iter = jesy_get_child(ctx, iter);
      continue;
    }

    if (iter->type == JESY_OBJECT) {
      dump_size++; /* '}' */
    }

    else if (iter->type == JESY_ARRAY) {
      dump_size++; /* ']' */
    }

    if (HAS_SIBLING(iter)) {
      iter = jesy_get_sibling(ctx, iter);
      dump_size++; /* ',' */
      continue;
    }

    while ((iter = jesy_get_parent(ctx, iter))) {
      if (iter->type == JESY_OBJECT) {
        dump_size++; /* '}' */
      }
      else if (iter->type == JESY_ARRAY) {
        dump_size++; /* ']' */
      }
      if (HAS_SIBLING(iter)) {
        iter = jesy_get_sibling(ctx, iter);
        dump_size++; /* ',' */
        break;
      }
    }
  }
  return dump_size;
}

uint32_t jesy_serialize(struct jesy_context *ctx, char *buffer, uint32_t length)
{
  char *dst = buffer;
  struct jesy_element *iter = ctx->root;
  uint32_t required_buffer = 0;

  required_buffer = jesy_get_dump_size(ctx);
  if (length < required_buffer) {
    ctx->status = JESY_OUT_OF_MEMORY;
    return 0;
  }
  if (required_buffer == 0) {
    return 0;
  }

  while (iter) {

    if (iter->type == JESY_OBJECT) {
      *dst++ = '{';
    }
    else if (iter->type == JESY_KEY) {
      *dst++ = '"';
      dst = (char*)memcpy(dst, iter->value, iter->length) + iter->length;
      *dst++ = '"';
      *dst++ = ':';
    }
    else if (iter->type == JESY_STRING) {
      *dst++ = '"';
      dst = (char*)memcpy(dst, iter->value, iter->length) + iter->length;
      *dst++ = '"';
    }
    else if ((iter->type == JESY_NUMBER)  ||
             (iter->type == JESY_TRUE)    ||
             (iter->type == JESY_FALSE)   ||
             (iter->type == JESY_NULL)) {
      dst = (char*)memcpy(dst, iter->value, iter->length) + iter->length;
    }
    else if (iter->type == JESY_ARRAY) {
      *dst++ = '[';
    }
    else {
      assert(0);
      ctx->status = JESY_UNEXPECTED_NODE;
      break;
    }

    if (HAS_CHILD(iter)) {
      iter = jesy_get_child(ctx, iter);
      continue;
    }

    if (iter->type == JESY_OBJECT) {
      *dst++ = '}';
    }

    else if (iter->type == JESY_ARRAY) {
      *dst++ = ']';
    }

    if (HAS_SIBLING(iter)) {
      iter = jesy_get_sibling(ctx, iter);
      *dst++ = ',';
      continue;
    }

     while ((iter = jesy_get_parent(ctx, iter))) {
      if (iter->type == JESY_OBJECT) {
        *dst++ = '}';
      }
      else if (iter->type == JESY_ARRAY) {
        *dst++ = ']';
      }
      if (HAS_SIBLING(iter)) {
        iter = jesy_get_sibling(ctx, iter);
        *dst++ = ',';
        break;
      }
    }
  }

  ctx->iter = ctx->root;
  return dst - buffer;
}

struct jesy_element* jesy_get_root(struct jesy_context *ctx)
{
  if (ctx) {
    return ctx->root;
  }
  return NULL;
}

void jesy_reset_iterator(struct jesy_context *ctx)
{
  ctx->iter = ctx->root;
}

struct jesy_element* jesy_get_key_value(struct jesy_context *ctx, struct jesy_element *object, char *key)
{
  struct jesy_element *iter = NULL;
  if (ctx && object && key && jesy_validate_element(ctx, object)) {
    if (object->type != JESY_OBJECT) {
      return NULL;
    }
    uint32_t key_len = (uint16_t)strnlen(key, 0xFFFF);

    iter = HAS_CHILD(object) ? &ctx->pool[object->first_child] : NULL;
    while (iter) {
      if ((iter->length == key_len) && (0 == strncmp(iter->value, key, key_len))) {
        iter = HAS_CHILD(iter) ? &ctx->pool[iter->first_child] : NULL;
        break;
      }
      iter = HAS_SIBLING(iter) ? &ctx->pool[iter->sibling] : NULL;
    }
  }
  return iter;
}

struct jesy_element* jesy_get_array_value(struct jesy_context *ctx, struct jesy_element *array, int16_t index)
{
  struct jesy_element *iter = NULL;
  if (ctx && array && jesy_validate_element(ctx, array)) {
    if (array->type != JESY_ARRAY) {
      return NULL;
    }

    if (index >= 0) {
      iter = HAS_CHILD(array) ? &ctx->pool[array->first_child] : NULL;
      for (; iter && index > 0; index--) {
        iter = iter = HAS_SIBLING(iter) ? &ctx->pool[iter->sibling] : NULL;
      }
    }
  }
  return iter;
}

bool jesy_set(struct jesy_context *ctx, char *key, char *value, uint16_t length)
{




}