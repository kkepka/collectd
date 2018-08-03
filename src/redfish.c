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
#define MAX_STR_LEN 128

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

static int redfish_plugin_cleanup(void);

#if COLLECT_DEBUG
static void redfish_plugin_print_config(void) {
  DEBUG(PLUGIN_NAME ": ====================CONFIGURATION====================");
  DEBUG(PLUGIN_NAME ": SERVICES: %d", llist_size(ctx->services));
  for (llentry_t *le = llist_head(ctx->services); le != NULL; le = le->next) {
    redfish_service_t *s = (redfish_service_t *)le->value;
    char queries_str[MAX_STR_LEN];

    strjoin(queries_str, MAX_STR_LEN, s->queries, s->queries_num, ", ");

  DEBUG(PLUGIN_NAME ": --------------------");
  DEBUG(PLUGIN_NAME ": Service: %s", s->name);
  DEBUG(PLUGIN_NAME ":   Host: %s", s->host);
  DEBUG(PLUGIN_NAME ":   User: %s", s->user);
  DEBUG(PLUGIN_NAME ":   Passwd: %s", s->passwd);
  DEBUG(PLUGIN_NAME ":   Queries[%" PRIsz "]: (%s)", s->queries_num, queries_str);
  }

  DEBUG(PLUGIN_NAME ": =====================================================");
  
  c_avl_iterator_t *i = c_avl_get_iterator(ctx->queries);
  char *key;
  redfish_query_t *q;
  
  DEBUG(PLUGIN_NAME ": QUERIES: %d", c_avl_size(ctx->queries));

  while (c_avl_iterator_next(i, (void **)&key, (void **)&q) == 0) {
  DEBUG(PLUGIN_NAME ": --------------------");
  DEBUG(PLUGIN_NAME ": Query: %s", q->name);
  DEBUG(PLUGIN_NAME ":   Endpoint: %s", q->endpoint);
    for (llentry_t *le = llist_head(q->resources); le != NULL; le = le->next) {
      redfish_resource_t *r = (redfish_resource_t *)le->value;
  DEBUG(PLUGIN_NAME ":   Resource: %s", r->name);
      for (llentry_t *le = llist_head(r->properties); le != NULL; le = le->next) {
        redfish_property_t *p = (redfish_property_t *)le->value;
  DEBUG(PLUGIN_NAME ":     Property: %s", p->name);
  DEBUG(PLUGIN_NAME ":       PluginInstance: %s", p->plugin_inst);
  DEBUG(PLUGIN_NAME ":       Type: %s", p->type);
  DEBUG(PLUGIN_NAME ":       TypeInstance: %s", p->type_inst);
      }
    }
  }

  c_avl_iterator_destroy(i);
  DEBUG(PLUGIN_NAME ": =====================================================");

}
#endif

static int redfish_plugin_init(void) {
#if COLLECT_DEBUG
  redfish_plugin_print_config();
#endif

  for (llentry_t *le = llist_head(ctx->services); le != NULL; le = le->next) {
    redfish_service_t *s = (redfish_service_t *)le->value;

    s->query_ptrs = llist_create();
    if (s->query_ptrs == NULL)
      goto error;

    for (size_t i = 0; i < s->queries_num; i++) {
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

  r->properties = llist_create();
 
  if (r->properties == NULL)
    goto free_r;
 
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

free_r:
  sfree(r);
error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for resource");
  return -ENOMEM;
}

static int redfish_plugin_config_query(oconfig_item_t *ci, c_avl_tree_t *queries) {
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

  ret = c_avl_insert(queries, q->name, q);

  if (ret != 0)
    goto free_q;

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
  for (size_t i = 0; i < q_num; i++) {
    if (ci->values[i].type != OCONFIG_TYPE_STRING) {
      ERROR(PLUGIN_NAME ": 'Queries' requires string arguments");
      return -EINVAL;
    }
    queries[i] = strdup(ci->values[i].value.string);

    if (queries[i] == NULL)
      goto error;
  }

  return 0;

error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for queries list");
  return -ENOMEM;
}

static int redfish_plugin_config_service(oconfig_item_t *ci) {
  redfish_service_t *s = calloc(1, sizeof(*s));

  if (s == NULL)
    goto error;

  int ret = cf_util_get_string(ci, &s->name);

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *opt = ci->children + i;

    if (strcasecmp("Host", opt->key) == 0)
      ret = cf_util_get_string(opt, &s->host);
    else if (strcasecmp("User", opt->key) == 0)
      ret = cf_util_get_string(opt, &s->user);
    else if (strcasecmp("Passwd", opt->key) == 0)
      ret = cf_util_get_string(opt, &s->passwd);
    else if (strcasecmp("Token", opt->key) == 0)
      ret = cf_util_get_string(opt, &s->token);
    else if (strcasecmp("Queries", opt->key) == 0) {
      ret = redfish_plugin_read_queries(opt, &s->queries);
      s->queries_num = opt->values_num;
    } else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      return -EINVAL;
    }

    if (ret != 0)
      return ret;
  }

  llentry_t *entry = llentry_create(s->name, s);
  if (entry != NULL)
    llist_append(ctx->services, entry);
  else
    goto free_s;

  return 0;

free_s:
  sfree(s);
error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for service");
  return -ENOMEM;
}

