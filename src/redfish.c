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
  unsigned int flags;
  char **queries;      /* List of queries */
  llist_t *query_ptrs; /* Pointers to query structs */
  size_t queries_num;
  enumeratorAuthentication auth;
  redfishService *redfish;
};
typedef struct redfish_service_s redfish_service_t;

struct redfish_ctx_s {
  llist_t *services;
  c_avl_tree_t *queries;
};
typedef struct redfish_ctx_s redfish_ctx_t;

redfish_ctx_t *ctx;

static int redfish_cleanup(void);
static int redfish_validate_config(void);

#if COLLECT_DEBUG
static void redfish_print_config(void) {
  DEBUG(PLUGIN_NAME ": ====================CONFIGURATION====================");
  DEBUG(PLUGIN_NAME ": SERVICES: %d", llist_size(ctx->services));
  for (llentry_t *le = llist_head(ctx->services); le != NULL; le = le->next) {
    redfish_service_t *s = (redfish_service_t *)le->value;
    char queries_str[MAX_STR_LEN];

    strjoin(queries_str, MAX_STR_LEN, s->queries, s->queries_num, ", ");

    DEBUG(PLUGIN_NAME ": --------------------");
    DEBUG(PLUGIN_NAME ": Service: %s", s->name);
    DEBUG(PLUGIN_NAME ":   Host: %s", s->host);

    if (s->user && s->passwd) {
      DEBUG(PLUGIN_NAME ":   User: %s", s->user);
      DEBUG(PLUGIN_NAME ":   Passwd: %s", s->passwd);
    } else if (s->token)
      DEBUG(PLUGIN_NAME ":   Token: %s", s->token);

    DEBUG(PLUGIN_NAME ":   Queries[%" PRIsz "]: (%s)", s->queries_num,
          queries_str);
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
      for (llentry_t *le = llist_head(r->properties); le != NULL;
           le = le->next) {
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

static int redfish_init(void) {
#if COLLECT_DEBUG
  redfish_print_config();
#endif
  int ret = redfish_validate_config();

  if (ret != 0)
    return ret;

  for (llentry_t *le = llist_head(ctx->services); le != NULL; le = le->next) {
    redfish_service_t *service = (redfish_service_t *)le->value;

    /* Preparing struct for authentication */
    if (service->user && service->passwd) {
      service->auth.authCodes.userPass.username = service->user;
      service->auth.authCodes.userPass.password = service->passwd;
      service->redfish = createServiceEnumerator(
          service->host, NULL, &service->auth, service->flags);
    } else if (service->token) {
      service->auth.authCodes.authToken.token = service->token;
      service->auth.authType = REDFISH_AUTH_BEARER_TOKEN;
      service->redfish = createServiceEnumerator(
          service->host, NULL, &service->auth, service->flags);
    } else {
      service->redfish =
          createServiceEnumerator(service->host, NULL, NULL, service->flags);
    }

    service->query_ptrs = llist_create();
    if (service->query_ptrs == NULL)
      goto error;

    /* Preparing query pointers list for every service */
    for (size_t i = 0; i < service->queries_num; i++) {
      redfish_query_t *ptr;
      if (c_avl_get(ctx->queries, service->queries[i], (void **)&ptr) != 0)
        goto error;

      llentry_t *entry = llentry_create(ptr->name, ptr);
      if (entry != NULL)
        llist_append(service->query_ptrs, entry);
      else
        goto error;
    }
  }

  return 0;

error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for service queries list");
  /* TODO: verify whether libredfish cleanup is needed */
  return -ENOMEM;
}

static int redfish_preconfig(void) {
  /* Allocating plugin context */
  ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    goto error;

  /* Creating placeholder for services */
  ctx->services = llist_create();
  if (ctx->services == NULL)
    goto free_ctx;

  /* Creating placeholder for queries */
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

static int redfish_config_property(redfish_resource_t *resource,
                                   oconfig_item_t *cfg_item) {
  assert(resource != NULL);
  assert(cfg_item != NULL);

  // is this really needed to be taken from heap?
  redfish_property_t *property = calloc(1, sizeof(*property));

  if (property == NULL)
    goto error;

  int ret = cf_util_get_string(cfg_item, &property->name);
  for (int i = 0; i < cfg_item->children_num; i++) {

    oconfig_item_t *opt = cfg_item->children + i;
    if (strcasecmp("PluginInstance", opt->key) == 0)
      ret = cf_util_get_string(opt, &property->plugin_inst);
    else if (strcasecmp("Type", opt->key) == 0)
      ret = cf_util_get_string(opt, &property->type);
    else if (strcasecmp("TypeInstance", opt->key) == 0)
      ret = cf_util_get_string(opt, &property->type_inst);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      sfree(property);
      return -EINVAL;
    }

    if (ret != 0)
      sfree(property);
    return ret;
  }

  llentry_t *entry = llentry_create(property->name, property);
  if (entry != NULL)
    llist_append(resource->properties, entry);
  else
    goto error;

  return 0;

error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for property");
  sfree(property);
  return -ENOMEM;
}

static int redfish_config_resource(redfish_query_t *query,
                                   oconfig_item_t *cfg_item) {
  assert(query != NULL);
  assert(cfg_item != NULL);

  redfish_resource_t *resource = calloc(1, sizeof(*resource));

  if (resource == NULL)
    goto error;

  resource->properties = llist_create();

  if (resource->properties == NULL)
    goto free_resource;

  int ret = cf_util_get_string(cfg_item, &resource->name);

  for (int i = 0; i < cfg_item->children_num; i++) {
    oconfig_item_t *opt = cfg_item->children + i;
    if (strcasecmp("Property", opt->key) == 0)
      ret = redfish_config_property(resource, opt);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      return -EINVAL;
    }

    if (ret != 0) {
      sfree(resource);
      return ret;
    }
  }

  llentry_t *entry = llentry_create(resource->name, resource);
  if (entry != NULL)
    llist_append(query->resources, entry);
  else
    goto free_resource;

  return 0;

free_resource:
  sfree(resource);
error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for resource");
  return -ENOMEM;
}

static int redfish_config_query(oconfig_item_t *cfg_item,
                                c_avl_tree_t *queries) {
  redfish_query_t *query = calloc(1, sizeof(*query));

  if (query == NULL)
    goto error;

  query->resources = llist_create();

  if (query->resources == NULL)
    goto free_query;

  int ret = cf_util_get_string(cfg_item, &query->name);
  // check ret value here

  for (int i = 0; i < cfg_item->children_num; i++) {
    oconfig_item_t *opt = cfg_item->children + i;

    if (strcasecmp("Endpoint", opt->key) == 0)
      ret = cf_util_get_string(opt, &query->endpoint);
    else if (strcasecmp("Resource", opt->key) == 0)
      ret = redfish_config_resource(query, opt);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      sfree(query);
      return -EINVAL;
    }

    if (ret != 0) {
      sfree(query);
      return ret;
    }
  }

  ret = c_avl_insert(queries, query->name, query);

  if (ret != 0)
    goto free_query;

  return 0;

free_query:
  sfree(query);
error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for query");
  return -ENOMEM;
}

static int redfish_read_queries(oconfig_item_t *cfg_item, char ***queries_ptr) {
  size_t q_num = cfg_item->values_num;

  *queries_ptr = calloc(1, sizeof(**queries_ptr) * q_num);
  if (*queries_ptr == NULL)
    goto error;

  char **queries = *queries_ptr;
  for (size_t i = 0; i < q_num; i++) {
    if (cfg_item->values[i].type != OCONFIG_TYPE_STRING) {
      ERROR(PLUGIN_NAME ": 'Queries' requires string arguments");
      sfree(queries_ptr);
      return -EINVAL;
    }
    // TODO: ptrs from strdup also need to be freed
    queries[i] = strdup(cfg_item->values[i].value.string);

    if (queries[i] == NULL)
      goto error;
  }

  return 0;

error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for queries list");
  sfree(queries_ptr);
  return -ENOMEM;
}

static int redfish_config_service(oconfig_item_t *cfg_item) {
  redfish_service_t *service = calloc(1, sizeof(*service));

  if (service == NULL)
    goto error;

  int ret = cf_util_get_string(cfg_item, &service->name);

  for (int i = 0; i < cfg_item->children_num; i++) {
    oconfig_item_t *opt = cfg_item->children + i;

    if (strcasecmp("Host", opt->key) == 0)
      ret = cf_util_get_string(opt, &service->host);
    else if (strcasecmp("User", opt->key) == 0)
      ret = cf_util_get_string(opt, &service->user);
    else if (strcasecmp("Passwd", opt->key) == 0)
      ret = cf_util_get_string(opt, &service->passwd);
    else if (strcasecmp("Token", opt->key) == 0)
      ret = cf_util_get_string(opt, &service->token);
    else if (strcasecmp("Queries", opt->key) == 0) {
      ret = redfish_read_queries(opt, &service->queries);
      service->queries_num = opt->values_num;
    } else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", opt->key);
      sfree(service);
      return -EINVAL;
    }

    if (ret != 0) {
      sfree(service);
      return ret;
    }
  }

  llentry_t *entry = llentry_create(service->name, service);
  if (entry != NULL)
    llist_append(ctx->services, entry);
  else
    goto free_service;

  return 0;

free_service:
  sfree(service);
error:
  ERROR(PLUGIN_NAME ": Failed to allocate memory for service");
  return -ENOMEM;
}

