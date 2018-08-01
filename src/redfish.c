/**
 * collectd - src/redfish.c
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Marcin Mozejko <marcinx.mozejko@intel.com>
 **/

#include "collectd.h"

#include "common.h"
#include "utils_avltree.h"
#include "utils_llist.h"

#include <redfish.h>

#define PLUGIN_NAME "redfish"

struct redfish_property_s {
  char *name;
  char *plugin_inst;
  char *type;
  char *type_inst;
};
typedef struct redfish_property_s redfish_property_t;

struct redfish_resource_s {
  char *name;
  llist_t *properties;
};
typedef struct redfish_resource_s redfish_resource_t;

struct redfish_query_s {
  char *name;
  char *endpoint;
  llist_t *resources;
};
typedef struct redfish_query_s redfish_query_t;

struct redfish_service_s {
  char *name;
  char *host;
  char *user;
  char *passwd;
  char *token;
  char **queries;
  llist_t *query_ptrs;
  size_t queries_num;
};
typedef struct redfish_service_s redfish_service_t;

struct redfish_ctx_s {
  llist_t *services;
  c_avl_tree_t *queries;
};
typedef struct redfish_ctx_s redfish_ctx_t;

redfish_ctx_t *ctx;

static int redfish_plugin_init(void) {

  for (llentry_t *le = llist_head(ctx->services); le != NULL; le = le->next) {
    redfish_service_t *s = (redfish_service_t *)le->value;

    s->query_ptrs = llist_create();
    if (s->query_ptrs == NULL)
      goto error;

    for (int i = 0; i < s->queries_num; i++) {
      redfish_query_t *ptr;
      if (c_avl_get(ctx->queries, s->queries[i], (void **)&ptr) != 0)
        return -1;

      llentry_t *e = llentry_create(ptr->name, ptr);
      if (e != NULL)
        llist_append(s->query_ptrs, e);
      else
        goto error;
    }
  }
  return 0;

error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for service queries list");
  return -ENOMEM;
}

static int redfish_plugin_preconfig(void) {

  ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    goto error;

  ctx->services = llist_create();
  if (ctx->services == NULL)
    goto free_ctx;

  ctx->queries = c_avl_create((int (*)(const void *, const void *))strcmp);
  if (ctx->services == NULL)
    goto free_services;

  return 0;

free_services:
  llist_destroy(ctx->services);
free_ctx:
  sfree(ctx);
error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for plugin context");
  return -ENOMEM;
}

static int redfish_plugin_config_property(redfish_resource_t *r,
                                          oconfig_item_t *ci) {
  assert(r != NULL);
  assert(ci != NULL);

  redfish_property_t *p = calloc(1, sizeof(*p));

  if (p == NULL)
    goto error;

  int ret = cf_util_get_string(ci, &p->name);
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *opt = ci->children + i;
    if (strcasecmp("PluginInstance", opt->key) == 0)
      ret = cf_util_get_string(opt, &p->plugin_inst);
    else if (strcasecmp("Type", opt->key) == 0)
      ret = cf_util_get_string(opt, &p->type);
    else if (strcasecmp("TypeInstance", opt->key) == 0)
      ret = cf_util_get_string(opt, &p->type_inst);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      return -EINVAL;
    }

    if (ret != 0)
      return ret;
  }

  llentry_t *entry = llentry_create(p->name, p);
  if (entry != NULL)
    llist_append(r->properties, entry);
  else
    goto error;

  return 0;

error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for property");
  return -ENOMEM;
}

static int redfish_plugin_config_resource(redfish_query_t *q,
                                          oconfig_item_t *ci) {
  assert(q != NULL);
  assert(ci != NULL);

  redfish_resource_t *r = calloc(1, sizeof(*r));

  if (r == NULL)
    goto error;

  int ret = cf_util_get_string(ci, &r->name);

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *opt = ci->children + i;
    if (strcasecmp("Property", opt->key) == 0)
      ret = redfish_plugin_config_property(r, opt);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      return -EINVAL;
    }

    if (ret != 0)
      return ret;
  }

  llentry_t *entry = llentry_create(r->name, r);
  if (entry != NULL)
    llist_append(q->resources, entry);
  else
    goto error;

  return 0;

