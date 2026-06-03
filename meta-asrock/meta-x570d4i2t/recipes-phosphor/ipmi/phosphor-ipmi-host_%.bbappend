# Enable dynamic sensor discovery so voltage/temperature/fan sensors published
# by adcsensor, hwmontempsensor, and externalsensor appear in the IPMI SDR
# without needing a static sensor.yaml map.
PACKAGECONFIG:append = " dynamic-sensors"