static int redfish_config(oconfig_item_t *cfg_item) {
  int ret = redfish_preconfig();

  if (ret != 0)
    return ret;

  for (int i = 0; i < cfg_item->children_num; i++) {
    oconfig_item_t *child = cfg_item->children + i;

    if (strcasecmp("Query", child->key) == 0)
      ret = redfish_config_query(child, ctx->queries);
    else if (strcasecmp("Service", child->key) == 0)
      ret = redfish_config_service(child);
    else {
      ERROR(PLUGIN_NAME ": Invalid configuration option \"%s\".", child->key);
      return -EINVAL;
    }

    if (ret != 0) {
      redfish_cleanup();
      return ret;
    }
  }

  return 0;
}

static int redfish_validate_config(void) {

  /*TODO*/

  return 0;
}

void redfish_process_payload(bool success, unsigned short http_code,
                             redfishPayload *payload, void *context) {
  if (success == false) {
    DEBUG(PLUGIN_NAME ": Query has failed, HTTP code = %u\n", http_code);
  }

  if (payload) {

    /*TODO*/
  }
}

static int redfish_read(__attribute__((unused)) user_data_t *ud) {
  for (llentry_t *le = llist_head(ctx->services); le != NULL; le = le->next) {
    redfish_service_t *service = (redfish_service_t *)le->value;

    for (llentry_t *le = llist_head(service->query_ptrs); le != NULL;
         le = le->next) {
      redfish_query_t *query = (redfish_query_t *)le->value;

      getPayloadByPathAsync(service->redfish, query->endpoint, NULL,
                            redfish_process_payload, NULL);
    }
  }

  return 0;
}

