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
#include <redfish.h>

#define PLUGIN_NAME "redfish"

static int redfish_plugin_init(void) {
  return 0;
}

static int redfish_plugin_config(oconfig_item_t *ci) {
  return 0;
}

static int redfish_plugin_read(__attribute__((unused)) user_data_t *ud) {
  return 0;
}

  static int redfish_plugin_shutdown(void) {
  return 0;
}

void module_register(void) {
  plugin_register_init(PLUGIN_NAME, redfish_plugin_init);
  plugin_register_complex_config(PLUGIN_NAME, redfish_plugin_config);
  plugin_register_complex_read(NULL, PLUGIN_NAME, redfish_plugin_read, 0,
                               NULL);
  plugin_register_shutdown(PLUGIN_NAME, redfish_plugin_shutdown);
}