error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for resource");
  return -ENOMEM;
}

static int redfish_plugin_config_query(oconfig_item_t *ci) {
  redfish_query_t *q = calloc(1, sizeof(*q));

  if (q == NULL)
    goto error;

  q->resources = llist_create();

  if (q->resources == NULL)
    goto free_q;

  int ret = cf_util_get_string(ci, &q->name);

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *opt = ci->children + i;

    if (strcasecmp("Endpoint", opt->key) == 0)
      ret = cf_util_get_string(opt, &q->endpoint);
    else if (strcasecmp("Resource", opt->key) == 0)
      ret = redfish_plugin_config_resource(q, opt);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      return -EINVAL;
    }

    if (ret != 0)
      return ret;
  }

  return 0;

free_q:
  sfree(q);
error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for query");
  return -ENOMEM;
}

static int redfish_plugin_read_queries(oconfig_item_t *ci,
                                       char ***queries_ptr) {
  size_t q_num = ci->values_num;

  *queries_ptr = calloc(1, sizeof(**queries_ptr) * q_num);
  if (*queries_ptr == NULL)
    goto error;

  char **queries = *queries_ptr;

  for (int i = 0; i < q_num; i++) {
    if (ci->values[i].type != OCONFIG_TYPE_STRING) {
      ERROR(PLUGIN_NAME ": 'Queries' requires string arguments");
      return -EINVAL;
    }
    char *query = queries[i];
    query = strdup(ci->values[i].value.string);

    if (query == NULL)
      goto error;
  }

  return 0;

error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for queries list");
  return -ENOMEM;
}

static int redfish_plugin_config_service(oconfig_item_t *ci) {
  redfish_service_t *s = calloc(1, sizeof(*s));

  if (s == NULL) {
    ERROR(PLUGIN_NAME ": Failed to allocate memory for service");
    return -ENOMEM;
  }

  int ret = cf_util_get_string(ci, &s->name);

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *opt = ci->children + i;

    if (strcasecmp("Host", opt->key) == 0)
      ret = cf_util_get_string(opt, &s->host);
    else if (strcasecmp("User", opt->key) == 0)
      ret = cf_util_get_string(opt, &s->host);
    else if (strcasecmp("Passwd", opt->key) == 0)
      ret = cf_util_get_string(opt, &s->host);
    else if (strcasecmp("Token", opt->key) == 0)
      ret = cf_util_get_string(opt, &s->host);
    else if (strcasecmp("Queries", opt->key) == 0)
      ret = redfish_plugin_read_queries(opt, &s->queries);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      return -EINVAL;
    }

    if (ret != 0)
      return ret;
  }

  return 0;
}

static int redfish_plugin_config(oconfig_item_t *ci) {
  int ret = redfish_plugin_preconfig();

  if (ret != 0)
    return ret;

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Query", child->key) == 0)
      ret = redfish_plugin_config_query(child);
    else if (strcasecmp("Service", child->key) == 0)
      ret = redfish_plugin_config_service(child);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", child->key);
      return -EINVAL;
    }
  }
  return 0;
}

static int redfish_plugin_read(__attribute__((unused)) user_data_t *ud) {
  return 0;
}

static int redfish_plugin_shutdown(void) {

  for (llentry_t *le = llist_head(ctx->services); le != NULL; le = le->next) {
    redfish_service_t *s = (redfish_service_t *)le->value;

    for (int i = 0; i < s->queries_num; i++)
      sfree(s->queries[i]);

    llist_destroy(s->query_ptrs);

    sfree(s->name);
    sfree(s->host);
    sfree(s->user);
    sfree(s->passwd);
    sfree(s->token);
    sfree(s->queries);
  }
  llist_destroy(ctx->services);

  sfree(ctx);

  return 0;
}

void module_register(void) {
  plugin_register_init(PLUGIN_NAME, redfish_plugin_init);
  plugin_register_complex_config(PLUGIN_NAME, redfish_plugin_config);
  plugin_register_complex_read(NULL, PLUGIN_NAME, redfish_plugin_read, 0, NULL);
  plugin_register_shutdown(PLUGIN_NAME, redfish_plugin_shutdown);
}