static int redfish_cleanup(void) {
  DEBUG(PLUGIN_NAME ": cleanup");
  DEBUG(PLUGIN_NAME ": ");
  DEBUG(PLUGIN_NAME ": ");
  DEBUG(PLUGIN_NAME ": ");
  DEBUG(PLUGIN_NAME ": ");
  DEBUG(PLUGIN_NAME ": ");

  for (llentry_t *le = llist_head(ctx->services); le; le = le->next) {
    redfish_service_t *service = (redfish_service_t *)le->value;

    cleanupServiceEnumerator(service->redfish);
    for (size_t i = 0; i < service->queries_num; i++)
      sfree(service->queries[i]);

    llist_destroy(service->query_ptrs);

    sfree(service->name);
    sfree(service->host);
    sfree(service->user);
    sfree(service->passwd);
    sfree(service->token);
    sfree(service->queries);
    sfree(service);
  }
  llist_destroy(ctx->services);

  c_avl_iterator_t *i = c_avl_get_iterator(ctx->queries);

  char *key;
  redfish_query_t *query;

  while (c_avl_iterator_next(i, (void **)&key, (void **)&query) == 0) {
    for (llentry_t *le = llist_head(query->resources); le != NULL;
         le = le->next) {
      redfish_resource_t *resource = (redfish_resource_t *)le->value;
      for (llentry_t *le = llist_head(resource->properties); le != NULL;
           le = le->next) {
        redfish_property_t *property = (redfish_property_t *)le->value;
        sfree(property->name);
        sfree(property->plugin_inst);
        sfree(property->type);
        sfree(property->type_inst);
      }
      sfree(resource->name);
    }
    sfree(query->name);
    sfree(query->endpoint);
    sfree(query);
  }

  c_avl_iterator_destroy(i);
  c_avl_destroy(ctx->queries);
  sfree(ctx);

  return 0;
}

void module_register(void) {
  plugin_register_init(PLUGIN_NAME, redfish_init);
  plugin_register_complex_config(PLUGIN_NAME, redfish_config);
  plugin_register_complex_read(NULL, PLUGIN_NAME, redfish_read, 0, NULL);
  plugin_register_shutdown(PLUGIN_NAME, redfish_cleanup);
}
