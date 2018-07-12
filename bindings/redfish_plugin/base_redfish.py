#!/usr/bin/env python

import collectd
import datetime
import requests
import traceback
import json


class BaseRedfish(object):

    def __init__(self):
        self.verbose = False
        self.debug = False
        self.prefix = 'redfish'
        self.interval = 60.0

    def config_callback(self, conf):
        """Takes a collectd conf object and fills in the local config."""
        for node in conf.children:
            if node.key == "Prefix":
                self.prefix = node.values[0]
            elif node.key == "Endpoint":
                self.endpoint = node.values[0]
            elif node.key == "User":
                self.user = node.values[0]
            elif node.key == "Password":
                self.password = node.values[0]
            elif node.key == "Hostname":
                self.hostname = node.values[0]
            elif node.key == "Verbose":
                if node.values[0] in ['True', 'true']:
                    self.verbose = True
            elif node.key == "Debug":
                if node.values[0] in ['True', 'true']:
                    self.debug = True
            elif node.key == 'Interval':
                self.interval = float(node.values[0])
            else:
                collectd.warning("%s: unknown config key: %s" % (self.prefix, node.key))

    def dispatch(self, stats):
        """
        Dispatches the given stats.

        stats should be something like:

        {'plugin': {'plugin_instance': {'type': {'type_instance': <value>, ...}}}}
        """
        if not stats:
            collectd.error("%s: failed to retrieve stats" % self.prefix)
            return

        self.logdebug("dispatching %d new stats :: %s" % (len(stats), stats))
        try:
            for plugin in stats.keys():
                for hostname in stats[plugin].keys():
                    for plugin_instance in stats[plugin][hostname].keys():
                        for sensor_type in stats[plugin][hostname][plugin_instance].keys():
                            type_value = stats[plugin][hostname][plugin_instance][sensor_type]
                            if not isinstance(type_value, dict):
                                self.dispatch_value(plugin, plugin_instance, sensor_type, None, type_value, hostname)
                            else:
                                for type_instance in stats[plugin][hostname][plugin_instance][sensor_type].keys():
                                    self.dispatch_value(plugin, plugin_instance,
                                                        sensor_type, type_instance,
                                                        stats[plugin][hostname][plugin_instance][sensor_type][type_instance],
                                                        hostname)
        except Exception as exc:
            collectd.error("%s: failed to dispatch values :: %s :: %s"
                           % (self.prefix, exc, traceback.format_exc()))

    def dispatch_value(self, plugin, plugin_instance, type_instance, data_type, value, hostname):
        """Looks for the given stat in stats, and dispatches it"""
        self.logdebug("dispatching value %s.%s.%s.%s=%s"
                      % (plugin, plugin_instance, data_type, type_instance, value))

        val = collectd.Values(type=data_type)
        val.plugin = plugin
        val.type_instance = type_instance
        val.values = [value]
        val.interval = self.interval
        val.host = hostname
        val.dispatch()
        self.logdebug("sent metric %s.%s.%s.%s.%s"
                      % (plugin, plugin_instance, data_type, type_instance, value))

    def read_callback(self):
        try:
            start = datetime.datetime.now()
            stats = self.get_stats()
            self.logverbose("collectd new data from service :: took %d seconds"
                            % (datetime.datetime.now() - start).seconds)
            self.dispatch(stats)
        except Exception as exc:
            collectd.error("%s: failed to get stats :: %s :: %s" % (self.prefix, exc, traceback.format_exc()))

    def get_stats(self):
        collectd.error('Not implemented, should be subclassed')
    """
    @staticmethod
    def endpoint_url_formatter(address, resource_url_path):
        protocol = "https"
        redfish_api = "redfish/v1/Chassis/1"
        endpoint = "{0}://{1}/{2}/{3}".format(protocol, address, redfish_api, resource_url_path)
        return endpoint
    """

    def get_sensor_data(self, plugin_instance, sensor_type, name, data_type, data_field, resource_url_path):
        """"                 ("fan", "Fans", "FanName", "fanspeed", "ReadingRPM")"""
        data = {self.prefix: {}}
        resp = requests.get(self.endpoint, verify=False, auth=(self.user, self.password))
        if resp.ok:
            resp.raise_for_status()
            jresp = resp.json()
            data[self.prefix][self.hostname[0]] = {}
            for sensor in jresp[sensor_type]:
                sensor_name = sensor[name]
                data[self.prefix][self.hostname[0]][plugin_instance] = {}
                data[self.prefix][self.hostname[0]][plugin_instance][sensor_name] = {}
                data[self.prefix][self.hostname[0]][plugin_instance][sensor_name][data_type] = sensor[data_field]

        else:
            collectd.error("%s: failed to get sensor data" % self.prefix)
        return data

    def logverbose(self, msg):
        if self.verbose:
            collectd.info("%s: %s" % (self.prefix, msg))

    def logdebug(self, msg):
        if self.debug:
            collectd.info("%s: %s" % (self.prefix, msg))