static int redfish_plugin_config(oconfig_item_t *ci) {
  int ret = redfish_plugin_preconfig();

  if (ret != 0)
    return ret;

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Query", child->key) == 0)
      ret = redfish_plugin_config_query(child, ctx->queries);
    else if (strcasecmp("Service", child->key) == 0)
      ret = redfish_plugin_config_service(child);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", child->key);
      return -EINVAL;
    }
    
    if (ret != 0) {
      redfish_plugin_cleanup();
      return ret;
    }
  }

  return 0;
}

static int redfish_plugin_read(__attribute__((unused)) user_data_t *ud) {
  return 0;
}

static int redfish_plugin_cleanup(void) {
  DEBUG(PLUGIN_NAME ": cleanup");
  DEBUG(PLUGIN_NAME ": ");
  DEBUG(PLUGIN_NAME ": ");
  DEBUG(PLUGIN_NAME ": ");
  DEBUG(PLUGIN_NAME ": ");
  DEBUG(PLUGIN_NAME ": ");

  for (llentry_t *le = llist_head(ctx->services); le != NULL; le = le->next) {
    redfish_service_t *s = (redfish_service_t *)le->value;

    for (size_t i = 0; i < s->queries_num; i++)
      sfree(s->queries[i]);

    llist_destroy(s->query_ptrs);

    sfree(s->name);
    sfree(s->host);
    sfree(s->user);
    sfree(s->passwd);
    sfree(s->token);
    sfree(s->queries);
    sfree(s);
  }
  llist_destroy(ctx->services);

  c_avl_iterator_t *i = c_avl_get_iterator(ctx->queries);
  
  char *key;
  redfish_query_t *q;

  while (c_avl_iterator_next(i, (void **)&key, (void **)&q) == 0) {
    for (llentry_t *le = llist_head(q->resources); le != NULL; le = le->next) {
      redfish_resource_t *r = (redfish_resource_t *)le->value;
      for (llentry_t *le = llist_head(r->properties); le != NULL; le = le->next) {
        redfish_property_t *p = (redfish_property_t *)le->value;
        sfree(p->name);
        sfree(p->plugin_inst);
        sfree(p->type);
        sfree(p->type_inst);
      }
      sfree(r->name);
    }
    sfree(q->name);    
    sfree(q->endpoint);
    sfree(q);
  }

  c_avl_iterator_destroy(i);
  c_avl_destroy(ctx->queries);
  sfree(ctx);

  return 0;
}

void module_register(void) {
  plugin_register_init(PLUGIN_NAME, redfish_plugin_init);
  plugin_register_complex_config(PLUGIN_NAME, redfish_plugin_config);
  plugin_register_complex_read(NULL, PLUGIN_NAME, redfish_plugin_read, 0, NULL);
  plugin_register_shutdown(PLUGIN_NAME, redfish_plugin_cleanup);
}
