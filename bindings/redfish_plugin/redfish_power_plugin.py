#!/usr/bin/env python
import collectd
import traceback
import base_redfish


class RedfishPowerPlugin(base_redfish.BaseRedfish):

    def __init__(self):
        super(RedfishPowerPlugin, self).__init__()

    def get_stats(self):
        data = self.get_sensor_data("power", "Voltages", "Name", "voltage", "ReadingVolts", "Power/Voltages")
        return data

try:
    plugin = RedfishPowerPlugin()
except Exception as exc:
    collectd.error("redfish-power: failed to initialize redfish-power plugin :: %s :: %s"
            % (exc, traceback.format_exc()))


def configure_callback(conf):
    """Received configuration information"""
    plugin.config_callback(conf)


def read_callback():
    """Callback triggerred by collectd on read"""
    plugin.read_callback()

collectd.register_config(configure_callback)
collectd.register_read(read_callback, plugin.interval)
